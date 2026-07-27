/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 */

/* 自动生成：由 cases 源样例和 expected 基准合成。改源改基准后重生成，勿手编。 */

#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include "bl_fixture.hpp"
#include "baseline_macros.h"

#define SEG_LEN 4096
TEST(Generated, RecordedRun)
{
    ctx_t *c = NULL;
    char *src = NULL, *dst = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {};

    c = setup(0, URMA_TM_UM);
    EXPECT_NE(nullptr, c);
    if (c == NULL) goto cleanup;
    EXPECT_NE(nullptr, c->jfc);
    EXPECT_NE(nullptr, c->jfr);
    EXPECT_NE(nullptr, c->jetty);
    if (c->jfc == NULL || c->jfr == NULL || c->jetty == NULL) goto cleanup;

    src = static_cast<char *>(calloc(1, SEG_LEN));
    EXPECT_NE(nullptr, src);
    if (src == NULL) goto cleanup;
    memset(src, 'A', SEG_LEN);
    tseg = register_seg(c, src, SEG_LEN);
    EXPECT_NE(nullptr, tseg);
    if (tseg == NULL) goto cleanup;

    dst = static_cast<char *>(calloc(1, SEG_LEN));
    EXPECT_NE(nullptr, dst);
    if (dst == NULL) goto cleanup;
    memset(dst, 'Z', SEG_LEN);
    tj = import_self(c);
    EXPECT_NE(nullptr, tj);
    if (tj == NULL) goto cleanup;

    ret = post_read(c, tj, dst, src, tseg, SEG_LEN, 0x42);
    EXPECT_EQ(0, ret);

    n = poll_cr(c, &cr);
    EXPECT_EQ(1, n);
    EXPECT_EQ(0, cr.status);
    EXPECT_EQ(0x42, cr.user_ctx);
    EXPECT_EQ(0, strncmp("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", dst, strlen("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA")));
    EXPECT_EQ(65, dst[0]);

cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
}
