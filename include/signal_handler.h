/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * signal_handler.h — Graceful shutdown via POSIX signals
 *
 * Copyright (C) 2026  Ahmad Rashed
 *
 * Installs handlers for SIGINT, SIGTERM and SIGHUP using sigaction(2).
 * Exposes a volatile atomic flag that the main loop polls for clean exit.
 *
 * Design notes:
 *  - Only async-signal-safe operations are performed inside the handler
 *    (setting an atomic flag, writing a one-byte pipe notification).
 *  - SIGHUP triggers a log rotation request rather than termination.
 */

#ifndef CANBUS_MONITOR_SIGNAL_HANDLER_H
#define CANBUS_MONITOR_SIGNAL_HANDLER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * sh_install() — Register SIGINT, SIGTERM, and SIGHUP handlers.
 *
 * Must be called once from the main thread before spawning worker threads.
 *
 * @return  0 on success, -errno on failure.
 */
int sh_install(void);

/**
 * sh_termination_requested() — Non-blocking check for shutdown signal.
 *
 * @return  true if SIGINT or SIGTERM was received.
 */
bool sh_termination_requested(void);

/**
 * sh_rotation_requested() — Non-blocking check for log-rotation signal.
 *
 * Clears the flag on read (edge-triggered semantics).
 *
 * @return  true if SIGHUP was received since the last call.
 */
bool sh_rotation_requested(void);

/**
 * sh_get_wake_fd() — File descriptor that becomes readable on any signal.
 *
 * The descriptor is the read end of an internal self-pipe.  Use it with
 * poll()/epoll() so the event loop wakes up immediately on signal receipt
 * without busy-polling sh_termination_requested().
 *
 * @return  Non-negative fd, or -1 if sh_install() has not been called.
 */
int sh_get_wake_fd(void);

#ifdef __cplusplus
}
#endif

#endif /* CANBUS_MONITOR_SIGNAL_HANDLER_H */
