/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * cli_parser.h — Command-line argument parser
 *
 * Copyright (C) 2026  Ahmad Rashed
 *
 * Parses argv into a strongly-typed configuration structure.
 * Uses only POSIX getopt_long (no proprietary libraries).
 */

#ifndef CANBUS_MONITOR_CLI_PARSER_H
#define CANBUS_MONITOR_CLI_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "frame_logger.h"   /* frame_logger_cfg_t */
#include "can_socket.h"     /* can_filter_spec_t  */

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of ID filters accepted on the command line */
#define CLI_MAX_FILTERS  32u

/**
 * cli_config_t — Fully resolved runtime configuration.
 *
 * Populated by cli_parse().  All string pointers point into argv[] —
 * do not free them.
 */
typedef struct {
    /* CAN interface */
    const char         *iface;          /**< e.g. "can0"                        */
    unsigned int        timeout_ms;     /**< Socket receive timeout (0=block)   */

    /* Logging */
    frame_logger_cfg_t  logger;         /**< Logger configuration               */
    bool                log_enabled;    /**< False → print to stdout only       */

    /* Filters */
    can_filter_spec_t   filters[CLI_MAX_FILTERS];
    size_t              filter_count;

    /* Display */
    bool                show_stats;     /**< Print statistics on exit           */
    bool                verbose;        /**< Extra diagnostic output            */

    /* Limits */
    uint64_t            max_frames;     /**< Stop after this many frames (0=∞) */
} cli_config_t;

/**
 * cli_parse() — Parse @argc / @argv into @cfg.
 *
 * Prints usage and exits with EXIT_FAILURE on invalid input.
 *
 * @param argc   As received by main().
 * @param argv   As received by main().
 * @param cfg    Output parameter.  Must not be NULL.
 * @return  0 on success, -1 if the caller should exit (e.g. --help printed).
 */
int cli_parse(int argc, char *argv[], cli_config_t *cfg);

/**
 * cli_print_usage() — Write usage text to stdout.
 */
void cli_print_usage(const char *prog_name);

#ifdef __cplusplus
}
#endif

#endif /* CANBUS_MONITOR_CLI_PARSER_H */
