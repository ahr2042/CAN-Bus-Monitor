/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * test_cli_parser.c — Unit tests for the CLI argument parser
 *
 * Copyright (C) 2026  Ahmad Rashed
 *
 * Because cli_parse() calls exit() on invalid input, invalid-input cases
 * are not tested here; only valid argument combinations are covered.
 *
 * Tests cover:
 *  - Default values when only interface is specified
 *  - Custom log directory (--output)
 *  - Custom prefix (--prefix)
 *  - Filter parsing (--filter)
 *  - Frame count limit (--count)
 *  - Verbose and stats flags
 *  - Timeout override (--timeout)
 */

#include "unity/unity.h"
#include "cli_parser.h"

#include <string.h>

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

/**
 * build_argv() — Construct an argv[] array from a list of string literals.
 *
 * The array is stack-allocated so callers must not keep pointers after
 * the enclosing scope exits.
 */
#define MAX_ARGV 32

typedef struct {
    const char *v[MAX_ARGV];
    int         c;
} test_argv_t;

static test_argv_t make_argv(const char **strs, int n)
{
    test_argv_t t;
    memset(&t, 0, sizeof(t));
    t.v[0] = "canbus_monitor";
    t.c = 1;
    for (int i = 0; i < n && t.c < MAX_ARGV - 1; ++i) {
        t.v[t.c++] = strs[i];
    }
    return t;
}

/* -------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

static void test_defaults_with_interface_only(void)
{
    const char *args[] = { "can0" };
    test_argv_t a = make_argv(args, 1);
    cli_config_t cfg;

    int rc = cli_parse(a.c, (char **)a.v, &cfg);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_STRING("can0", cfg.iface);
    TEST_ASSERT_EQUAL_UINT(200u, cfg.timeout_ms);
    TEST_ASSERT_EQUAL_UINT64(0u, cfg.max_frames);
    TEST_ASSERT_FALSE(cfg.show_stats);
    TEST_ASSERT_FALSE(cfg.verbose);
    TEST_ASSERT_EQUAL_SIZE_T(0u, cfg.filter_count);
}

static void test_custom_output_directory(void)
{
    const char *args[] = { "--output", "/tmp/can_logs", "vcan0" };
    test_argv_t a = make_argv(args, 3);
    cli_config_t cfg;

    int rc = cli_parse(a.c, (char **)a.v, &cfg);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_STRING("/tmp/can_logs", cfg.logger.base_path);
}

static void test_custom_prefix(void)
{
    const char *args[] = { "--prefix", "vehicle", "can0" };
    test_argv_t a = make_argv(args, 3);
    cli_config_t cfg;

    int rc = cli_parse(a.c, (char **)a.v, &cfg);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_STRING("vehicle", cfg.logger.prefix);
}

static void test_prefix_defaults_to_interface(void)
{
    const char *args[] = { "can1" };
    test_argv_t a = make_argv(args, 1);
    cli_config_t cfg;

    cli_parse(a.c, (char **)a.v, &cfg);

    TEST_ASSERT_EQUAL_STRING("can1", cfg.logger.prefix);
}

static void test_verbose_flag(void)
{
    const char *args[] = { "--verbose", "can0" };
    test_argv_t a = make_argv(args, 2);
    cli_config_t cfg;

    cli_parse(a.c, (char **)a.v, &cfg);
    TEST_ASSERT_TRUE(cfg.verbose);
}

static void test_stats_flag(void)
{
    const char *args[] = { "--stats", "can0" };
    test_argv_t a = make_argv(args, 2);
    cli_config_t cfg;

    cli_parse(a.c, (char **)a.v, &cfg);
    TEST_ASSERT_TRUE(cfg.show_stats);
}

static void test_frame_count_limit(void)
{
    const char *args[] = { "--count", "1000", "can0" };
    test_argv_t a = make_argv(args, 3);
    cli_config_t cfg;

    cli_parse(a.c, (char **)a.v, &cfg);
    TEST_ASSERT_EQUAL_UINT64(1000u, cfg.max_frames);
}

static void test_timeout_override(void)
{
    const char *args[] = { "--timeout", "500", "can0" };
    test_argv_t a = make_argv(args, 3);
    cli_config_t cfg;

    cli_parse(a.c, (char **)a.v, &cfg);
    TEST_ASSERT_EQUAL_UINT(500u, cfg.timeout_ms);
}

static void test_single_filter(void)
{
    const char *args[] = { "--filter", "1F0:FF0", "can0" };
    test_argv_t a = make_argv(args, 3);
    cli_config_t cfg;

    cli_parse(a.c, (char **)a.v, &cfg);
    TEST_ASSERT_EQUAL_SIZE_T(1u, cfg.filter_count);
    TEST_ASSERT_EQUAL_UINT(0x1F0u, (unsigned)cfg.filters[0].id);
    TEST_ASSERT_EQUAL_UINT(0xFF0u, (unsigned)cfg.filters[0].mask);
}

static void test_multiple_filters(void)
{
    const char *args[] = {
        "--filter", "100:7FF",
        "--filter", "200:7FF",
        "--filter", "300:700",
        "can0"
    };
    test_argv_t a = make_argv(args, 7);
    cli_config_t cfg;

    cli_parse(a.c, (char **)a.v, &cfg);
    TEST_ASSERT_EQUAL_SIZE_T(3u, cfg.filter_count);
    TEST_ASSERT_EQUAL_UINT(0x100u, (unsigned)cfg.filters[0].id);
    TEST_ASSERT_EQUAL_UINT(0x200u, (unsigned)cfg.filters[1].id);
    TEST_ASSERT_EQUAL_UINT(0x300u, (unsigned)cfg.filters[2].id);
}

static void test_filter_id_only_gets_default_mask(void)
{
    /* No colon in filter → mask should default to 0x7FF */
    const char *args[] = { "--filter", "1A0", "can0" };
    test_argv_t a = make_argv(args, 3);
    cli_config_t cfg;

    cli_parse(a.c, (char **)a.v, &cfg);
    TEST_ASSERT_EQUAL_SIZE_T(1u, cfg.filter_count);
    TEST_ASSERT_EQUAL_UINT(0x1A0u, (unsigned)cfg.filters[0].id);
    TEST_ASSERT_EQUAL_UINT(0x7FFu, (unsigned)cfg.filters[0].mask);
}

