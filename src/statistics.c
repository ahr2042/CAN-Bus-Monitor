/* SPDX-License-Identifier: GPL-3.0-or-later
 * statistics.c -- Per-ID and aggregate CAN bus statistics
 * Copyright (C) 2026  Ahmad Rashed
 */
#define _POSIX_C_SOURCE 200809L
#include "statistics.h"
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FNV_OFFSET  2166136261u
#define FNV_PRIME   16777619u

static uint32_t fnv1a_u32(uint32_t val)
{
    uint32_t h = FNV_OFFSET;
    for (int i = 0; i < 4; ++i) {
        h ^= (uint8_t)(val & 0xFFu);
        h *= FNV_PRIME;
        val >>= 8u;
    }
    return h;
}

typedef struct {
    bool          occupied;
    stats_entry_t entry;
    double   dlc_m2;
    double   interval_m2;
    uint64_t welford_n;
} bucket_t;

struct statistics {
    bucket_t          *buckets;
    size_t             capacity;
    size_t             size;
    _Atomic(uint64_t)  total_frames;
    _Atomic(uint64_t)  total_bytes;
    _Atomic(uint64_t)  dropped_frames;
    _Atomic(uint64_t)  log_errors;
};

static size_t next_pow2_sz(size_t n)
{
    if (n == 0u) return 1u;
    --n;
    n |= n >> 1u; n |= n >> 2u; n |= n >> 4u;
    n |= n >> 8u; n |= n >> 16u;
    return n + 1u;
}

static bucket_t *ht_find_or_insert(struct statistics *s, canid_t id)
{
    size_t mask  = s->capacity - 1u;
    size_t index = fnv1a_u32(id) & mask;
    for (size_t probe = 0u; probe < s->capacity; ++probe) {
        bucket_t *b = &s->buckets[(index + probe) & mask];
        if (!b->occupied) {
            memset(b, 0, sizeof(*b));
            b->occupied      = true;
            b->entry.id      = id;
            b->entry.dlc_min = 0xFF;
            s->size++;
            return b;
        }
        if (b->entry.id == id) return b;
    }
    return NULL;
}

static int ht_resize(struct statistics *s, size_t new_cap)
{
    bucket_t *new_buckets = calloc(new_cap, sizeof(bucket_t));
    if (!new_buckets) return -1;
    bucket_t *old     = s->buckets;
    size_t    old_cap = s->capacity;
    s->buckets  = new_buckets;
    s->capacity = new_cap;
    s->size     = 0u;
    for (size_t i = 0u; i < old_cap; ++i) {
        if (!old[i].occupied) continue;
        bucket_t *dst = ht_find_or_insert(s, old[i].entry.id);
        if (dst) *dst = old[i];
    }
    free(old);
    return 0;
}

stats_t *stats_create(size_t initial_capacity)
{
    if (initial_capacity == 0u) initial_capacity = 256u;
    stats_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->capacity = next_pow2_sz(initial_capacity);
    s->buckets  = calloc(s->capacity, sizeof(bucket_t));
    if (!s->buckets) { free(s); return NULL; }
    atomic_store(&s->total_frames,   0u);
    atomic_store(&s->total_bytes,    0u);
    atomic_store(&s->dropped_frames, 0u);
    atomic_store(&s->log_errors,     0u);
    return s;
}

