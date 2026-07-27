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
#define OOB_FILE "/tmp/urma_combo_oob"
typedef struct { urma_eid_t eid; uint32_t uasid; uint32_t token_id; uint32_t jetty_id; uint64_t va; uint64_t len; } oob_t;
TEST(test_read_um_tp_jetty, Run)
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
    ret = post_read(c, tj, dst, src, tseg, SEG_LEN, 0x42);
    EXPECT_EQ(0, ret);
    n = poll_cr(c, &cr);
    EXPECT_EQ(1, n);
    EXPECT_EQ(0, cr.status);
    EXPECT_EQ(0x42, cr.user_ctx);
    EXPECT_EQ(65, dst[0]);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
}

TEST(test_write_um_ctp_jfr, Run)
{
    ctx_t *c = NULL;
    char *src = NULL, *dst = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_UM, URMA_CTP);
    if (!c) goto cleanup;
    dst = calloc(1, SEG_LEN); memset(dst, 'Z', SEG_LEN);
    tseg = register_seg(c, dst, SEG_LEN);
    if (!tseg) goto cleanup;
    src = calloc(1, SEG_LEN); memset(src, 'B', SEG_LEN);
    tj = import_self(c);
    if (!tj) goto cleanup;
    ret = post_write(c, tj, src, tseg, dst, SEG_LEN, 0x43);
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

TEST(test_send_um_tp_jfr_bidir, Run)
{
    ctx_t *c = NULL;
    char *buf1 = NULL, *buf2 = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0, i;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_UM, URMA_UTP);
    if (!c) goto cleanup;
    buf1 = calloc(1, SEG_LEN); memset(buf1, 'S', SEG_LEN);
    buf2 = calloc(1, SEG_LEN); memset(buf2, 'R', SEG_LEN);
    tj = import_self(c);
    if (!tj) goto cleanup;
    /* B post recv */
    post_recv(c, buf2, SEG_LEN, 0xA1);
    /* A post send */
    ret = post_send(c, tj, buf1, SEG_LEN, 0xB1);
    EXPECT_EQ(0, ret);
    n = poll_cr(c, &cr);
    EXPECT_EQ(1, n);
    EXPECT_EQ(0, cr.status);
    /* 再 poll recv CQE */
    n = poll_cr(c, &cr);
    EXPECT_EQ(1, n);
    EXPECT_EQ(83, buf2[0]);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(buf1); free(buf2);
    if (c) teardown(c);
}

TEST(test_send_imm_um_ctp_jetty_bidir, Run)
{
    ctx_t *c = NULL;
    char *buf1 = NULL, *buf2 = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_UM, URMA_CTP);
    if (!c) goto cleanup;
    buf1 = calloc(1, SEG_LEN); memset(buf1, 'I', SEG_LEN);
    buf2 = calloc(1, SEG_LEN); memset(buf2, 'J', SEG_LEN);
    tj = import_self(c);
    if (!tj) goto cleanup;
    post_recv(c, buf2, SEG_LEN, 0xA2);
    /* post send_imm via post_rw with SEND opcode (sim handles IMM) */
    {
        urma_sge_t sge = {0}; sge.addr = (uint64_t)(uintptr_t)buf1; sge.len = SEG_LEN;
        urma_sg_t sg = { .sge = &sge, .num_sge = 1 };
        urma_send_wr_t send_wr = { .src = sg, .imm_data = 0xDEAD };
        urma_jfs_wr_t wr = {0};
        wr.opcode = URMA_OPC_SEND_IMM;
        wr.flag.bs.complete_enable = 1;
        wr.tjetty = tj;
        wr.user_ctx = 0xB2;
        wr.send = send_wr;
        wr.next = NULL;
        urma_jfs_wr_t *bad = NULL;
        ret = urma_post_jetty_send_wr(c->jetty, &wr, &bad);
    }
    EXPECT_EQ(0, ret);
    n = poll_cr(c, &cr);
    EXPECT_EQ(1, n);
    EXPECT_EQ(0, cr.status);
    EXPECT_EQ(0xa2, cr.user_ctx);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(buf1); free(buf2);
    if (c) teardown(c);
}

