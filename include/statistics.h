/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * statistics.h — Per-ID and aggregate CAN bus statistics
 *
 * Copyright (C) 2026  Ahmad Rashed
 *
 * Maintains a hash table (open addressing, FNV-1a key hash) keyed on
 * CAN ID.  Each entry tracks frame count, byte count, min/max/mean DLC,
 * and inter-frame timing.
 */

#ifndef CANBUS_MONITOR_STATISTICS_H
#define CANBUS_MONITOR_STATISTICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <linux/can.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Per-ID statistics record
 * ------------------------------------------------------------------------- */
typedef struct {
    canid_t     id;             /**< CAN ID (11-bit or 29-bit)        */
    uint64_t    frame_count;    /**< Total frames seen for this ID    */
    uint64_t    byte_count;     /**< Total data bytes (sum of DLCs)   */
    uint8_t     dlc_min;        /**< Minimum DLC observed             */
    uint8_t     dlc_max;        /**< Maximum DLC observed             */
    double      dlc_mean;       /**< Running mean DLC (Welford)       */
    uint64_t    ts_first_ns;    /**< Timestamp of first frame (ns)    */
    uint64_t    ts_last_ns;     /**< Timestamp of most recent frame   */
    double      interval_mean_us; /**< Mean inter-frame interval (µs) */
    double      interval_var_us;  /**< Welford variance of interval   */
} stats_entry_t;

/* -------------------------------------------------------------------------
 * Aggregate statistics
 * ------------------------------------------------------------------------- */
typedef struct {
    uint64_t total_frames;      /**< All frames received              */
    uint64_t total_bytes;       /**< All data bytes                   */
    uint64_t dropped_frames;    /**< Frames dropped (ring buffer full)*/
    uint64_t log_errors;        /**< Logger I/O errors                */
    uint64_t unique_ids;        /**< Number of distinct CAN IDs seen  */
} stats_aggregate_t;

/* -------------------------------------------------------------------------
 * Opaque statistics context
 * ------------------------------------------------------------------------- */
typedef struct statistics stats_t;

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/**
 * stats_create() — Allocate a statistics context.
 *
 * @param initial_capacity  Initial number of hash table buckets.
 *                          0 → use a default of 256.
 * @return  Handle on success, NULL on allocation failure.
 */
stats_t *stats_create(size_t initial_capacity);

/**
 * stats_update() — Incorporate a newly received frame into the statistics.
 *
 * @param stats   Handle returned by stats_create().
 * @param frame   Received CAN frame.
 * @param ts_ns   Kernel timestamp of the frame (nanoseconds).
 */
void stats_update(stats_t *stats,
                  const struct can_frame *frame,
                  uint64_t ts_ns);

/**
 * stats_increment_dropped() — Record a dropped frame (ring buffer overflow).
 */
void stats_increment_dropped(stats_t *stats);

/**
 * stats_increment_log_error() — Record a logger I/O error.
 */
void stats_increment_log_error(stats_t *stats);

/**
 * stats_get_aggregate() — Return a copy of the aggregate counters.
 */
stats_aggregate_t stats_get_aggregate(const stats_t *stats);

/**
 * stats_print_report() — Write a human-readable report to stdout.
 *
 * Lists each observed CAN ID sorted by frame count (descending).
 */
void stats_print_report(const stats_t *stats);

/**
 * stats_destroy() — Release all resources.  Passing NULL is a no-op.
 */
void stats_destroy(stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* CANBUS_MONITOR_STATISTICS_H */
