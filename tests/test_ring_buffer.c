/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * test_ring_buffer.c — Unit tests for the SPSC ring buffer
 *
 * Copyright (C) 2026  Ahmad Rashed
 *
 * Tests cover:
 *  - Creation with various capacities (power-of-two rounding)
 *  - Push/pop round-trip correctness
 *  - Full-buffer push rejection
 *  - Empty-buffer pop rejection
 *  - FIFO ordering
 *  - rb_size() accuracy
 *  - Wrap-around (multiple full cycles)
 *  - NULL/edge-case safety
 */

#include "unity/unity.h"
#include "ring_buffer.h"

#include <stdint.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Helper types
 * ------------------------------------------------------------------------- */

typedef struct {
    uint32_t id;
    uint8_t  data[8];
} test_item_t;

static test_item_t make_item(uint32_t id)
{
    test_item_t item = { .id = id };
    for (int i = 0; i < 8; ++i) {
        item.data[i] = (uint8_t)(id + i);
    }
    return item;
}

/* -------------------------------------------------------------------------
 * Test cases
 * ------------------------------------------------------------------------- */

static void test_create_returns_non_null(void)
{
    ring_buffer_t *rb = rb_create(8u, sizeof(test_item_t));
    TEST_ASSERT_NOT_NULL(rb);
    rb_destroy(rb);
}

static void test_capacity_rounded_to_power_of_two(void)
{
    ring_buffer_t *rb = rb_create(7u, sizeof(test_item_t));
    TEST_ASSERT_NOT_NULL(rb);
    /* Capacity must be at least 7 (rounded to 8) */
    TEST_ASSERT_TRUE(rb_capacity(rb) >= 7u);
    rb_destroy(rb);
}

static void test_empty_buffer_pop_returns_false(void)
{
    ring_buffer_t *rb = rb_create(4u, sizeof(int));
    TEST_ASSERT_NOT_NULL(rb);

    int out = 0;
    TEST_ASSERT_FALSE(rb_pop(rb, &out));

    rb_destroy(rb);
}

static void test_push_pop_single_item(void)
{
    ring_buffer_t *rb = rb_create(4u, sizeof(test_item_t));
    TEST_ASSERT_NOT_NULL(rb);

    test_item_t in  = make_item(42u);
    test_item_t out;
    memset(&out, 0xFF, sizeof(out));

    TEST_ASSERT_TRUE(rb_push(rb, &in));
    TEST_ASSERT_TRUE(rb_pop(rb, &out));
    TEST_ASSERT_EQUAL_UINT(42u, out.id);
    TEST_ASSERT_EQUAL_INT(0, memcmp(in.data, out.data, 8u));

    rb_destroy(rb);
}

static void test_fifo_ordering(void)
{
    ring_buffer_t *rb = rb_create(8u, sizeof(uint32_t));
    TEST_ASSERT_NOT_NULL(rb);

    for (uint32_t i = 0u; i < 5u; ++i) {
        TEST_ASSERT_TRUE(rb_push(rb, &i));
    }

    for (uint32_t i = 0u; i < 5u; ++i) {
        uint32_t out = UINT32_MAX;
        TEST_ASSERT_TRUE(rb_pop(rb, &out));
        TEST_ASSERT_EQUAL_UINT(i, out);
    }

    rb_destroy(rb);
}

static void test_full_buffer_push_returns_false(void)
{
    /* Capacity 4 → real slots = 4 (one sentinel), usable = 3 */
    ring_buffer_t *rb = rb_create(3u, sizeof(uint32_t));
    TEST_ASSERT_NOT_NULL(rb);

    size_t cap = rb_capacity(rb);

    /* Fill the buffer */
    for (size_t i = 0u; i < cap; ++i) {
        uint32_t v = (uint32_t)i;
        TEST_ASSERT_TRUE(rb_push(rb, &v));
    }

    /* Next push must fail */
    uint32_t extra = 99u;
    TEST_ASSERT_FALSE(rb_push(rb, &extra));

    rb_destroy(rb);
}

static void test_size_tracking(void)
{
    ring_buffer_t *rb = rb_create(8u, sizeof(uint32_t));
    TEST_ASSERT_NOT_NULL(rb);

    TEST_ASSERT_EQUAL_SIZE_T(0u, rb_size(rb));

    uint32_t v = 1u;
    rb_push(rb, &v);
    TEST_ASSERT_EQUAL_SIZE_T(1u, rb_size(rb));

    v = 2u;
    rb_push(rb, &v);
    TEST_ASSERT_EQUAL_SIZE_T(2u, rb_size(rb));

    rb_pop(rb, &v);
    TEST_ASSERT_EQUAL_SIZE_T(1u, rb_size(rb));

    rb_pop(rb, &v);
    TEST_ASSERT_EQUAL_SIZE_T(0u, rb_size(rb));

    rb_destroy(rb);
}

static void test_wrap_around_multiple_cycles(void)
{
    ring_buffer_t *rb = rb_create(4u, sizeof(uint32_t));
    TEST_ASSERT_NOT_NULL(rb);

    /* Push and pop through the buffer 3× its capacity to exercise wrap */
    size_t cap    = rb_capacity(rb);
    size_t cycles = 3u;

    for (size_t cycle = 0u; cycle < cycles; ++cycle) {
        for (size_t i = 0u; i < cap; ++i) {
            uint32_t v = (uint32_t)(cycle * cap + i);
            TEST_ASSERT_TRUE(rb_push(rb, &v));
        }
        for (size_t i = 0u; i < cap; ++i) {
            uint32_t out = UINT32_MAX;
            TEST_ASSERT_TRUE(rb_pop(rb, &out));
            TEST_ASSERT_EQUAL_UINT((unsigned)(cycle * cap + i), out);
        }
    }

    rb_destroy(rb);
}

static void test_destroy_null_is_safe(void)
{
    rb_destroy(NULL);   /* Must not crash */
    TEST_PASS();
}

static void test_zero_item_size_returns_null(void)
{
    ring_buffer_t *rb = rb_create(8u, 0u);
    TEST_ASSERT_NULL(rb);
}

/* -------------------------------------------------------------------------
 * Test runner
 * ------------------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_create_returns_non_null);
    RUN_TEST(test_capacity_rounded_to_power_of_two);
    RUN_TEST(test_empty_buffer_pop_returns_false);
    RUN_TEST(test_push_pop_single_item);
    RUN_TEST(test_fifo_ordering);
    RUN_TEST(test_full_buffer_push_returns_false);
    RUN_TEST(test_size_tracking);
    RUN_TEST(test_wrap_around_multiple_cycles);
    RUN_TEST(test_destroy_null_is_safe);
    RUN_TEST(test_zero_item_size_returns_null);

    return UNITY_END();
}
