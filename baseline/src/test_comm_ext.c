// SPDX-License-Identifier: MIT
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
//
// 正交覆盖测试扩展集：补充 #13-#20，覆盖原矩阵未覆盖的组合。
// #13 read + UM + CTP + jetty + 单向 + udma (补 CTP+jetty+read+UM)
// #14 write + RM + CTP + jetty + 双向 + udma (补 RM+CTP+jetty+write)
// #15 send + RC + CTP + jfs+jfr + 双向 + udma (补 RC+CTP+jfr+send)
// #16 read + RC + TP + jfs+jfr + 单向 + udma (补 RC+TP+jfr+read)
// #17 write + RC + CTP + jetty + 单向 + bonding (补 RC+CTP+jetty+write+bonding)
// #18 send + RM + CTP + jetty + 双向 + bonding (补 RM+CTP+jetty+send+bonding)
// #19 read + UM + TP + jfs+jfr + 双向 + bonding (补 UM+TP+jfr+read+bonding)
// #20 write_imm + RC + CTP + jetty + 单向 + bonding (补 write_imm+RC+bonding)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "bl_fixture.h"
#include "baseline_macros.h"

#define SEG_LEN 4096

void test_read_um_ctp_jetty(void)
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

void test_write_rm_ctp_jetty_bidir(void)
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
    WRITE_VAL("write_rm_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("write_rm_poll_n", n);
    WRITE_VAL("write_rm_poll_status", cr.status);
    WRITE_VAL("write_rm_dst_first", dst[0]);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
}

void test_send_rc_ctp_jfr_bidir(void)
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
    WRITE_VAL("send_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("send_poll_n", n);
    WRITE_VAL("send_poll_status", cr.status);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(buf1); free(buf2);
    if (c) teardown(c);
}

void test_read_rc_tp_jfr(void)
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

void test_write_rc_ctp_jetty_bonding(void)
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
    WRITE_VAL("write_rc_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("write_rc_poll_n", n);
    WRITE_VAL("write_rc_poll_status", cr.status);
    WRITE_VAL("write_rc_dst_first", dst[0]);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
}

void test_send_rm_ctp_jetty_bonding_bidir(void)
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
    WRITE_VAL("send_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("send_poll_n", n);
    WRITE_VAL("send_poll_status", cr.status);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(buf1); free(buf2);
    if (c) teardown(c);
}

void test_read_um_tp_jfr_bonding_bidir(void)
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

void test_write_imm_rc_ctp_jetty_bonding(void)
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
    WRITE_VAL("write_imm_rc_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("write_imm_rc_poll_n", n);
    WRITE_VAL("write_imm_rc_poll_status", cr.status);
    WRITE_VAL("write_imm_rc_dst_first", dst[0]);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
}

int main(void)
{
    test_read_um_ctp_jetty();
    test_write_rm_ctp_jetty_bidir();
    test_send_rc_ctp_jfr_bidir();
    test_read_rc_tp_jfr();
    test_write_rc_ctp_jetty_bonding();
    test_send_rm_ctp_jetty_bonding_bidir();
    test_read_um_tp_jfr_bonding_bidir();
    test_write_imm_rc_ctp_jetty_bonding();
    return 0;
}
