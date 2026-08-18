/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * urma_sim 单元测试（UT）：测 urma_sim_mmap_decode_offset（urma_sim_res.c）。
 *
 * 仿 umdk/test/urma/ut 的 gtest 风格，纯 C 自实现断言宏（容器无 gtest 库，故不依赖 GTest）。
 * 链接 liburma_sim.so 调真函数（非 inline 副本）——测的是 sim 真实导出代码。
 *
 * 被测函数（urma_sim_res.c:48）：
 *   int urma_sim_mmap_decode_offset(off_t offset, int page_size, uint32_t *cmd, uint32_t *idx)
 *   反解 mmap offset：v = offset/page_size; cmd = v & 0xf; idx = (v>>4) & 0xfffffff
 * 编码（真 udma udma_u_common.h:get_mmap_offset）：
 *   offset = ((idx & 0xfffffff) << 4 | (cmd & 0xf)) * page_size
 *
 * 构建：通过 sim 的 CMakeLists（option BUILD_SIM_UT=ON）编出 urma_sim_mmap_decode_ut，
 *   链接 liburma_sim.so（urma_sim_mmap_decode_offset 在 urma_sim.version 的 URMA_SIM_API 块导出）。
 */

#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>   /* off_t */
#include <string.h>

#include "urma_sim_res.h"   /* urma_sim_mmap_decode_offset 声明 */

/* 真 udma 编码（udma_u_common.h:get_mmap_offset），构造测试输入做往返验证。 */
static off_t encode_offset(uint32_t idx, int page_size, uint32_t cmd)
{
    unsigned long v = ((unsigned long)(idx & 0xfffffff) << 4) | (unsigned long)(cmd & 0xf);
    return (off_t)v * page_size;
}

/* === 仿 gtest 的最小断言框架 === */
static int g_pass = 0, g_fail = 0;

#define EXPECT_EQ(a, b) do { \
    long _a = (long)(a), _b = (long)(b); \
    if (_a == _b) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: EXPECT_EQ(%ld, %ld)\n", __FILE__, __LINE__, _a, _b); } \
} while (0)

#define TEST(name) static void name(void)

/* === 测试用例 === */

/* case1: 基本往返——编码 (idx=4101, cmd=JETTY_DSQE=2, page=4096) 再反解，应还原。 */
TEST(test_encode_decode_roundtrip)
{
    uint32_t idx = 4101, cmd = 2;   /* UDMA_MMAP_JETTY_DSQE=2 */
    int page = 4096;
    off_t off = encode_offset(idx, page, cmd);
    uint32_t g_cmd = 0, g_idx = 0;
    EXPECT_EQ(0, urma_sim_mmap_decode_offset(off, page, &g_cmd, &g_idx));
    EXPECT_EQ((long)cmd, (long)g_cmd);
    EXPECT_EQ((long)idx, (long)g_idx);
}

/* case2: 各 cmd 值（0..15）都正确提取低 4 位。 */
TEST(test_cmd_low_nibble)
{
    int page = 4096;
    for (uint32_t cmd = 0; cmd <= 0xf; cmd++) {
        off_t off = encode_offset(100, page, cmd);
        uint32_t g_cmd = 0, g_idx = 0;
        urma_sim_mmap_decode_offset(off, page, &g_cmd, &g_idx);
        EXPECT_EQ((long)cmd, (long)g_cmd);
        EXPECT_EQ(100L, (long)g_idx);
    }
}

/* case3: idx 28 位上限（0xfffffff）能正确还原，不溢出。 */
TEST(test_idx_max_28bit)
{
    uint32_t idx = 0xfffffff;  /* MAP_INDEX_MASK 上限 */
    int page = 4096;
    off_t off = encode_offset(idx, page, 0x3);
    uint32_t g_cmd = 0, g_idx = 0;
    urma_sim_mmap_decode_offset(off, page, &g_cmd, &g_idx);
    EXPECT_EQ(0x3L, (long)g_cmd);
    EXPECT_EQ((long)idx, (long)g_idx);
}

/* case4: page_size 无效（<=0）返回 -1（边界）。 */
TEST(test_invalid_page_size)
{
    uint32_t cmd = 0, idx = 0;
    EXPECT_EQ(-1, urma_sim_mmap_decode_offset(0x1000, 0, &cmd, &idx));
    EXPECT_EQ(-1, urma_sim_mmap_decode_offset(0x1000, -1, &cmd, &idx));
}

/* case5: cmd 高于 4 位会被截断（cmd & 0xf，真 udma MAP_COMMAND_MASK）。
 * 编码时 cmd=0x1f 只存低 4 位 0xf，反解也得 0xf。 */
TEST(test_cmd_truncated_to_4bit)
{
    uint32_t idx = 5;
    int page = 4096;
    off_t off = encode_offset(idx, page, 0x1f);
    uint32_t g_cmd = 0, g_idx = 0;
    urma_sim_mmap_decode_offset(off, page, &g_cmd, &g_idx);
    EXPECT_EQ(0xfL, (long)g_cmd);
    EXPECT_EQ((long)idx, (long)g_idx);
}

/* case6: offset=0 + page=4096 → v=0 → cmd=0, idx=0（零值边界）。 */
TEST(test_zero_offset)
{
    uint32_t cmd = 0xffff, idx = 0xffff;   /* 故意非零，验被覆盖 */
    EXPECT_EQ(0, urma_sim_mmap_decode_offset(0, 4096, &cmd, &idx));
    EXPECT_EQ(0L, (long)cmd);
    EXPECT_EQ(0L, (long)idx);
}

/* case7: page_size=1（最小页）+ 任意 idx/cmd 仍正确（offset 不放大）。 */
TEST(test_page_size_one)
{
    uint32_t idx = 0x12345, cmd = 0xa;
    off_t off = encode_offset(idx, 1, cmd);
    uint32_t g_cmd = 0, g_idx = 0;
    urma_sim_mmap_decode_offset(off, 1, &g_cmd, &g_idx);
    EXPECT_EQ((long)cmd, (long)g_cmd);
    EXPECT_EQ((long)idx, (long)g_idx);
}

int main(void)
{
    printf("== urma_sim_mmap_decode_offset unit tests ==\n");
    test_encode_decode_roundtrip();
    test_cmd_low_nibble();
    test_idx_max_28bit();
    test_invalid_page_size();
    test_cmd_truncated_to_4bit();
    test_zero_offset();
    test_page_size_one();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    printf("TEST %s\n", g_fail == 0 ? "PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}
