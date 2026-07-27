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
TEST(test_read_um_ctp_jetty, Run)
{
    ctx_t *c = NULL;
    char *src = NULL, *dst = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_UM, URMA_CTP);
    if (!c) goto cleanup;
    src = calloc(1, SEG_LEN); memset(src, 'A', SEG_LEN);
    tseg = register_seg(c, src, SEG_LEN);
    if (!tseg) goto cleanup;
    dst = calloc(1, SEG_LEN); memset(dst, 'Z', SEG_LEN);
    tj = import_self(c);
    if (!tj) goto cleanup;
    ret = post_read(c, tj, dst, src, tseg, SEG_LEN, 0x13);
    EXPECT_EQ(0, ret);
    n = poll_cr(c, &cr);
    EXPECT_EQ(1, n);
    EXPECT_EQ(0, cr.status);
    EXPECT_EQ(65, dst[0]);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
}

TEST(test_write_rm_ctp_jetty_bidir, Run)
{
    ctx_t *c = NULL;
    char *src = NULL, *dst = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_RM, URMA_CTP);
    if (!c) goto cleanup;
    dst = calloc(1, SEG_LEN); memset(dst, 'Z', SEG_LEN);
    tseg = register_seg(c, dst, SEG_LEN);
    if (!tseg) goto cleanup;
    src = calloc(1, SEG_LEN); memset(src, 'D', SEG_LEN);
    tj = import_self(c);
    if (!tj) goto cleanup;
    ret = post_write(c, tj, src, tseg, dst, SEG_LEN, 0x14);
    EXPECT_EQ(0, ret);
    n = poll_cr(c, &cr);
    EXPECT_EQ(1, n);
    EXPECT_EQ(0, cr.status);
    EXPECT_EQ(68, dst[0]);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
}

TEST(test_send_rc_ctp_jfr_bidir, Run)
{
    ctx_t *c = NULL;
    char *buf1 = NULL, *buf2 = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_RC, URMA_CTP);
    if (!c) goto cleanup;
    buf1 = calloc(1, SEG_LEN); memset(buf1, 'S', SEG_LEN);
    buf2 = calloc(1, SEG_LEN); memset(buf2, 'R', SEG_LEN);
    tj = bind_remote(c, c->ctx->eid, c->ctx->uasid, get_jetty_id(c));
    if (!tj) goto cleanup;
    post_recv(c, buf2, SEG_LEN, 0x15);
    ret = post_send(c, tj, buf1, SEG_LEN, 0x25);
    EXPECT_EQ(0, ret);
    n = poll_cr(c, &cr);
    EXPECT_EQ(1, n);
    EXPECT_EQ(0, cr.status);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(buf1); free(buf2);
    if (c) teardown(c);
}

TEST(test_read_rc_tp_jfr, Run)
{
    ctx_t *c = NULL;
    char *src = NULL, *dst = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_RC, URMA_UTP);
    if (!c) goto cleanup;
    src = calloc(1, SEG_LEN); memset(src, 'A', SEG_LEN);
    tseg = register_seg(c, src, SEG_LEN);
    if (!tseg) goto cleanup;
    dst = calloc(1, SEG_LEN); memset(dst, 'Z', SEG_LEN);
    tj = bind_remote(c, c->ctx->eid, c->ctx->uasid, get_jetty_id(c));
    if (!tj) goto cleanup;
    ret = post_read(c, tj, dst, src, tseg, SEG_LEN, 0x16);
    EXPECT_EQ(0, ret);
    n = poll_cr(c, &cr);
    EXPECT_EQ(1, n);
    EXPECT_EQ(0, cr.status);
    EXPECT_EQ(65, dst[0]);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
}

TEST(test_write_rc_ctp_jetty_bonding, Run)
{
    ctx_t *c = NULL;
    char *src = NULL, *dst = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_RC, URMA_CTP);
    if (!c) goto cleanup;
    dst = calloc(1, SEG_LEN); memset(dst, 'Z', SEG_LEN);
    tseg = register_seg(c, dst, SEG_LEN);
    if (!tseg) goto cleanup;
    src = calloc(1, SEG_LEN); memset(src, 'P', SEG_LEN);
    tj = bind_remote(c, c->ctx->eid, c->ctx->uasid, get_jetty_id(c));
    if (!tj) goto cleanup;
    ret = post_write(c, tj, src, tseg, dst, SEG_LEN, 0x17);
    EXPECT_EQ(0, ret);
    n = poll_cr(c, &cr);
    EXPECT_EQ(1, n);
    EXPECT_EQ(0, cr.status);
    EXPECT_EQ(80, dst[0]);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
}

