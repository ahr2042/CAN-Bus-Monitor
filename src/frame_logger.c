/* SPDX-License-Identifier: GPL-3.0-or-later
 * frame_logger.c -- CAN frame logging with rotation and flush thread
 * Copyright (C) 2026  Ahmad Rashed
 */
#define _POSIX_C_SOURCE 200809L
#include "frame_logger.h"
#include "ring_buffer.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <linux/can.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define LOG_RING_CAPACITY  4096u
#define LOG_LINE_MAX       128u

typedef struct {
    char   data[LOG_LINE_MAX];
    size_t len;
} log_entry_t;

struct frame_logger {
    frame_logger_cfg_t cfg;
    ring_buffer_t     *ring;
    FILE              *fp;
    size_t             current_bytes;
    unsigned int       seq;
    char             **history;
    unsigned int       history_head;
    pthread_t          flush_thread;
    _Atomic(bool)      flush_stop;
    pthread_mutex_t    file_mutex;
};

static void build_log_path(const frame_logger_t *l, char *out, size_t sz)
{
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    snprintf(out, sz, "%s/%s_%04d%02d%02d_%02d%02d%02d_%04u.asc",
             l->cfg.base_path, l->cfg.prefix,
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
             lt.tm_hour, lt.tm_min, lt.tm_sec,
             l->seq);
}

static int open_new_log_file(frame_logger_t *l)
{
    char path[PATH_MAX];
    build_log_path(l, path, sizeof(path));
    l->fp = fopen(path, "w");
    if (!l->fp) return -errno;

    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    char tbuf[64];
    asctime_r(&lt, tbuf);
    fprintf(l->fp,
            "date %s"
            "base hex  timestamps absolute\n"
            "no internal events logged\n"
            "// canbus_monitor v1.0.0\n",
            tbuf);

    l->current_bytes = (size_t)ftell(l->fp);

    if (l->cfg.max_rotations > 0u) {
        free(l->history[l->history_head]);
        l->history[l->history_head] = strdup(path);
        unsigned int oldest = (l->history_head + 1u) % l->cfg.max_rotations;
        if (l->history[oldest]) {
            (void)unlink(l->history[oldest]);
            free(l->history[oldest]);
            l->history[oldest] = NULL;
        }
        l->history_head = (l->history_head + 1u) % l->cfg.max_rotations;
    }
    l->seq++;
    return 0;
}

static int flush_ring(frame_logger_t *l)
{
    log_entry_t entry;
    int errors = 0;
    pthread_mutex_lock(&l->file_mutex);
    while (rb_pop(l->ring, &entry)) {
        if (!l->fp) {
            if (open_new_log_file(l) != 0) { errors++; continue; }
        }
        size_t w = fwrite(entry.data, 1u, entry.len, l->fp);
        if (w != entry.len) { errors++; continue; }
        l->current_bytes += w;
        if (l->current_bytes >= l->cfg.max_file_bytes) {
            fclose(l->fp); l->fp = NULL; l->current_bytes = 0u;
            if (open_new_log_file(l) != 0) errors++;
        }
    }
    if (l->fp) fflush(l->fp);
    pthread_mutex_unlock(&l->file_mutex);
    return errors > 0 ? -EIO : 0;
}

static void *flush_thread_fn(void *arg)
{
    frame_logger_t *l = (frame_logger_t *)arg;
    struct timespec ts = {
        .tv_sec  = (time_t)(l->cfg.flush_interval_ms / 1000u),
        .tv_nsec = (long)((l->cfg.flush_interval_ms % 1000u) * 1000000L),
    };
    while (!atomic_load(&l->flush_stop)) {
        (void)flush_ring(l);
        nanosleep(&ts, NULL);
    }
    (void)flush_ring(l);
    return NULL;
}

frame_logger_t *frame_logger_create(const frame_logger_cfg_t *cfg)
{
    if (!cfg || !cfg->base_path || !cfg->prefix) { errno = EINVAL; return NULL; }
    frame_logger_t *l = calloc(1, sizeof(*l));
    if (!l) return NULL;
    l->cfg = *cfg;
    if (l->cfg.max_file_bytes    == 0u) l->cfg.max_file_bytes    = FRAME_LOGGER_DEFAULT_MAX_BYTES;
    if (l->cfg.flush_interval_ms == 0u) l->cfg.flush_interval_ms = FRAME_LOGGER_DEFAULT_FLUSH_MS;
    if (l->cfg.max_rotations     == 0u) l->cfg.max_rotations     = FRAME_LOGGER_DEFAULT_ROTATIONS;
    if (mkdir(l->cfg.base_path, 0755) < 0 && errno != EEXIST) { free(l); return NULL; }
    if (l->cfg.max_rotations > 0u) {
        l->history = calloc(l->cfg.max_rotations, sizeof(char *));
        if (!l->history) { free(l); return NULL; }
    }
    l->ring = rb_create(LOG_RING_CAPACITY, sizeof(log_entry_t));
    if (!l->ring) { free(l->history); free(l); return NULL; }
    pthread_mutex_init(&l->file_mutex, NULL);
    atomic_store(&l->flush_stop, false);
    pthread_mutex_lock(&l->file_mutex);
    (void)open_new_log_file(l);
    pthread_mutex_unlock(&l->file_mutex);
    if (l->cfg.flush_interval_ms > 0u) {
        if (pthread_create(&l->flush_thread, NULL, flush_thread_fn, l) != 0) {
            rb_destroy(l->ring);
            if (l->fp) fclose(l->fp);
            free(l->history);
            pthread_mutex_destroy(&l->file_mutex);
            free(l);
            return NULL;
        }
    }
    return l;
}

int frame_logger_write(frame_logger_t *logger,
                       const struct can_frame *frame,
                       uint64_t ts_ns,
                       unsigned int channel)
{
    log_entry_t entry;
    double  ts_s    = (double)ts_ns / 1.0e9;
    canid_t id      = frame->can_id & CAN_EFF_MASK;
    int     id_width = (frame->can_id & CAN_EFF_FLAG) ? 8 : 3;

    char data_hex[CAN_MAX_DLEN * 3u + 1u];
    data_hex[0] = '\0';
    for (uint8_t i = 0u; i < frame->can_dlc; ++i)
        snprintf(data_hex + i * 3, 4u, "%02X ", frame->data[i]);
    size_t dlen = strlen(data_hex);
    if (dlen > 0u) data_hex[dlen - 1u] = '\0';

    entry.len = (size_t)snprintf(entry.data, sizeof(entry.data),
        "%12.6f  %u  %0*" PRIX32 "  Rx  d  %u  %s\n",
        ts_s, channel, id_width, (uint32_t)id,
        frame->can_dlc, data_hex);

    if (!rb_push(logger->ring, &entry)) return -ENOBUFS;
    return 0;
}

int frame_logger_flush(frame_logger_t *logger) { return flush_ring(logger); }

void frame_logger_destroy(frame_logger_t *logger)
{
    if (!logger) return;
    atomic_store(&logger->flush_stop, true);
    if (logger->cfg.flush_interval_ms > 0u)
        pthread_join(logger->flush_thread, NULL);
    (void)flush_ring(logger);
    pthread_mutex_lock(&logger->file_mutex);
    if (logger->fp) { fclose(logger->fp); logger->fp = NULL; }
    pthread_mutex_unlock(&logger->file_mutex);
    pthread_mutex_destroy(&logger->file_mutex);
    rb_destroy(logger->ring);
    if (logger->history) {
        for (unsigned int i = 0u; i < logger->cfg.max_rotations; ++i)
            free(logger->history[i]);
        free(logger->history);
    }
    free(logger);
}
