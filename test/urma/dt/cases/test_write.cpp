/* SPDX-License-Identifier: MIT */

#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <unistd.h>
#include "dt_fixture.hpp"
#include "baseline_macros.h"

#define SEG_LEN 4096
TEST(test_write, Run)
{
    dt_ctx_t *c = NULL;
    char *src = NULL;
    char *dst = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_seg_t *src_tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1;
    int n = 0;
    urma_cr_t cr = {};

    c = dt_setup_default(0);
    ASSERT_NE(nullptr, c);
    if (c == NULL) {
        goto cleanup;
    }
    ASSERT_NE(nullptr, c->jfc);
    ASSERT_NE(nullptr, c->jfr);
    ASSERT_NE(nullptr, c->jetty);
    if (c->jfc == NULL || c->jfr == NULL || c->jetty == NULL) {
        goto cleanup;
    }

    dst = static_cast<char *>(calloc(1, SEG_LEN));
    ASSERT_NE(nullptr, dst);
    if (dst == NULL) {
        goto cleanup;
    }
    memset(dst, 'Z', SEG_LEN);
    tseg = dt_register_seg(c, dst, SEG_LEN);
    ASSERT_NE(nullptr, tseg);
    if (tseg == NULL) {
        goto cleanup;
    }

    src = static_cast<char *>(calloc(1, SEG_LEN));
    ASSERT_NE(nullptr, src);
    if (src == NULL) {
        goto cleanup;
    }
    memset(src, 'B', SEG_LEN);
    src_tseg = dt_register_seg(c, src, SEG_LEN);
    ASSERT_NE(nullptr, src_tseg);
    if (src_tseg == NULL) {
        goto cleanup;
    }

    tj = dt_import_self(c);
    ASSERT_NE(nullptr, tj);
    if (tj == NULL) {
        goto cleanup;
    }

    ret = dt_post_write(c, tj, src, src_tseg, dst, tseg, SEG_LEN, 0x43);
    EXPECT_EQ(0, ret);
    n = dt_poll_cr(c, &cr);
    EXPECT_EQ(1, n);
    EXPECT_EQ(0, cr.status);
    EXPECT_EQ(0x43, cr.user_ctx);
    EXPECT_EQ(66, dst[0]);
    EXPECT_EQ(0, strncmp("BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB", dst, strlen("BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB")));

cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst);
    free(src);
    if (c) dt_teardown(c);
}
