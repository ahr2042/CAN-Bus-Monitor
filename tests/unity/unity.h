/* SPDX-License-Identifier: MIT
 *
 * unity.h — Minimal Unity-compatible test framework (single-header subset)
 *
 * This is a from-scratch, GPL-3.0-compatible re-implementation of the Unity
 * test runner interface.  The original Unity project (ThrowTheSwitch/Unity)
 * is MIT licensed.  This file is an independent implementation providing the
 * same macro API so tests written against Unity compile without modification.
 *
 * Supported macros:
 *   UNITY_BEGIN()
 *   UNITY_END()
 *   RUN_TEST(func)
 *   TEST_ASSERT_TRUE(cond)
 *   TEST_ASSERT_FALSE(cond)
 *   TEST_ASSERT_EQUAL_INT(expected, actual)
 *   TEST_ASSERT_EQUAL_UINT(expected, actual)
 *   TEST_ASSERT_EQUAL_UINT64(expected, actual)
 *   TEST_ASSERT_EQUAL_SIZE_T(expected, actual)
 *   TEST_ASSERT_NULL(ptr)
 *   TEST_ASSERT_NOT_NULL(ptr)
 *   TEST_ASSERT_EQUAL_STRING(expected, actual)
 *   TEST_FAIL_MESSAGE(msg)
 *   TEST_PASS()
 */

#ifndef UNITY_H
#define UNITY_H

#include <inttypes.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Internal state
 * ------------------------------------------------------------------------- */
typedef struct {
    unsigned tests_run;
    unsigned tests_failed;
    unsigned tests_ignored;
    const char *current_test;
    const char *current_file;
    int         current_line;
    jmp_buf     jmp;
    int         jmp_active;
} unity_state_t;

extern unity_state_t Unity;

static inline void unity_fail_internal(const char *file, int line,
                                        const char *msg)
{
    fprintf(stderr, "%s:%d:%s:FAIL: %s\n",
            file, line, Unity.current_test, msg);
    Unity.tests_failed++;
    if (Unity.jmp_active) {
        longjmp(Unity.jmp, 1);
    }
}

/* -------------------------------------------------------------------------
 * Public macros
 * ------------------------------------------------------------------------- */

#define UNITY_BEGIN()  do { \
    memset(&Unity, 0, sizeof(Unity)); \
    printf("------- Running Tests -------\n"); \
} while (0)

#define UNITY_END()  unity_end_report()

static inline int unity_end_report(void)
{
    printf("\n%u Tests  %u Failures  %u Ignored\n",
           Unity.tests_run, Unity.tests_failed, Unity.tests_ignored);
    if (Unity.tests_failed == 0u) {
        printf("OK\n");
    } else {
        printf("FAIL\n");
    }
    return (int)Unity.tests_failed;
}

#define RUN_TEST(func)  do { \
    Unity.current_test = #func; \
    Unity.current_file = __FILE__; \
    Unity.tests_run++; \
    Unity.jmp_active = 1; \
    if (setjmp(Unity.jmp) == 0) { \
        func(); \
        printf("  PASS  " #func "\n"); \
    } \
    Unity.jmp_active = 0; \
} while (0)

/* --- Assertion helpers --------------------------------------------------- */

#define TEST_ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        char _msg[128]; \
        snprintf(_msg, sizeof(_msg), "Expected TRUE but was FALSE: " #cond); \
        unity_fail_internal(__FILE__, __LINE__, _msg); \
    } \
} while (0)

#define TEST_ASSERT_FALSE(cond)  TEST_ASSERT_TRUE(!(cond))

#define TEST_ASSERT_NULL(ptr) do { \
    if ((ptr) != NULL) { \
        unity_fail_internal(__FILE__, __LINE__, \
            "Expected NULL but was not NULL: " #ptr); \
    } \
} while (0)

#define TEST_ASSERT_NOT_NULL(ptr) do { \
    if ((ptr) == NULL) { \
        unity_fail_internal(__FILE__, __LINE__, \
            "Expected non-NULL but got NULL: " #ptr); \
    } \
} while (0)

#define TEST_ASSERT_EQUAL_INT(expected, actual) do { \
    int _e = (int)(expected); \
    int _a = (int)(actual); \
    if (_e != _a) { \
        char _msg[256]; \
        snprintf(_msg, sizeof(_msg), \
            "Expected %d but got %d (" #actual ")", _e, _a); \
        unity_fail_internal(__FILE__, __LINE__, _msg); \
    } \
} while (0)

#define TEST_ASSERT_EQUAL_UINT(expected, actual) do { \
    unsigned _e = (unsigned)(expected); \
    unsigned _a = (unsigned)(actual); \
    if (_e != _a) { \
        char _msg[256]; \
        snprintf(_msg, sizeof(_msg), \
            "Expected %u but got %u (" #actual ")", _e, _a); \
        unity_fail_internal(__FILE__, __LINE__, _msg); \
    } \
} while (0)

#define TEST_ASSERT_EQUAL_UINT64(expected, actual) do { \
    uint64_t _e = (uint64_t)(expected); \
    uint64_t _a = (uint64_t)(actual); \
    if (_e != _a) { \
        char _msg[256]; \
        snprintf(_msg, sizeof(_msg), \
            "Expected %" PRIu64 " but got %" PRIu64 " (" #actual ")", _e, _a); \
        unity_fail_internal(__FILE__, __LINE__, _msg); \
    } \
} while (0)

#define TEST_ASSERT_EQUAL_SIZE_T(expected, actual) do { \
    size_t _e = (size_t)(expected); \
    size_t _a = (size_t)(actual); \
    if (_e != _a) { \
        char _msg[256]; \
        snprintf(_msg, sizeof(_msg), \
            "Expected %zu but got %zu (" #actual ")", _e, _a); \
        unity_fail_internal(__FILE__, __LINE__, _msg); \
    } \
} while (0)

#define TEST_ASSERT_EQUAL_STRING(expected, actual) do { \
    const char *_e = (expected); \
    const char *_a = (actual); \
    if (_e == NULL || _a == NULL || strcmp(_e, _a) != 0) { \
        char _msg[256]; \
        snprintf(_msg, sizeof(_msg), \
            "Expected \"%s\" but got \"%s\" (" #actual ")", \
            _e ? _e : "(null)", _a ? _a : "(null)"); \
        unity_fail_internal(__FILE__, __LINE__, _msg); \
    } \
} while (0)

#define TEST_FAIL_MESSAGE(msg) \
    unity_fail_internal(__FILE__, __LINE__, (msg))

#define TEST_PASS()  do { /* nothing */ } while (0)

/* -------------------------------------------------------------------------
 * Implementation file (single-TU instantiation)
 * ------------------------------------------------------------------------- */
#ifdef UNITY_IMPLEMENTATION
unity_state_t Unity;
#endif

#endif /* UNITY_H */
