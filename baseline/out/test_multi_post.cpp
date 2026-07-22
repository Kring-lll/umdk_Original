/* SPDX-License-Identifier: MIT */
/* 自动生成：gen_baseline gen_test <src.cpp> <expected.txt> <out.cpp>
 * 由 baseline 生成器从测试源 + 录制基准合成。改源改基准后重生成，勿手编。 */

#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include "bl_fixture.hpp"
#include "baseline_macros.h"

#define SEG_LEN 4096
#define NUM_POST 8
TEST(Generated, RecordedRun)
{
    ctx_t *c = NULL;
    char *src = NULL, *dst = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, i, got = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_UM);
    if (!c) goto cleanup;
    src = calloc(1, SEG_LEN); memset(src, 'A', SEG_LEN);
    tseg = register_seg(c, src, SEG_LEN);
    if (!tseg) goto cleanup;
    dst = calloc(1, SEG_LEN); memset(dst, 'Z', SEG_LEN);
    tj = import_self(c);
    if (!tj) goto cleanup;

    /* 连续 post NUM_POST 个 READ，每个 user_ctx 不同 */
    for (i = 0; i < NUM_POST; i++) {
        ret = post_read(c, tj, dst, src, tseg, SEG_LEN, 0x10 + i);
        if (ret != 0) break;
    }
    EXPECT_EQ(0, ret);
    EXPECT_EQ(8, NUM_POST);

    /* 逐个 poll，验全拿到 */
    for (i = 0; i < NUM_POST; i++) {
        int n = poll_cr(c, &cr);
        if (n > 0 && cr.status == 0) got++;
        else break;
    }
    EXPECT_EQ(8, got);
    EXPECT_EQ(8, NUM_POST);
    EXPECT_EQ(65, dst[0]);

cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
}
