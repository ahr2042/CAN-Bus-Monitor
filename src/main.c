/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * main.c — CAN Bus Monitor and Logger entry point
 *
 * Copyright (C) 2026  Ahmad Rashed
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 * Architecture overview
 * ---------------------------------------------------------------------------
 *
 *  ┌─────────────────────────────────────────────────────────────────────┐
 *  │                         main thread                                 │
 *  │                                                                     │
 *  │  poll(CAN socket fd, signal pipe fd)                                │
 *  │       │                                                             │
 *  │       ├─ CAN frame ready ──► can_socket_recv()                     │
 *  │       │                       │                                     │
 *  │       │                       ├─► frame_logger_write()  (lock-free) │
 *  │       │                       ├─► stats_update()                   │
 *  │       │                       └─► print to stdout (if verbose)     │
 *  │       │                                                             │
 *  │       └─ signal received ───► set exit flag or rotate log          │
 *  └───────────────────────┬─────────────────────────────────────────────┘
 *                          │  ring buffer (SPSC, lock-free)
 *  ┌───────────────────────▼─────────────────────────────────────────────┐
 *  │                    flush thread                                     │
 *  │   drains ring buffer ──► writes ASC log lines ──► rotates file     │
 *  └─────────────────────────────────────────────────────────────────────┘
 */

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/can.h>

#include "can_socket.h"
#include "cli_parser.h"
#include "frame_logger.h"
#include "signal_handler.h"
#include "statistics.h"

/* -------------------------------------------------------------------------
 * CAN ID formatting helper
 * ------------------------------------------------------------------------- */

/**
 * format_can_id() — Format a CAN ID for human-readable output.
 *
 * Standard 11-bit IDs are formatted as 3-digit hex; extended 29-bit IDs
 * as 8-digit hex with the suffix 'X'.
 *
 * @param id      Raw canid_t value from struct can_frame.
 * @param buf     Output buffer (at least 12 bytes).
 * @param buf_sz  Size of @buf.
 */
static void format_can_id(canid_t id, char *buf, size_t buf_sz)
{
    if (id & CAN_EFF_FLAG) {
        snprintf(buf, buf_sz, "%08X X", id & CAN_EFF_MASK);
    } else if (id & CAN_ERR_FLAG) {
        snprintf(buf, buf_sz, "ERR %08X", id & CAN_ERR_MASK);
    } else {
        snprintf(buf, buf_sz, "%03X", id & CAN_SFF_MASK);
    }
}

/**
 * print_frame() — Print a single CAN frame to stdout.
 *
 * Format mirrors a simplified candump output:
 *   <ts_s>  <iface>  <id>  [<dlc>]  <data...>
 */
static void print_frame(const char *iface,
                        const struct can_frame *frame,
                        uint64_t ts_ns)
{
    char id_str[16];
    char data_str[CAN_MAX_DLEN * 3u + 1u] = { 0 };

    format_can_id(frame->can_id, id_str, sizeof(id_str));

    for (uint8_t i = 0u; i < frame->can_dlc; ++i) {
        snprintf(data_str + i * 3, 4u, "%02X ", frame->data[i]);
    }

    double ts_s = (double)ts_ns / 1.0e9;
    printf("(%12.6f)  %-8s  %-12s  [%u]  %s\n",
           ts_s, iface, id_str, frame->can_dlc, data_str);
}

