/* SPDX-License-Identifier: GPL-3.0-or-later
 * test_frame_logger.c -- Unit tests for the frame logger module
 * Copyright (C) 2026  Ahmad Rashed
 */
#define _POSIX_C_SOURCE 200809L
#include "unity/unity.h"
#include "frame_logger.h"
#include <dirent.h>
#include <errno.h>
#include <linux/can.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TMP_DIR_TEMPLATE "/tmp/canmon_test_XXXXXX"
static char g_tmp_dir[64];

static void setup_tmp_dir(void)
{
    strncpy(g_tmp_dir, TMP_DIR_TEMPLATE, sizeof(g_tmp_dir));
    if (!mkdtemp(g_tmp_dir)) { perror("mkdtemp"); exit(EXIT_FAILURE); }
}

static void teardown_tmp_dir(void)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_tmp_dir);
    (void)system(cmd);
}

static frame_logger_cfg_t make_cfg(void)
{
    frame_logger_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_path        = g_tmp_dir;
    cfg.prefix           = "test";
    cfg.max_file_bytes   = FRAME_LOGGER_DEFAULT_MAX_BYTES;
    cfg.flush_interval_ms = 100u;
    cfg.max_rotations    = 5u;
    return cfg;
}

static int count_asc_files(const char *dir, const char *prefix)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, prefix, strlen(prefix)) == 0 &&
            strstr(ent->d_name, ".asc"))
            count++;
    }
    closedir(d);
    return count;
}

static size_t read_first_asc(const char *dir, const char *prefix,
                              char *buf, size_t buf_sz)
{
    DIR *d = opendir(dir);
    if (!d) return 0u;
    char path[512];
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, prefix, strlen(prefix)) == 0 &&
            strstr(ent->d_name, ".asc")) {
            snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
            closedir(d);
            FILE *fp = fopen(path, "r");
            if (!fp) return 0u;
            size_t n = fread(buf, 1u, buf_sz - 1u, fp);
            buf[n] = '\0';
            fclose(fp);
            return n;
        }
    }
    closedir(d);
    return 0u;
}

static struct can_frame make_frame(canid_t id, uint8_t dlc)
{
    struct can_frame f;
    memset(&f, 0, sizeof(f));
    f.can_id  = id;
    f.can_dlc = dlc;
    for (uint8_t i = 0u; i < dlc; ++i) f.data[i] = i;
    return f;
}

static void test_create_and_destroy(void)
{
    setup_tmp_dir();
    frame_logger_cfg_t cfg = make_cfg();
    frame_logger_t *l = frame_logger_create(&cfg);
    TEST_ASSERT_NOT_NULL(l);
    frame_logger_destroy(l);
    teardown_tmp_dir();
}

static void test_creates_asc_file_on_open(void)
{
    setup_tmp_dir();
    frame_logger_cfg_t cfg = make_cfg();
    frame_logger_t *l = frame_logger_create(&cfg);
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_TRUE(count_asc_files(g_tmp_dir, "test") >= 1);
    frame_logger_destroy(l);
    teardown_tmp_dir();
}

static void test_asc_header_written(void)
{
    setup_tmp_dir();
    frame_logger_cfg_t cfg = make_cfg();
    frame_logger_t *l = frame_logger_create(&cfg);
    TEST_ASSERT_NOT_NULL(l);
    frame_logger_flush(l);
    char buf[512];
    size_t n = read_first_asc(g_tmp_dir, "test", buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0u);
    TEST_ASSERT_TRUE(strstr(buf, "date") != NULL);
    TEST_ASSERT_TRUE(strstr(buf, "base hex") != NULL);
    frame_logger_destroy(l);
    teardown_tmp_dir();
}

static void test_write_and_flush_produces_asc_line(void)
{
    setup_tmp_dir();
    frame_logger_cfg_t cfg = make_cfg();
    frame_logger_t *l = frame_logger_create(&cfg);
    TEST_ASSERT_NOT_NULL(l);
    struct can_frame f = make_frame(0x1A0u, 4u);
    TEST_ASSERT_EQUAL_INT(0, frame_logger_write(l, &f, 1000000000ULL, 1u));
    TEST_ASSERT_EQUAL_INT(0, frame_logger_flush(l));
    char buf[1024];
    size_t n = read_first_asc(g_tmp_dir, "test", buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0u);
    TEST_ASSERT_TRUE(strstr(buf, "1A0") != NULL);
    TEST_ASSERT_TRUE(strstr(buf, "Rx") != NULL);
    frame_logger_destroy(l);
    teardown_tmp_dir();
}

static void test_multiple_writes_all_flushed(void)
{
    setup_tmp_dir();
    frame_logger_cfg_t cfg = make_cfg();
    frame_logger_t *l = frame_logger_create(&cfg);
    TEST_ASSERT_NOT_NULL(l);
    for (int i = 0; i < 20; ++i) {
        struct can_frame f = make_frame((canid_t)(0x100u + (unsigned)i), 8u);
        TEST_ASSERT_EQUAL_INT(0, frame_logger_write(l, &f,
            (uint64_t)i * 10000000ULL, 1u));
    }
    frame_logger_flush(l);
    char buf[8192];
    size_t n = read_first_asc(g_tmp_dir, "test", buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0u);
    int rx_count = 0;
    const char *p = buf;
    while ((p = strstr(p, "Rx")) != NULL) { rx_count++; p++; }
    TEST_ASSERT_TRUE(rx_count >= 20);
    frame_logger_destroy(l);
    teardown_tmp_dir();
}

static void test_destroy_null_is_safe(void)
{
    frame_logger_destroy(NULL);
    TEST_PASS();
}

static void test_null_config_returns_null(void)
{
    frame_logger_t *l = frame_logger_create(NULL);
    TEST_ASSERT_NULL(l);
}

static void test_rotation_creates_new_file(void)
{
    setup_tmp_dir();
    frame_logger_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_path        = g_tmp_dir;
    cfg.prefix           = "rot";
    cfg.max_file_bytes   = 256u;
    cfg.flush_interval_ms = 50u;
    cfg.max_rotations    = 5u;
    frame_logger_t *l = frame_logger_create(&cfg);
    TEST_ASSERT_NOT_NULL(l);
    for (int i = 0; i < 30; ++i) {
        struct can_frame f = make_frame(0x7FFu, 8u);
        frame_logger_write(l, &f, (uint64_t)i * 10000000ULL, 1u);
    }
    frame_logger_flush(l);
    struct timespec ts;
    ts.tv_sec  = 0;
    ts.tv_nsec = 200000000L;
    nanosleep(&ts, NULL);
    TEST_ASSERT_TRUE(count_asc_files(g_tmp_dir, "rot") >= 2);
    frame_logger_destroy(l);
    teardown_tmp_dir();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_and_destroy);
    RUN_TEST(test_creates_asc_file_on_open);
    RUN_TEST(test_asc_header_written);
    RUN_TEST(test_write_and_flush_produces_asc_line);
    RUN_TEST(test_multiple_writes_all_flushed);
    RUN_TEST(test_destroy_null_is_safe);
    RUN_TEST(test_null_config_returns_null);
    RUN_TEST(test_rotation_creates_new_file);
    return UNITY_END();
}
