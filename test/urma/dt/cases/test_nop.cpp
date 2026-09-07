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
TEST(test_nop, Run)
{
    dt_ctx_t *c = NULL;
    char *buf = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1;
    int n = 0;
    urma_cr_t cr = {};

    c = dt_setup_default(0);
    ASSERT_NE(nullptr, c);
    if (c == NULL) {
        goto cleanup;
    }

    buf = static_cast<char *>(calloc(1, SEG_LEN));
    ASSERT_NE(nullptr, buf);
    if (buf == NULL) {
        goto cleanup;
    }
    memset(buf, 'X', SEG_LEN);
    tseg = dt_register_seg(c, buf, SEG_LEN);
    ASSERT_NE(nullptr, tseg);
    if (tseg == NULL) {
        goto cleanup;
    }

    tj = dt_import_self(c);
    ASSERT_NE(nullptr, tj);
    if (tj == NULL) {
        goto cleanup;
    }

    ret = dt_post_rw(c, tj, URMA_OPC_NOP, buf, tseg, buf, tseg, SEG_LEN, 0x4E, 0, 0);
    EXPECT_EQ(0, ret);
    n = dt_poll_cr(c, &cr);
    EXPECT_EQ(1, n);
    EXPECT_EQ(0, cr.status);
    EXPECT_EQ(0x4e, cr.user_ctx);
    EXPECT_EQ(88, buf[0]);

cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(buf);
    if (c) dt_teardown(c);
}
