/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * signal_handler.c — Graceful shutdown via POSIX signals
 *
 * Copyright (C) 2026  Ahmad Rashed
 *
 * Uses the self-pipe trick:
 *   A pipe is created at startup.  The signal handler writes one byte to
 *   the write end; the main event loop polls the read end alongside the
 *   CAN socket so it wakes up immediately without busy-waiting.
 *
 * Only async-signal-safe functions are used inside signal handlers:
 *   atomic_store, write(2).
 */

#include "signal_handler.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Module-private state
 * ------------------------------------------------------------------------- */

static _Atomic(bool) s_terminate  = false;
static _Atomic(bool) s_rotate     = false;
static int           s_pipe_rd    = -1;
static int           s_pipe_wr    = -1;

/* -------------------------------------------------------------------------
 * Signal handler (async-signal-safe)
 * ------------------------------------------------------------------------- */

static void signal_handler_fn(int signum)
{
    const uint8_t byte = (uint8_t)signum;

    switch (signum) {
    case SIGINT:
    case SIGTERM:
        atomic_store(&s_terminate, true);
        break;
    case SIGHUP:
        atomic_store(&s_rotate, true);
        break;
    default:
        break;
    }

    /* Notify the event loop via the self-pipe.
     * Ignore EAGAIN: if the pipe is full the loop will wake up anyway. */
    if (s_pipe_wr >= 0) {
        (void)write(s_pipe_wr, &byte, 1u);
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

int sh_install(void)
{
    int pipefd[2];

    /* Create a non-blocking self-pipe */
    if (pipe(pipefd) < 0) {
        return -errno;
    }

    /* Make both ends non-blocking to prevent the handler from stalling */
    for (int i = 0; i < 2; ++i) {
        int flags = fcntl(pipefd[i], F_GETFL, 0);
        if (flags < 0 || fcntl(pipefd[i], F_SETFL, flags | O_NONBLOCK) < 0) {
            close(pipefd[0]);
            close(pipefd[1]);
            return -errno;
        }
        /* Set close-on-exec so child processes don't inherit the pipe */
        if (fcntl(pipefd[i], F_SETFD, FD_CLOEXEC) < 0) {
            close(pipefd[0]);
            close(pipefd[1]);
            return -errno;
        }
    }

    s_pipe_rd = pipefd[0];
    s_pipe_wr = pipefd[1];

    /* Install handlers using sigaction for reliable semantics */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler_fn;
    sigemptyset(&sa.sa_mask);
    /* SA_RESTART: automatically restart interrupted system calls where
     * possible (e.g. read/write on the CAN socket) */
    sa.sa_flags = SA_RESTART;

    const int signals[] = { SIGINT, SIGTERM, SIGHUP };
    for (size_t i = 0u; i < sizeof(signals) / sizeof(signals[0]); ++i) {
        if (sigaction(signals[i], &sa, NULL) < 0) {
            close(s_pipe_rd);
            close(s_pipe_wr);
            s_pipe_rd = s_pipe_wr = -1;
            return -errno;
        }
    }

    return 0;
}

bool sh_termination_requested(void)
{
    return atomic_load(&s_terminate);
}

bool sh_rotation_requested(void)
{
    /* Exchange: clear after reading (edge-triggered) */
    return atomic_exchange(&s_rotate, false);
}

int sh_get_wake_fd(void)
{
    return s_pipe_rd;
}
