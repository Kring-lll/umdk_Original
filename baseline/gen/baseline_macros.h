/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * 门禁式回归测试的宏标注。测试代码用这些宏标注"要验什么"，跑时输出固定格式行，
 * 生成器（gen_baseline.c）逐行读测试代码的宏 + 输出，一一对应生成基准/比对。
 *
 * 输出格式（机器可解析，生成器按此匹配）：
 *   BASELINE|<file>|<line>|<kind>|<name>|<value>
 * 例：
 *   BASELINE|test_baseline_demo.c|42|VAL|poll_status|0
 *   BASELINE|test_baseline_demo.c|45|NONEMPTY|tjetty|0x44ea00
 *   BASELINE|test_baseline_demo.c|48|STR|recv_buf|Hello
 *
 * 宏分类（决定生成器产出的断言类型）：
 *   WRITE_VAL(name, val)      —— 输出值，期望逐次跑不变（逻辑值：status/user_ctx/opcode/len）。
 *                                  生成器产 EXPECT_EQ(name, <录制的值>)。
 *   ASSERT_NONEMPTY(name, p)   —— 验非空（地址/handle，不比值，ASLR/递增不稳）。
 *                                  生成器产 EXPECT_NE(name, NULL)。
 *   WRITE_STR(name, s)       —— 比字符串（buf 内容、固定 payload）。
 *                                  生成器产 EXPECT_STREQ(name, "<录制的串>")。
 *
 * 用法：测试代码里在该验的地方调宏，跑一次 → 输出被重定向到文件 → 生成器读它 + 读测试源 → 生成基准。
 */

#ifndef URMA_BASELINE_MACROS_H
#define URMA_BASELINE_MACROS_H

#include <stdio.h>
#include <string.h>

/* 内部：输出一行 BASELINE|file|line|kind|name|value
 * name/value 用宏字符串化，line 用 __LINE__。value 按 kind 格式化。 */
#define _BL_LINE(file, kind, name, valstr) \
    fprintf(stderr, "BASELINE|%s|%d|%s|%s|%s\n", file, __LINE__, kind, name, valstr)

/* WRITE_VAL：输出数值（int/uint/hex）。生成器产 EXPECT_EQ。 */
#define WRITE_VAL(name, val) do { \
    char _buf[32]; \
    snprintf(_buf, sizeof(_buf), "%d", (int)(long)(val)); \
    _BL_LINE(__FILE__, "VAL", name, _buf); \
} while (0)

/* WRITE_HEX：输出 hex 值（user_ctx/handle 类，要逐字节比但用 hex）。生成器产 EXPECT_EQ_HEX。 */
#define WRITE_HEX(name, val) do { \
    char _buf[32]; \
    snprintf(_buf, sizeof(_buf), "0x%lx", (unsigned long)(val)); \
    _BL_LINE(__FILE__, "HEX", name, _buf); \
} while (0)

/* ASSERT_NONEMPTY：验非空（地址/handle）。生成器产 EXPECT_NE(name, NULL)，不比值。 */
#define ASSERT_NONEMPTY(name, p) do { \
    char _buf[32]; \
    snprintf(_buf, sizeof(_buf), "%p", (void *)(uintptr_t)(p)); \
    _BL_LINE(__FILE__, "NONEMPTY", name, _buf); \
} while (0)

/* WRITE_STR：比字符串（buf 内容）。生成器产 EXPECT_STREQ。 */
#define WRITE_STR(name, s) do { \
    /* 截到首个换行，避免格式破坏 */ \
    const char *_s = (s); \
    char _buf[128]; \
    if (_s != NULL) { \
        size_t _n = strchr(_s, '\n') ? (size_t)(strchr(_s, '\n') - _s) : strlen(_s); \
        if (_n >= sizeof(_buf)) _n = sizeof(_buf) - 1; \
        memcpy(_buf, _s, _n); _buf[_n] = '\0'; \
    } else { _buf[0] = '\0'; } \
    _BL_LINE(__FILE__, "STR", name, _buf); \
} while (0)

#endif /* URMA_BASELINE_MACROS_H */