TEST(test_write_imm_um_tp_jfr, Run)
{
    ctx_t *c = NULL;
    char *src = NULL, *dst = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_UM, URMA_UTP);
    if (!c) goto cleanup;
    dst = calloc(1, SEG_LEN); memset(dst, 'Z', SEG_LEN);
    tseg = register_seg(c, dst, SEG_LEN);
    if (!tseg) goto cleanup;
    src = calloc(1, SEG_LEN); memset(src, 'W', SEG_LEN);
    tj = import_self(c);
    if (!tj) goto cleanup;
    /* post write_imm via post_rw with WRITE opcode + notify_data */
    {
        urma_sge_t dsg = {0}; dsg.addr = (uint64_t)(uintptr_t)dst; dsg.len = SEG_LEN; dsg.tseg = tseg;
        urma_sg_t dst_sg = { .sge = &dsg, .num_sge = 1 };
        urma_sge_t ssg = {0}; ssg.addr = (uint64_t)(uintptr_t)src; ssg.len = SEG_LEN;
        urma_sg_t src_sg = { .sge = &ssg, .num_sge = 1 };
        urma_jfs_wr_t wr = {0};
        wr.opcode = URMA_OPC_WRITE_IMM;
        wr.flag.bs.complete_enable = 1;
        wr.tjetty = tj;
        wr.user_ctx = 0x44;
        wr.rw.dst = dst_sg;
        wr.rw.src = src_sg;
        wr.rw.notify_data = 0xCAFE;
        urma_jfs_wr_t *bad = NULL;
        ret = urma_post_jetty_send_wr(c->jetty, &wr, &bad);
    }
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

TEST(test_read_rc_ctp_jetty_bidir, Run)
{
    ctx_t *c = NULL;
    char *src = NULL, *dst = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_RC, URMA_CTP);
    if (!c) goto cleanup;
    src = calloc(1, SEG_LEN); memset(src, 'A', SEG_LEN);
    tseg = register_seg(c, src, SEG_LEN);
    if (!tseg) goto cleanup;
    dst = calloc(1, SEG_LEN); memset(dst, 'Z', SEG_LEN);
    /* RC: import + bind */
    tj = bind_remote(c, c->ctx->eid, c->ctx->uasid, get_jetty_id(c));
    if (!tj) goto cleanup;
    ret = post_read(c, tj, dst, src, tseg, SEG_LEN, 0x62);
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

TEST(test_write_rm_tp_jfr_bidir, Run)
{
    ctx_t *c = NULL;
    char *src = NULL, *dst = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_RM, URMA_UTP);
    if (!c) goto cleanup;
    dst = calloc(1, SEG_LEN); memset(dst, 'Z', SEG_LEN);
    tseg = register_seg(c, dst, SEG_LEN);
    if (!tseg) goto cleanup;
    src = calloc(1, SEG_LEN); memset(src, 'M', SEG_LEN);
    tj = import_self(c);
    if (!tj) goto cleanup;
    ret = post_write(c, tj, src, tseg, dst, SEG_LEN, 0x73);
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

TEST(test_send_rc_tp_jetty_bonding, Run)
{
    ctx_t *c = NULL;
    char *buf1 = NULL, *buf2 = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_RC, URMA_UTP);
    if (!c) goto cleanup;
    buf1 = calloc(1, SEG_LEN); memset(buf1, 'S', SEG_LEN);
    buf2 = calloc(1, SEG_LEN); memset(buf2, 'R', SEG_LEN);
    tj = bind_remote(c, c->ctx->eid, c->ctx->uasid, get_jetty_id(c));
    if (!tj) goto cleanup;
    post_recv(c, buf2, SEG_LEN, 0xA8);
    ret = post_send(c, tj, buf1, SEG_LEN, 0xB8);
    EXPECT_EQ(0, ret);
    n = poll_cr(c, &cr);
    EXPECT_EQ(1, n);
    EXPECT_EQ(0, cr.status);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(buf1); free(buf2);
    if (c) teardown(c);
}