static void test_max_size_option(void)
{
    const char *args[] = { "--max-size", "10485760", "can0" };
    test_argv_t a = make_argv(args, 3);
    cli_config_t cfg;

    cli_parse(a.c, (char **)a.v, &cfg);
    TEST_ASSERT_EQUAL_UINT64(10485760u, (uint64_t)cfg.logger.max_file_bytes);
}

static void test_rotations_option(void)
{
    const char *args[] = { "--rotations", "5", "can0" };
    test_argv_t a = make_argv(args, 3);
    cli_config_t cfg;

    cli_parse(a.c, (char **)a.v, &cfg);
    TEST_ASSERT_EQUAL_UINT(5u, cfg.logger.max_rotations);
}

/* -------------------------------------------------------------------------
 * Runner
 * ------------------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_defaults_with_interface_only);
    RUN_TEST(test_custom_output_directory);
    RUN_TEST(test_custom_prefix);
    RUN_TEST(test_prefix_defaults_to_interface);
    RUN_TEST(test_verbose_flag);
    RUN_TEST(test_stats_flag);
    RUN_TEST(test_frame_count_limit);
    RUN_TEST(test_timeout_override);
    RUN_TEST(test_single_filter);
    RUN_TEST(test_multiple_filters);
    RUN_TEST(test_filter_id_only_gets_default_mask);
    RUN_TEST(test_max_size_option);
    RUN_TEST(test_rotations_option);

    return UNITY_END();
}
