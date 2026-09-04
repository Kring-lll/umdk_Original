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
TEST(test_multi_post, Run)
{
    dt_ctx_t *c = NULL;
    char *src = NULL;
    char *dst = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_seg_t *dst_tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1;
    int i = 0;
    int got = 0;
    urma_cr_t cr = {};

    c = dt_setup_default(0);
    ASSERT_NE(nullptr, c);
    if (c == NULL) {
        goto cleanup;
    }

    src = static_cast<char *>(calloc(1, SEG_LEN));
    ASSERT_NE(nullptr, src);
    if (src == NULL) {
        goto cleanup;
    }
    memset(src, 'A', SEG_LEN);
    tseg = dt_register_seg(c, src, SEG_LEN);
    ASSERT_NE(nullptr, tseg);
    if (tseg == NULL) {
        goto cleanup;
    }

    dst = static_cast<char *>(calloc(1, SEG_LEN));
    ASSERT_NE(nullptr, dst);
    if (dst == NULL) {
        goto cleanup;
    }
    memset(dst, 'Z', SEG_LEN);
    dst_tseg = dt_register_seg(c, dst, SEG_LEN);
    ASSERT_NE(nullptr, dst_tseg);
    if (dst_tseg == NULL) {
        goto cleanup;
    }

    tj = dt_import_self(c);
    ASSERT_NE(nullptr, tj);
    if (tj == NULL) {
        goto cleanup;
    }

    for (i = 0; i < 8; i++) {
        ret = dt_post_read(c, tj, dst, dst_tseg, src, tseg, SEG_LEN, 0x10 + i);
        if (ret != 0) break;
    }
    EXPECT_EQ(0, ret);

    for (i = 0; i < 8; i++) {
        int n = dt_poll_cr(c, &cr);
        if (n > 0 && cr.status == 0) got++;
        else break;
    }
    EXPECT_EQ(8, got);
    EXPECT_EQ(65, dst[0]);

cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst);
    free(src);
    if (c) dt_teardown(c);
}