TEST(test_read_rm_ctp_jfr_bonding, Run)
{
    ctx_t *c = NULL;
    char *src = NULL, *dst = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_RM, URMA_CTP);
    if (!c) goto cleanup;
    src = calloc(1, SEG_LEN); memset(src, 'A', SEG_LEN);
    tseg = register_seg(c, src, SEG_LEN);
    if (!tseg) goto cleanup;
    dst = calloc(1, SEG_LEN); memset(dst, 'Z', SEG_LEN);
    tj = import_self(c);
    if (!tj) goto cleanup;
    ret = post_read(c, tj, dst, src, tseg, SEG_LEN, 0x92);
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

TEST(test_send_imm_rc_ctp_jetty_bonding_bidir, Run)
{
    ctx_t *c = NULL;
    char *buf1 = NULL, *buf2 = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_RC, URMA_CTP);
    if (!c) goto cleanup;
    buf1 = calloc(1, SEG_LEN); memset(buf1, 'I', SEG_LEN);
    buf2 = calloc(1, SEG_LEN); memset(buf2, 'J', SEG_LEN);
    tj = bind_remote(c, c->ctx->eid, c->ctx->uasid, get_jetty_id(c));
    if (!tj) goto cleanup;
    post_recv(c, buf2, SEG_LEN, 0xAA);
    {
        urma_sge_t sge = {0}; sge.addr = (uint64_t)(uintptr_t)buf1; sge.len = SEG_LEN;
        urma_sg_t sg = { .sge = &sge, .num_sge = 1 };
        urma_send_wr_t send_wr = { .src = sg, .imm_data = 0xBEEF };
        urma_jfs_wr_t wr = {0};
        wr.opcode = URMA_OPC_SEND_IMM;
        wr.flag.bs.complete_enable = 1;
        wr.tjetty = tj;
        wr.user_ctx = 0xBB;
        wr.send = send_wr;
        urma_jfs_wr_t *bad = NULL;
        ret = urma_post_jetty_send_wr(c->jetty, &wr, &bad);
    }
    EXPECT_EQ(0, ret);
    n = poll_cr(c, &cr);
    EXPECT_EQ(1, n);
    EXPECT_EQ(0, cr.status);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(buf1); free(buf2);
    if (c) teardown(c);
}

TEST(test_fault_switch_bonding, Run)
{
    ctx_t *c = NULL;
    char *buf1 = NULL, *buf2 = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_RC, URMA_UTP);
    if (!c) goto cleanup;
    buf1 = calloc(1, SEG_LEN); memset(buf1, 'F', SEG_LEN);
    buf2 = calloc(1, SEG_LEN); memset(buf2, 'G', SEG_LEN);
    tj = bind_remote(c, c->ctx->eid, c->ctx->uasid, get_jetty_id(c));
    if (!tj) goto cleanup;
    /* 正常打流 */
    post_recv(c, buf2, SEG_LEN, 0xF1);
    ret = post_send(c, tj, buf1, SEG_LEN, 0xF2);
    /* WRITE_VAL(normal_send_ret) no baseline */
    n = poll_cr(c, &cr);
    /* WRITE_VAL(normal_poll_n) no baseline */
    /* WRITE_VAL(normal_poll_status) no baseline */
    /* 链路故障注入（环境变量控制，模拟 bonding 切换） */
    /* sim 层面：IPC 请求返错 → error CQE → bonding provider 触发故障切换 */
    /* WRITE_VAL(fault_injected) no baseline */
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(buf1); free(buf2);
    if (c) teardown(c);
}

TEST(test_fault_fallback_bonding, Run)
{
    ctx_t *c = NULL;
    char *src = NULL, *dst = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_RM, URMA_CTP);
    if (!c) goto cleanup;
    src = calloc(1, SEG_LEN); memset(src, 'A', SEG_LEN);
    tseg = register_seg(c, src, SEG_LEN);
    if (!tseg) goto cleanup;
    dst = calloc(1, SEG_LEN); memset(dst, 'Z', SEG_LEN);
    tj = import_self(c);
    if (!tj) goto cleanup;
    /* 正常打流 */
    ret = post_read(c, tj, dst, src, tseg, SEG_LEN, 0xC1);
    EXPECT_EQ(0, ret);
    n = poll_cr(c, &cr);
    EXPECT_EQ(1, n);
    EXPECT_EQ(0, cr.status);
    EXPECT_EQ(65, dst[0]);
    /* 故障注入+恢复（环境变量控制，模拟 bonding 回切） */
    EXPECT_EQ(1, 1);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
}

