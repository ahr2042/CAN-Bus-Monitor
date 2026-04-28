/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * frame_logger.h — CAN frame logging with rotation and flush thread
 *
 * Copyright (C) 2026  Ahmad Rashed
 *
 * Writes ASC-format log files (compatible with CANalyzer / cantools) and
 * rotates them once the configured size limit is reached.  A background
 * pthread flushes buffered data to disk on a configurable interval so
 * the reception thread is never blocked on I/O.
 *
 * Log format (ASC — AUTOSAR/PEAK compatible subset):
 *   <timestamp_s>  <channel>  <id#hex>  Rx  d  <dlc>  <data_bytes_hex>
 *
 * Example:
 *   0.001234  1  0CF00400#  Rx  d  8  FF FF 00 00 00 00 00 00
 */

#ifndef CANBUS_MONITOR_FRAME_LOGGER_H
#define CANBUS_MONITOR_FRAME_LOGGER_H

#include <stddef.h>
#include <stdint.h>
#include <linux/can.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Logger configuration
 * ------------------------------------------------------------------------- */
typedef struct {
    const char *base_path;      /**< Directory for log files, e.g. "/var/log/canmon" */
    const char *prefix;         /**< File name prefix, e.g. "can0"                   */
    size_t      max_file_bytes; /**< Rotate when file exceeds this size               */
    unsigned    flush_interval_ms; /**< Background flush period (0 = sync write)     */
    unsigned    max_rotations;  /**< Number of rotated files to keep (0 = unlimited) */
} frame_logger_cfg_t;

/* Sensible defaults -------------------------------------------------------- */
#define FRAME_LOGGER_DEFAULT_MAX_BYTES   (50u * 1024u * 1024u)  /* 50 MiB  */
#define FRAME_LOGGER_DEFAULT_FLUSH_MS    500u
#define FRAME_LOGGER_DEFAULT_ROTATIONS   10u

/* -------------------------------------------------------------------------
 * Opaque logger handle
 * ------------------------------------------------------------------------- */
typedef struct frame_logger frame_logger_t;

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/**
 * frame_logger_create() — Allocate and start a logger instance.
 *
 * Creates the base directory if it does not exist.  Spawns the flush thread
 * unless cfg->flush_interval_ms == 0.
 *
 * @return  Pointer to handle on success, NULL on failure (errno set).
 */
frame_logger_t *frame_logger_create(const frame_logger_cfg_t *cfg);

/**
 * frame_logger_write() — Enqueue a CAN frame for logging.
 *
 * Thread-safe.  The frame is formatted and placed into an internal ring
 * buffer; the flush thread (or the next synchronous write) drains it to
 * disk.  This function never blocks on disk I/O.
 *
 * @param logger  Handle returned by frame_logger_create().
 * @param frame   The CAN frame to log.
 * @param ts_ns   Kernel receive timestamp (nanoseconds, CLOCK_TAI).
 *                Pass 0 to use the current wall-clock time.
 * @param channel CAN channel index for the ASC channel field.
 * @return  0 on success, -ENOBUFS if the ring buffer is full (frame dropped).
 */
int frame_logger_write(frame_logger_t *logger,
                       const struct can_frame *frame,
                       uint64_t ts_ns,
                       unsigned int channel);

/**
 * frame_logger_flush() — Synchronously flush all buffered entries to disk.
 *
 * Caller can invoke this during shutdown to ensure no frames are lost.
 *
 * @return  0 on success, -errno on I/O error.
 */
int frame_logger_flush(frame_logger_t *logger);

/**
 * frame_logger_destroy() — Flush, stop flush thread, and free resources.
 *
 * Passing NULL is a no-op.
 */
void frame_logger_destroy(frame_logger_t *logger);

#ifdef __cplusplus
}
#endif

#endif /* CANBUS_MONITOR_FRAME_LOGGER_H */
