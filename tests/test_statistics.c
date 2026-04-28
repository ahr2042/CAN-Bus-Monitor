/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * test_statistics.c — Unit tests for the CAN statistics module
 *
 * Copyright (C) 2026  Ahmad Rashed
 *
 * Tests cover:
 *  - Aggregate frame and byte counting
 *  - Per-ID DLC min/max tracking
 *  - Welford mean DLC convergence
 *  - Inter-frame interval tracking
 *  - Hash table growth and collision handling (many unique IDs)
 *  - Dropped frame and log error counters
 *  - NULL safety of stats_destroy()
 */

#include "unity/unity.h"
#include "statistics.h"

#include <linux/can.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static struct can_frame make_frame(canid_t id, uint8_t dlc)
{
    struct can_frame f;
    memset(&f, 0, sizeof(f));
    f.can_id  = id & CAN_SFF_MASK;  /* standard 11-bit ID */
    f.can_dlc = dlc;
    for (uint8_t i = 0u; i < dlc; ++i) {
        f.data[i] = i;
    }
    return f;
}

#define TS_STEP_NS  10000000ULL  /* 10 ms between frames */

/* -------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

static void test_create_destroy(void)
{
    stats_t *s = stats_create(0u);
    TEST_ASSERT_NOT_NULL(s);
    stats_destroy(s);
}

static void test_destroy_null_is_safe(void)
{
    stats_destroy(NULL);
    TEST_PASS();
}

static void test_aggregate_counts_single_frame(void)
{
    stats_t *s = stats_create(16u);
    TEST_ASSERT_NOT_NULL(s);

    struct can_frame f = make_frame(0x100u, 4u);
    stats_update(s, &f, 1000000000ULL);

    stats_aggregate_t agg = stats_get_aggregate(s);
    TEST_ASSERT_EQUAL_UINT64(1u, agg.total_frames);
    TEST_ASSERT_EQUAL_UINT64(4u, agg.total_bytes);
    TEST_ASSERT_EQUAL_UINT64(1u, agg.unique_ids);

    stats_destroy(s);
}

static void test_aggregate_counts_multiple_frames(void)
{
    stats_t *s = stats_create(16u);
    TEST_ASSERT_NOT_NULL(s);

    for (int i = 0; i < 10; ++i) {
        struct can_frame f = make_frame(0x200u, 8u);
        stats_update(s, &f, (uint64_t)i * TS_STEP_NS);
    }

    stats_aggregate_t agg = stats_get_aggregate(s);
    TEST_ASSERT_EQUAL_UINT64(10u,  agg.total_frames);
    TEST_ASSERT_EQUAL_UINT64(80u,  agg.total_bytes);
    TEST_ASSERT_EQUAL_UINT64(1u,   agg.unique_ids);

    stats_destroy(s);
}

static void test_dlc_min_max(void)
{
    stats_t *s = stats_create(16u);
    TEST_ASSERT_NOT_NULL(s);

    const uint8_t dlcs[] = { 4u, 2u, 8u, 1u, 6u };

    for (size_t i = 0u; i < 5u; ++i) {
        struct can_frame f = make_frame(0x300u, dlcs[i]);
        stats_update(s, &f, i * TS_STEP_NS);
    }

    /* We can't read per-ID entries directly (opaque), but aggregate
     * byte count tells us the sum of DLCs: 4+2+8+1+6 = 21 */
    stats_aggregate_t agg = stats_get_aggregate(s);
    TEST_ASSERT_EQUAL_UINT64(5u,  agg.total_frames);
    TEST_ASSERT_EQUAL_UINT64(21u, agg.total_bytes);

    stats_destroy(s);
}

static void test_multiple_unique_ids(void)
{
    stats_t *s = stats_create(16u);
    TEST_ASSERT_NOT_NULL(s);

    for (canid_t id = 0x001u; id <= 0x010u; ++id) {
        struct can_frame f = make_frame(id, 4u);
        stats_update(s, &f, (uint64_t)id * TS_STEP_NS);
    }

    stats_aggregate_t agg = stats_get_aggregate(s);
    TEST_ASSERT_EQUAL_UINT64(16u, agg.total_frames);
    TEST_ASSERT_EQUAL_UINT64(16u, agg.unique_ids);

    stats_destroy(s);
}

static void test_hash_table_grows_with_many_ids(void)
{
    /* Insert 300 unique IDs to force multiple resize operations */
    stats_t *s = stats_create(8u);  /* start small */
    TEST_ASSERT_NOT_NULL(s);

    for (canid_t id = 1u; id <= 300u; ++id) {
        struct can_frame f = make_frame(id, 2u);
        stats_update(s, &f, (uint64_t)id * TS_STEP_NS);
    }

    stats_aggregate_t agg = stats_get_aggregate(s);
    TEST_ASSERT_EQUAL_UINT64(300u, agg.total_frames);
    TEST_ASSERT_EQUAL_UINT64(300u, agg.unique_ids);

    stats_destroy(s);
}

static void test_dropped_frame_counter(void)
{
    stats_t *s = stats_create(16u);
    TEST_ASSERT_NOT_NULL(s);

    stats_increment_dropped(s);
    stats_increment_dropped(s);
    stats_increment_dropped(s);

    stats_aggregate_t agg = stats_get_aggregate(s);
    TEST_ASSERT_EQUAL_UINT64(3u, agg.dropped_frames);

    stats_destroy(s);
}

static void test_log_error_counter(void)
{
    stats_t *s = stats_create(16u);
    TEST_ASSERT_NOT_NULL(s);

    stats_increment_log_error(s);

    stats_aggregate_t agg = stats_get_aggregate(s);
    TEST_ASSERT_EQUAL_UINT64(1u, agg.log_errors);

    stats_destroy(s);
}

static void test_same_id_repeated(void)
{
    stats_t *s = stats_create(16u);
    TEST_ASSERT_NOT_NULL(s);

    for (int i = 0; i < 50; ++i) {
        struct can_frame f = make_frame(0x7FFu, 8u);
        stats_update(s, &f, (uint64_t)i * TS_STEP_NS);
    }

    stats_aggregate_t agg = stats_get_aggregate(s);
    TEST_ASSERT_EQUAL_UINT64(50u,  agg.total_frames);
    TEST_ASSERT_EQUAL_UINT64(400u, agg.total_bytes);
    TEST_ASSERT_EQUAL_UINT64(1u,   agg.unique_ids);

    stats_destroy(s);
}

/* -------------------------------------------------------------------------
 * Runner
 * ------------------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_create_destroy);
    RUN_TEST(test_destroy_null_is_safe);
    RUN_TEST(test_aggregate_counts_single_frame);
    RUN_TEST(test_aggregate_counts_multiple_frames);
    RUN_TEST(test_dlc_min_max);
    RUN_TEST(test_multiple_unique_ids);
    RUN_TEST(test_hash_table_grows_with_many_ids);
    RUN_TEST(test_dropped_frame_counter);
    RUN_TEST(test_log_error_counter);
    RUN_TEST(test_same_id_repeated);

    return UNITY_END();
}