TEST(test_send_rm_ctp_jetty_bonding_bidir, Run)
{
    ctx_t *c = NULL;
    char *buf1 = NULL, *buf2 = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_RM, URMA_CTP);
    if (!c) goto cleanup;
    buf1 = calloc(1, SEG_LEN); memset(buf1, 'Q', SEG_LEN);
    buf2 = calloc(1, SEG_LEN); memset(buf2, 'R', SEG_LEN);
    tj = bind_remote(c, c->ctx->eid, c->ctx->uasid, get_jetty_id(c));
    if (!tj) goto cleanup;
    post_recv(c, buf2, SEG_LEN, 0x18);
    ret = post_send(c, tj, buf1, SEG_LEN, 0x28);
    EXPECT_EQ(0, ret);
    n = poll_cr(c, &cr);
    EXPECT_EQ(1, n);
    EXPECT_EQ(0, cr.status);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(buf1); free(buf2);
    if (c) teardown(c);
}

TEST(test_read_um_tp_jfr_bonding_bidir, Run)
{
    ctx_t *c = NULL;
    char *src = NULL, *dst = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_UM, URMA_UTP);
    if (!c) goto cleanup;
    src = calloc(1, SEG_LEN); memset(src, 'A', SEG_LEN);
    tseg = register_seg(c, src, SEG_LEN);
    if (!tseg) goto cleanup;
    dst = calloc(1, SEG_LEN); memset(dst, 'Z', SEG_LEN);
    tj = import_self(c);
    if (!tj) goto cleanup;
    ret = post_read(c, tj, dst, src, tseg, SEG_LEN, 0x19);
    EXPECT_EQ(0, ret);
    n = poll_cr(c, &cr);
    EXPECT_EQ(1, n);
    EXPECT_EQ(0, cr.status);
    EXPECT_EQ(65, dst[0]);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
}

TEST(test_write_imm_rc_ctp_jetty_bonding, Run)
{
    ctx_t *c = NULL;
    char *src = NULL, *dst = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_RC, URMA_CTP);
    if (!c) goto cleanup;
    dst = calloc(1, SEG_LEN); memset(dst, 'Z', SEG_LEN);
    tseg = register_seg(c, dst, SEG_LEN);
    if (!tseg) goto cleanup;
    src = calloc(1, SEG_LEN); memset(src, 'X', SEG_LEN);
    tj = bind_remote(c, c->ctx->eid, c->ctx->uasid, get_jetty_id(c));
    if (!tj) goto cleanup;
    {
        urma_sge_t dsg = {0}; dsg.addr = (uint64_t)(uintptr_t)dst; dsg.len = SEG_LEN; dsg.tseg = tseg;
        urma_sg_t dst_sg = { .sge = &dsg, .num_sge = 1 };
        urma_sge_t ssg = {0}; ssg.addr = (uint64_t)(uintptr_t)src; ssg.len = SEG_LEN;
        urma_sg_t src_sg = { .sge = &ssg, .num_sge = 1 };
        urma_jfs_wr_t wr = {0};
        wr.opcode = URMA_OPC_WRITE_IMM;
        wr.flag.bs.complete_enable = 1;
        wr.tjetty = tj;
        wr.user_ctx = 0x20;
        wr.rw.dst = dst_sg;
        wr.rw.src = src_sg;
        wr.rw.notify_data = 0xBEEF;
        urma_jfs_wr_t *bad = NULL;
        ret = urma_post_jetty_send_wr(c->jetty, &wr, &bad);
    }
    EXPECT_EQ(0, ret);
    n = poll_cr(c, &cr);
    EXPECT_EQ(1, n);
    EXPECT_EQ(0, cr.status);
    EXPECT_EQ(88, dst[0]);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
}

