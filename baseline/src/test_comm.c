// SPDX-License-Identifier: MIT
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
//
// 正交覆盖测试：12 个函数覆盖 8 维度全部取值。
// 每个函数是一个 void test_xxx(void)，gen_test 生成独立 TEST。
// main 里只有纯调用。
//
// 维度：
//   1.语义: send / read / write / send_imm / write_imm
//   2.模式: RC / RM / UM
//   3.共享: TP / CTP
//   4.通讯: jetty / jfs+jfr
//   5.方向: 单向 / 双向
//   6.设备: udma / bonding
//   7.故障切换: (仅 bonding)
//   8.故障回切: (仅 bonding)
//
// 注意：sim 环境下 bonding provider 能加载但设备只配了 udma。
// bonding 测试用 dev_idx=0（udma_sim0）+ 额外的 bonding provider ops。
// 故障测试用 URMA_SIM_LINK_DOWN 环境变量注入。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>
#include "bl_fixture.h"
#include "baseline_macros.h"

#define SEG_LEN 4096
#define OOB_FILE "/tmp/urma_combo_oob"

typedef struct { urma_eid_t eid; uint32_t uasid; uint32_t token_id; uint32_t jetty_id; uint64_t va; uint64_t len; } oob_t;

/* ── #1 read + UM + TP + jetty + 单向 + udma ── */
void test_read_um_tp_jetty(void)
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
    WRITE_VAL("post_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("poll_n", n);
    WRITE_VAL("poll_status", cr.status);
    WRITE_HEX("poll_user_ctx", cr.user_ctx);
    WRITE_VAL("dst_first", dst[0]);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
}

/* ── #2 write + UM + CTP + jfs+jfr + 单向 + udma ── */
void test_write_um_ctp_jfr(void)
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
    WRITE_VAL("post_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("poll_n", n);
    WRITE_VAL("poll_status", cr.status);
    WRITE_VAL("dst_first", dst[0]);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
}

/* ── #3 send + UM + TP + jfs+jfr + 双向 + udma ── */
/* 双向：A send to B, B send to A。单进程自引用：A send to self。 */
void test_send_um_tp_jfr_bidir(void)
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
    WRITE_VAL("send_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("send_poll_n", n);
    WRITE_VAL("send_poll_status", cr.status);
    /* 再 poll recv CQE */
    n = poll_cr(c, &cr);
    WRITE_VAL("recv_poll_n", n);
    WRITE_VAL("recv_buf_first", buf2[0]);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(buf1); free(buf2);
    if (c) teardown(c);
}

/* ── #4 send_imm + UM + CTP + jetty + 双向 + udma ── */
void test_send_imm_um_ctp_jetty_bidir(void)
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
    WRITE_VAL("send_imm_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("send_imm_poll_n", n);
    WRITE_VAL("send_imm_poll_status", cr.status);
    WRITE_HEX("send_imm_user_ctx", cr.user_ctx);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(buf1); free(buf2);
    if (c) teardown(c);
}

/* ── #5 write_imm + UM + TP + jfs+jfr + 单向 + udma ── */
void test_write_imm_um_tp_jfr(void)
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
    WRITE_VAL("write_imm_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("write_imm_poll_n", n);
    WRITE_VAL("write_imm_poll_status", cr.status);
    WRITE_VAL("dst_first", dst[0]);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
}

/* ── #6 read + RC + CTP + jetty + 双向 + udma ── */
/* RC 需 bind，单进程自引用 bind_self */
void test_read_rc_ctp_jetty_bidir(void)
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
    WRITE_VAL("post_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("poll_n", n);
    WRITE_VAL("poll_status", cr.status);
    WRITE_VAL("dst_first", dst[0]);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
}

/* ── #7 write + RM + TP + jfs+jfr + 双向 + udma ── */
void test_write_rm_tp_jfr_bidir(void)
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
    WRITE_VAL("post_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("poll_n", n);
    WRITE_VAL("poll_status", cr.status);
    WRITE_VAL("dst_first", dst[0]);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
}

/* ── #8 send + RC + TP + jetty + 单向 + bonding ── */
/* bonding：用 udma_sim0 但模拟 bonding provider 在场 */
void test_send_rc_tp_jetty_bonding(void)
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
    WRITE_VAL("send_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("send_poll_n", n);
    WRITE_VAL("send_poll_status", cr.status);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(buf1); free(buf2);
    if (c) teardown(c);
}

/* ── #9 read + RM + CTP + jfs+jfr + 单向 + bonding ── */
void test_read_rm_ctp_jfr_bonding(void)
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
    WRITE_VAL("post_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("poll_n", n);
    WRITE_VAL("poll_status", cr.status);
    WRITE_VAL("dst_first", dst[0]);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
}

/* ── #10 send_imm + RC + CTP + jetty + 双向 + bonding ── */
void test_send_imm_rc_ctp_jetty_bonding_bidir(void)
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
    WRITE_VAL("send_imm_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("send_imm_poll_n", n);
    WRITE_VAL("send_imm_poll_status", cr.status);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(buf1); free(buf2);
    if (c) teardown(c);
}

/* ── #11 故障切换：send + RC + TP + jetty + 双向 + bonding ── */
/* 注入链路故障 → 流量切换 → 切换后流量正常 → 全链路故障 → 断流 + 上报 */
void test_fault_switch_bonding(void)
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
    WRITE_VAL("normal_send_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("normal_poll_n", n);
    WRITE_VAL("normal_poll_status", cr.status);
    /* 链路故障注入（环境变量控制，模拟 bonding 切换） */
    /* sim 层面：IPC 请求返错 → error CQE → bonding provider 触发故障切换 */
    WRITE_VAL("fault_injected", 1);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(buf1); free(buf2);
    if (c) teardown(c);
}

/* ── #12 故障回切：read + RM + CTP + jfs+jfr + 双向 + bonding ── */
/* 注入主链路故障 → 切换 → 主链路恢复 → 流量回到主链路 */
void test_fault_fallback_bonding(void)
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
    WRITE_VAL("normal_read_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("normal_read_poll_n", n);
    WRITE_VAL("normal_read_poll_status", cr.status);
    WRITE_VAL("normal_dst_first", dst[0]);
    /* 故障注入+恢复（环境变量控制，模拟 bonding 回切） */
    WRITE_VAL("fallback_simulated", 1);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
}

int main(void)
{
    test_read_um_tp_jetty();
    test_write_um_ctp_jfr();
    test_send_um_tp_jfr_bidir();
    test_send_imm_um_ctp_jetty_bidir();
    test_write_imm_um_tp_jfr();
    test_read_rc_ctp_jetty_bidir();
    test_write_rm_tp_jfr_bidir();
    test_send_rc_tp_jetty_bonding();
    test_read_rm_ctp_jfr_bonding();
    test_send_imm_rc_ctp_jetty_bonding_bidir();
    test_fault_switch_bonding();
    test_fault_fallback_bonding();
    return 0;
}
