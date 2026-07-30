/* SPDX-License-Identifier: MIT */
/* 自动生成：gen_baseline gen_test <src.c> <expected.txt> <out.cpp>
 * 由 baseline 生成器从测试源 + 录制基准合成。改源改基准后重生成，勿手编。 */

#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <unistd.h>
#include "bl_fixture.hpp"
#include "baseline_macros.h"

#define SEG_LEN 4096
TEST(test_cas, Run)
{
    ctx_t *c = NULL;
    char *abuf = NULL, *obuf = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    c = setup_default(0);
    if (!c) goto cleanup;
    abuf = calloc(1, SEG_LEN); *(uint64_t *)abuf = 0x100;
    tseg = register_seg(c, abuf, SEG_LEN);
    if (!tseg) goto cleanup;
    obuf = calloc(1, 8);
    tj = import_self(c);
    if (!tj) goto cleanup;
    ret = post_cas(c, tj, abuf, tseg, obuf, 0x100, 0x200, 8, 0xC1);
    EXPECT_EQ(0, ret);
    n = poll_cr(c, &cr);
    EXPECT_EQ(1, n);
    EXPECT_EQ(0, cr.status);
    EXPECT_EQ(0x200, *(uint64_t *)abuf);
    EXPECT_EQ(0x100, *(uint64_t *)obuf);
    *(uint64_t *)obuf = 0;
    ret = post_cas(c, tj, abuf, tseg, obuf, 0xFFF, 0xAAA, 8, 0xC2);
    EXPECT_EQ(0, ret);
    n = poll_cr(c, &cr);
    EXPECT_EQ(1, n);
    EXPECT_EQ(0, cr.status);
    EXPECT_EQ(0x200, *(uint64_t *)abuf);
    EXPECT_EQ(0x200, *(uint64_t *)obuf);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(abuf); free(obuf);
    if (c) teardown(c);
}