/* -------------------------------------------------------------------------
 * Main event loop
 * ------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    cli_config_t cfg;
    if (cli_parse(argc, argv, &cfg) < 0) {
        /* --help or --version was printed; exit cleanly */
        return EXIT_SUCCESS;
    }

    /* Install signal handlers before opening any resources */
    if (sh_install() < 0) {
        perror("sh_install");
        return EXIT_FAILURE;
    }

    /* Open CAN socket */
    can_socket_t *sock = can_socket_open(cfg.iface, cfg.timeout_ms);
    if (!sock) {
        fprintf(stderr, "Failed to open CAN socket on '%s': %s\n",
                cfg.iface, strerror(errno));
        return EXIT_FAILURE;
    }

    /* Install kernel-level filters (optional) */
    if (cfg.filter_count > 0u) {
        if (can_socket_set_filters(sock, cfg.filters, cfg.filter_count) < 0) {
            fprintf(stderr, "Warning: could not set CAN filters: %s\n",
                    strerror(errno));
        } else if (cfg.verbose) {
            printf("Applied %zu kernel CAN filter(s).\n", cfg.filter_count);
        }
    }

    /* Create frame logger */
    frame_logger_t *logger = NULL;
    if (cfg.log_enabled) {
        logger = frame_logger_create(&cfg.logger);
        if (!logger) {
            fprintf(stderr, "Failed to create logger at '%s': %s\n",
                    cfg.logger.base_path, strerror(errno));
            can_socket_close(sock);
            return EXIT_FAILURE;
        }
        if (cfg.verbose) {
            printf("Logging to: %s/%s_*.asc  (max %.1f MiB per file, %u rotations)\n",
                   cfg.logger.base_path, cfg.logger.prefix,
                   (double)cfg.logger.max_file_bytes / (1024.0 * 1024.0),
                   cfg.logger.max_rotations);
        }
    }

    /* Create statistics context */
    stats_t *stats = stats_create(0u);
    if (!stats) {
        fprintf(stderr, "Failed to allocate statistics context.\n");
        frame_logger_destroy(logger);
        can_socket_close(sock);
        return EXIT_FAILURE;
    }

    printf("Listening on %s — press Ctrl-C to stop.\n", cfg.iface);

    /* Poll file descriptors:
     *   [0] CAN socket — receives frames
     *   [1] signal self-pipe — wakes on SIGINT/SIGTERM/SIGHUP
     */
    struct pollfd pfds[2] = {
        { .fd = can_socket_get_fd(sock), .events = POLLIN },
        { .fd = sh_get_wake_fd(),        .events = POLLIN },
    };

    uint64_t frame_count = 0u;
    int      exit_code   = EXIT_SUCCESS;

    /* -----------------------------------------------------------------------
     * Main receive loop
     * --------------------------------------------------------------------- */
    while (!sh_termination_requested()) {
        /* Check frame count limit */
        if (cfg.max_frames > 0u && frame_count >= cfg.max_frames) {
            printf("Reached frame limit (%" PRIu64 ").\n", cfg.max_frames);
            break;
        }

        /* Check for SIGHUP — request log rotation */
        if (sh_rotation_requested() && logger) {
            if (cfg.verbose) {
                printf("SIGHUP received — requesting log rotation.\n");
            }
            (void)frame_logger_flush(logger);
        }

        int nfds = poll(pfds, 2u, -1 /* block */);
        if (nfds < 0) {
            if (errno == EINTR) continue;   /* Signal interrupted poll — loop */
            perror("poll");
            exit_code = EXIT_FAILURE;
            break;
        }

        /* Signal pipe readable: drain it (byte content is irrelevant) */
        if (pfds[1].revents & POLLIN) {
            uint8_t discard[16];
            (void)read(pfds[1].fd, discard, sizeof(discard));
            /* Loop will re-check sh_termination_requested() at top */
            continue;
        }

        /* CAN socket readable: receive the frame */
        if (!(pfds[0].revents & POLLIN)) {
            continue;
        }

        struct can_frame frame;
        uint64_t ts_ns = 0u;
        int rc = can_socket_recv(sock, &frame, &ts_ns);

        if (rc < 0) {
            fprintf(stderr, "recv error: %s\n", strerror(-rc));
            exit_code = EXIT_FAILURE;
            break;
        }
        if (rc == 0) {
            /* Timeout (should not happen with poll, but handle defensively) */
            continue;
        }

        /* Ignore error frames in statistics but still log them */
        bool is_error = (frame.can_id & CAN_ERR_FLAG) != 0u;

        if (!is_error) {
            stats_update(stats, &frame, ts_ns);
        }

        if (logger) {
            int log_rc = frame_logger_write(logger, &frame, ts_ns, 1u);
            if (log_rc == -ENOBUFS) {
                stats_increment_dropped(stats);
            } else if (log_rc < 0) {
                stats_increment_log_error(stats);
            }
        }

        if (cfg.verbose || is_error) {
            print_frame(cfg.iface, &frame, ts_ns);
        }

        frame_count++;
    }

    /* -----------------------------------------------------------------------
     * Teardown
     * --------------------------------------------------------------------- */
    printf("\nShutting down — received %" PRIu64 " frame(s).\n", frame_count);

    if (logger) {
        (void)frame_logger_flush(logger);
        frame_logger_destroy(logger);
    }

    if (cfg.show_stats) {
        stats_print_report(stats);
    }

    stats_destroy(stats);
    can_socket_close(sock);

    return exit_code;
}
