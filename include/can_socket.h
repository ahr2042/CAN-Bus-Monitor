/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * can_socket.h — SocketCAN interface abstraction
 *
 * Copyright (C) 2026  Ahmad Rashed
 *
 * Provides a thin, error-checked wrapper around the Linux SocketCAN API
 * (AF_CAN / SOCK_RAW).  All public functions return negative errno values
 * on failure so callers can use standard POSIX error handling.
 *
 * Reference: linux/can.h, linux/can/raw.h, socketcan documentation
 */

#ifndef CANBUS_MONITOR_CAN_SOCKET_H
#define CANBUS_MONITOR_CAN_SOCKET_H

#include <stdint.h>
#include <linux/can.h>   /* struct can_frame, canid_t */

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Opaque handle — hides the raw file descriptor from callers.
 * Use can_socket_open() / can_socket_close() for lifecycle management.
 * ------------------------------------------------------------------------- */
typedef struct can_socket can_socket_t;

/* -------------------------------------------------------------------------
 * Filter specification passed to can_socket_set_filters().
 *
 * A received frame passes if:
 *   (frame.can_id & mask) == (id & mask)
 *
 * Set mask = 0 to accept all frames (wildcard).
 * Set the CAN_INV_FILTER bit in id to invert the match.
 * ------------------------------------------------------------------------- */
typedef struct {
    canid_t  id;    /**< CAN ID to match (may include CAN_EFF_FLAG / CAN_RTR_FLAG) */
    canid_t  mask;  /**< Bitmask applied before comparison                          */
} can_filter_spec_t;

/* -------------------------------------------------------------------------
 * CAN socket receive timeout in milliseconds.  Pass 0 for blocking mode.
 * ------------------------------------------------------------------------- */
#define CAN_SOCKET_BLOCK_INFINITE  0u

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/**
 * can_socket_open() — Create and bind a raw CAN socket on @iface.
 *
 * @param iface   Interface name, e.g. "can0", "vcan0".
 * @param timeout_ms  Receive timeout in ms; 0 = block indefinitely.
 * @return  Pointer to an opaque handle on success, NULL on failure (errno set).
 */
can_socket_t *can_socket_open(const char *iface, unsigned int timeout_ms);

/**
 * can_socket_set_filters() — Replace the kernel-level receive filter list.
 *
 * Filtering is done in the kernel before frames are copied to user space,
 * which is more efficient than filtering in user space.
 *
 * @param cs      Handle returned by can_socket_open().
 * @param filters Array of filter specifications.
 * @param count   Number of elements in @filters.
 * @return  0 on success, -errno on failure.
 */
int can_socket_set_filters(can_socket_t *cs,
                           const can_filter_spec_t *filters,
                           size_t count);

/**
 * can_socket_recv() — Receive the next CAN frame.
 *
 * Blocks until a frame arrives or the configured timeout expires.
 *
 * @param cs     Handle returned by can_socket_open().
 * @param frame  Output parameter: populated with the received frame.
 * @param ts_ns  Output parameter: kernel receive timestamp (CLOCK_TAI), nanoseconds.
 *               Set to 0 if timestamp is unavailable.
 * @return  1 if a frame was received, 0 on timeout, -errno on error.
 */
int can_socket_recv(can_socket_t *cs, struct can_frame *frame, uint64_t *ts_ns);

/**
 * can_socket_get_fd() — Return the underlying file descriptor.
 *
 * Useful for integrating with poll()/select()/epoll() in event loops.
 */
int can_socket_get_fd(const can_socket_t *cs);

/**
 * can_socket_close() — Release all resources associated with @cs.
 *
 * Passing NULL is a no-op.
 */
void can_socket_close(can_socket_t *cs);

#ifdef __cplusplus
}
#endif

#endif /* CANBUS_MONITOR_CAN_SOCKET_H */
