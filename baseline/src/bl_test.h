/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * 纯 C 自实现的 gtest 风格测试框架（不依赖 gtest 库，gcc 直接编）。
 *
 * 用法：
 *   TEST(Suite, Name) { EXPECT_EQ(1, x); ... }
 *   // main 由本头提供，自动跑所有 TEST（constructor 自动注册）
 *
 * 宏：
 *   TEST(suite, name)     —— 定义测试函数 + 自动注册
 *   EXPECT_EQ(exp, act)   —— 值相等（不等 FAIL，继续）
 *   EXPECT_NE(exp, act)   —— 值不等
 *   EXPECT_STREQ(exp, act)—— 字符串相等
 *   EXPECT_STRNE(exp, act)—— 字符串不等
 *   ASSERT_NE(exp, act)   —— 值不等（不等 FAIL + return，停止本测试）
 *   ASSERT_NE_NULL(p)     —— 指针非空（空 FAIL + return）
 */

#ifndef BL_TEST_H
#define BL_TEST_H

#include <stdio.h>
#include <string.h>

static int g_bl_pass = 0;
static int g_bl_fail = 0;

typedef void (*bl_test_fn)(void);
static bl_test_fn g_bl_tests[128];
static int g_bl_test_cnt = 0;
static const char *g_bl_cur_test = "";

/* TEST 定义函数 + constructor 自动注册 */
#define TEST(suite, name) \
    static void suite##_##name(void); \
    __attribute__((constructor)) static void _bl_reg_##suite##_##name(void) { \
        g_bl_tests[g_bl_test_cnt++] = suite##_##name; \
    } \
    static void suite##_##name(void)

#define EXPECT_EQ(exp, act) do { \
    long _e = (long)(exp), _a = (long)(act); \
    if (_e == _a) { g_bl_pass++; } \
    else { g_bl_fail++; printf("  FAIL: EXPECT_EQ(%ld, %ld) at line %d\n", _e, _a, __LINE__); } \
} while (0)

#define EXPECT_NE(exp, act) do { \
    long _e = (long)(exp), _a = (long)(act); \
    if (_e != _a) { g_bl_pass++; } \
    else { g_bl_fail++; printf("  FAIL: EXPECT_NE(%ld, %ld) at line %d\n", _e, _a, __LINE__); } \
} while (0)

#define EXPECT_STREQ(exp, act) do { \
    const char *_e = (exp), *_a = (act); \
    if (_e && _a && strcmp(_e, _a) == 0) { g_bl_pass++; } \
    else { g_bl_fail++; printf("  FAIL: EXPECT_STREQ at line %d\n", __LINE__); } \
} while (0)

#define EXPECT_STRNE(exp, act) do { \
    const char *_e = (exp), *_a = (act); \
    if (!_e || !_a || strcmp(_e, _a) != 0) { g_bl_pass++; } \
    else { g_bl_fail++; printf("  FAIL: EXPECT_STRNE at line %d\n", __LINE__); } \
} while (0)

/* ASSERT：失败后 return（停止本测试） */
#define ASSERT_NE(exp, act) do { \
    long _e = (long)(exp), _a = (long)(act); \
    if (_e == _a) { g_bl_fail++; printf("  FAIL: ASSERT_NE(%ld, %ld) at line %d\n", _e, _a, __LINE__); return; } \
    g_bl_pass++; \
} while (0)

#define ASSERT_NE_NULL(p) do { \
    if ((void *)(p) == NULL) { g_bl_fail++; printf("  FAIL: ASSERT_NE_NULL at line %d\n", __LINE__); return; } \
    g_bl_pass++; \
} while (0)

#endif /* BL_TEST_H */

/* main 由头提供（放 #endif 后，避免重复 include 时 main 重复） */
#ifndef BL_TEST_MAIN
#define BL_TEST_MAIN

int main(void)
{
    for (int i = 0; i < g_bl_test_cnt; i++) {
        g_bl_tests[i]();
    }
    printf("\n%d passed, %d failed\n", g_bl_pass, g_bl_fail);
    printf("TEST %s\n", g_bl_fail == 0 ? "PASS" : "FAIL");
    return g_bl_fail == 0 ? 0 : 1;
}

#endif
