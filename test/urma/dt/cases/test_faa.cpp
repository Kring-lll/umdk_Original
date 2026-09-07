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
TEST(test_faa, Run)
{
    dt_ctx_t *c = NULL;
    char *abuf = NULL;
    char *obuf = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_seg_t *obuf_tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1;
    int n = 0;
    urma_cr_t cr = {};

    c = dt_setup_default(0);
    ASSERT_NE(nullptr, c);
    if (c == NULL) {
        goto cleanup;
    }

    abuf = static_cast<char *>(calloc(1, SEG_LEN));
    ASSERT_NE(nullptr, abuf);
    if (abuf == NULL) {
        goto cleanup;
    }
    *(reinterpret_cast<uint64_t *>(abuf)) = 0x200;
    tseg = dt_register_seg(c, abuf, SEG_LEN);
    ASSERT_NE(nullptr, tseg);
    if (tseg == NULL) {
        goto cleanup;
    }

    obuf = static_cast<char *>(calloc(1, 8));
    ASSERT_NE(nullptr, obuf);
    if (obuf == NULL) {
        goto cleanup;
    }
    obuf_tseg = dt_register_seg(c, obuf, 8);
    ASSERT_NE(nullptr, obuf_tseg);
    if (obuf_tseg == NULL) {
        goto cleanup;
    }

    tj = dt_import_self(c);
    ASSERT_NE(nullptr, tj);
    if (tj == NULL) {
        goto cleanup;
    }

    ret = dt_post_faa(c, tj, abuf, tseg, obuf, obuf_tseg, 5, 8, 0xF1);
    EXPECT_EQ(0, ret);
    n = dt_poll_cr(c, &cr);
    EXPECT_EQ(1, n);
    EXPECT_EQ(0, cr.status);
    EXPECT_EQ(0x205, *(reinterpret_cast<uint64_t *>(abuf)));
    EXPECT_EQ(0x200, *(reinterpret_cast<uint64_t *>(obuf)));

cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(abuf);
    free(obuf);
    if (c) dt_teardown(c);
}
