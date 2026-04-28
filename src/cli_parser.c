/* SPDX-License-Identifier: GPL-3.0-or-later
 * cli_parser.c -- Command-line argument parsing
 * Copyright (C) 2026  Ahmad Rashed
 *
 * Parses arguments using getopt_long(3).  All error messages are written
 * to stderr and the program exits with EXIT_FAILURE on invalid input.
 *
 * Usage:
 *   canbus_monitor [OPTIONS] <interface>
 *
 *   -o, --output <dir>      Log directory (default: ./canbus_logs)
 *   -p, --prefix <name>     Log file prefix (default: interface name)
 *   -s, --max-size <bytes>  Max log file size (default: 52428800)
 *   -r, --rotations <n>     Rotation count (default: 10)
 *   -t, --timeout <ms>      Socket receive timeout (default: 200 ms)
 *   -f, --filter <id:mask>  CAN filter in hex (repeatable)
 *   -n, --count <n>         Stop after n frames (0 = unlimited)
 *       --stats             Print statistics on exit
 *   -v, --verbose           Verbose output
 *   -h, --help              Show help
 *       --version           Show version
 */
#include "cli_parser.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROGRAM_VERSION  "1.0.0"
#define DEFAULT_LOG_DIR  "./canbus_logs"
#define DEFAULT_TIMEOUT  200u

enum {
    OPT_STATS   = 128,
    OPT_VERSION = 129,
};

static const struct option s_long_opts[] = {
    { "output",     required_argument, NULL, 'o' },
    { "prefix",     required_argument, NULL, 'p' },
    { "max-size",   required_argument, NULL, 's' },
    { "rotations",  required_argument, NULL, 'r' },
    { "timeout",    required_argument, NULL, 't' },
    { "filter",     required_argument, NULL, 'f' },
    { "count",      required_argument, NULL, 'n' },
    { "stats",      no_argument,       NULL, OPT_STATS   },
    { "verbose",    no_argument,       NULL, 'v' },
    { "help",       no_argument,       NULL, 'h' },
    { "version",    no_argument,       NULL, OPT_VERSION },
    { NULL,         0,                 NULL, 0   },
};

void cli_print_usage(const char *prog_name)
{
    printf(
        "Usage: %s [OPTIONS] <interface>\n"
        "\n"
        "Monitor and log CAN bus frames via SocketCAN.\n"
        "\n"
        "Options:\n"
        "  -o, --output <dir>       Log output directory (default: %s)\n"
        "  -p, --prefix <name>      Log file name prefix (default: iface name)\n"
        "  -s, --max-size <bytes>   Rotate log file at this size (default: 52428800)\n"
        "  -r, --rotations <n>      Rotated files to keep (default: 10)\n"
        "  -t, --timeout <ms>       Receive timeout in ms (default: %u)\n"
        "  -f, --filter <id:mask>   Hex CAN filter, e.g. 0x1F0:0xFF0 (repeatable)\n"
        "  -n, --count <n>          Stop after n frames (0 = unlimited)\n"
        "      --stats              Print per-ID statistics on exit\n"
        "  -v, --verbose            Enable verbose output\n"
        "  -h, --help               Show this help and exit\n"
        "      --version            Show version and exit\n"
        "\n"
        "Examples:\n"
        "  %s can0\n"
        "  %s -o /var/log/can -p vehicle --stats can0\n"
        "  %s -f 0x100:0x7FF -n 1000 vcan0\n"
        "\n"
        "License: GNU General Public License v3.0 or later\n",
        prog_name, DEFAULT_LOG_DIR, DEFAULT_TIMEOUT,
        prog_name, prog_name, prog_name);
}

static int parse_filter(const char *str, can_filter_spec_t *out)
{
    char *colon = strchr(str, ':');
    char *end   = NULL;

    out->id = (canid_t)strtoul(str, &end, 16);
    if (end == str) return -1;

    if (colon) {
        out->mask = (canid_t)strtoul(colon + 1u, &end, 16);
        if (end == colon + 1u) return -1;
    } else {
        out->mask = 0x7FFu;
    }
    return 0;
}