void stats_update(stats_t *s, const struct can_frame *frame, uint64_t ts_ns)
{
    if (s->size * 10u >= s->capacity * 7u)
        (void)ht_resize(s, s->capacity * 2u);

    canid_t   id = frame->can_id & CAN_EFF_MASK;
    bucket_t *b  = ht_find_or_insert(s, id);
    if (!b) return;
    stats_entry_t *e = &b->entry;

    atomic_fetch_add(&s->total_frames, 1u);
    atomic_fetch_add(&s->total_bytes,  (uint64_t)frame->can_dlc);
    e->frame_count++;
    e->byte_count += frame->can_dlc;
    if (frame->can_dlc < e->dlc_min) e->dlc_min = frame->can_dlc;
    if (frame->can_dlc > e->dlc_max) e->dlc_max = frame->can_dlc;

    b->welford_n++;
    double dlc_d  = (double)frame->can_dlc - e->dlc_mean;
    e->dlc_mean  += dlc_d / (double)b->welford_n;
    b->dlc_m2    += dlc_d * ((double)frame->can_dlc - e->dlc_mean);

    if (e->frame_count == 1u) {
        e->ts_first_ns = ts_ns;
        e->ts_last_ns  = ts_ns;
    } else {
        double interval_us = (double)(ts_ns - e->ts_last_ns) / 1000.0;
        e->ts_last_ns = ts_ns;
        uint64_t n_int = e->frame_count - 1u;
        double id_d = interval_us - e->interval_mean_us;
        e->interval_mean_us += id_d / (double)n_int;
        b->interval_m2 += id_d * (interval_us - e->interval_mean_us);
        if (n_int > 1u)
            e->interval_var_us = b->interval_m2 / (double)(n_int - 1u);
    }
}

void stats_increment_dropped(stats_t *s) { atomic_fetch_add(&s->dropped_frames, 1u); }
void stats_increment_log_error(stats_t *s) { atomic_fetch_add(&s->log_errors, 1u); }

stats_aggregate_t stats_get_aggregate(const stats_t *s)
{
    return (stats_aggregate_t){
        .total_frames   = atomic_load(&s->total_frames),
        .total_bytes    = atomic_load(&s->total_bytes),
        .dropped_frames = atomic_load(&s->dropped_frames),
        .log_errors     = atomic_load(&s->log_errors),
        .unique_ids     = s->size,
    };
}

static int cmp_desc(const void *a, const void *b)
{
    const stats_entry_t *ea = (const stats_entry_t *)a;
    const stats_entry_t *eb = (const stats_entry_t *)b;
    if (eb->frame_count > ea->frame_count) return  1;
    if (eb->frame_count < ea->frame_count) return -1;
    return 0;
}

void stats_print_report(const stats_t *s)
{
    stats_aggregate_t agg = stats_get_aggregate(s);
    printf("\n=== CAN Bus Monitor -- Statistics Report ===\n");
    printf("Total frames   : %" PRIu64 "\n", agg.total_frames);
    printf("Total bytes    : %" PRIu64 "\n", agg.total_bytes);
    printf("Dropped frames : %" PRIu64 "\n", agg.dropped_frames);
    printf("Log errors     : %" PRIu64 "\n", agg.log_errors);
    printf("Unique CAN IDs : %" PRIu64 "\n", agg.unique_ids);
    if (s->size == 0u) { printf("(no frames received)\n"); return; }

    stats_entry_t *entries = malloc(s->size * sizeof(stats_entry_t));
    if (!entries) { printf("(out of memory)\n"); return; }
    size_t n = 0u;
    for (size_t i = 0u; i < s->capacity; ++i)
        if (s->buckets[i].occupied) entries[n++] = s->buckets[i].entry;
    qsort(entries, n, sizeof(stats_entry_t), cmp_desc);
    printf("\n%-10s  %10s  %10s  %4s  %4s  %7s  %10s  %10s\n",
           "CAN ID","Frames","Bytes","dMin","dMax","dMean","us Mean","us StdDev");
    for (size_t i = 0u; i < n; ++i) {
        const stats_entry_t *e = &entries[i];
        printf("%10" PRIX32 "  %10" PRIu64 "  %10" PRIu64
               "  %4u  %4u  %7.2f  %10.1f  %10.1f\n",
               (uint32_t)e->id, e->frame_count, e->byte_count,
               e->dlc_min, e->dlc_max, e->dlc_mean,
               e->interval_mean_us, sqrt(e->interval_var_us));
    }
    free(entries);
}

void stats_destroy(stats_t *s)
{
    if (!s) return;
    free(s->buckets);
    free(s);
}