int cli_parse(int argc, char *argv[], cli_config_t *cfg)
{
    if (!cfg) return -1;

    /* Reset getopt state so successive calls in the same process work
     * correctly (important for unit tests that call cli_parse multiple times).
     */
    optind = 1;

    /* Apply defaults */
    memset(cfg, 0, sizeof(*cfg));
    cfg->logger.base_path         = DEFAULT_LOG_DIR;
    cfg->logger.prefix            = NULL;   /* resolved after parsing */
    cfg->logger.max_file_bytes    = FRAME_LOGGER_DEFAULT_MAX_BYTES;
    cfg->logger.flush_interval_ms = FRAME_LOGGER_DEFAULT_FLUSH_MS;
    cfg->logger.max_rotations     = FRAME_LOGGER_DEFAULT_ROTATIONS;
    cfg->timeout_ms               = DEFAULT_TIMEOUT;
    cfg->log_enabled              = true;

    int opt;
    while ((opt = getopt_long(argc, argv, "o:p:s:r:t:f:n:vh",
                              s_long_opts, NULL)) != -1) {
        char *end = NULL;
        switch (opt) {
        case 'o':
            cfg->logger.base_path = optarg;
            break;
        case 'p':
            cfg->logger.prefix = optarg;
            break;
        case 's':
            cfg->logger.max_file_bytes = (size_t)strtoull(optarg, &end, 10);
            if (*end != '\0' || cfg->logger.max_file_bytes == 0u) {
                fprintf(stderr, "Error: invalid --max-size value '%s'\n", optarg);
                exit(EXIT_FAILURE);
            }
            break;
        case 'r':
            cfg->logger.max_rotations = (unsigned)strtoul(optarg, &end, 10);
            if (*end != '\0') {
                fprintf(stderr, "Error: invalid --rotations value '%s'\n", optarg);
                exit(EXIT_FAILURE);
            }
            break;
        case 't':
            cfg->timeout_ms = (unsigned)strtoul(optarg, &end, 10);
            if (*end != '\0') {
                fprintf(stderr, "Error: invalid --timeout value '%s'\n", optarg);
                exit(EXIT_FAILURE);
            }
            break;
        case 'f':
            if (cfg->filter_count >= CLI_MAX_FILTERS) {
                fprintf(stderr, "Error: too many filters (max %u)\n", CLI_MAX_FILTERS);
                exit(EXIT_FAILURE);
            }
            if (parse_filter(optarg, &cfg->filters[cfg->filter_count]) < 0) {
                fprintf(stderr, "Error: invalid filter '%s' (expected hex id:mask)\n", optarg);
                exit(EXIT_FAILURE);
            }
            cfg->filter_count++;
            break;
        case 'n':
            cfg->max_frames = strtoull(optarg, &end, 10);
            if (*end != '\0') {
                fprintf(stderr, "Error: invalid --count value '%s'\n", optarg);
                exit(EXIT_FAILURE);
            }
            break;
        case OPT_STATS:
            cfg->show_stats = true;
            break;
        case 'v':
            cfg->verbose = true;
            break;
        case 'h':
            cli_print_usage(argv[0]);
            return -1;
        case OPT_VERSION:
            printf("canbus_monitor %s\n"
                   "License: GPL-3.0-or-later\n"
                   "Copyright (C) 2026  Ahmad Rashed\n",
                   PROGRAM_VERSION);
            return -1;
        default:
            cli_print_usage(argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    /* Positional argument: interface name */
    if (optind >= argc) {
        fprintf(stderr, "Error: CAN interface name is required.\n\n");
        cli_print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }
    cfg->iface = argv[optind];

    /* Default log prefix to interface name only if -p was not given */
    if (!cfg->logger.prefix) {
        cfg->logger.prefix = cfg->iface;
    }

    return 0;
}
