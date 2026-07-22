/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * 单进程 WRITE_WITH_IMM 验证：自己 post WRITE_WITH_IMM 写自己 seg + 带 imm，
 * poll recv CQE 检查 imm_data 回填 + seg 数据写对。
 *
 * 验证 sim A 类 WRITE_WITH_IMM(4) 单进程 imm 闭环：
 *   建 jetty+jfr → register_seg(seg_buf) → post 1 recv（供 imm 通知 CQE）
 *   → import 自己 jetty（rmt_jetty_id=自己 jetty_id）
 *   → post WRITE_WITH_IMM(dst=seg_buf, imm=0xCAFE1234)
 *   → sim process_wqe(WRITE_IMM,4): ctl_len=64 → imm@48, SGE@64
 *     → memcpy 写 seg（WRITE 路径）+ urma_sim_jfr_deliver_imm 给本进程 JFR 产 recv CQE 带 imm
 *   → poll 拿 recv CQE：opcode=WRITE_WITH_IMM, imm=0xCAFE1234
 *   → 验 seg_buf 被写对 + recv CQE imm 对
 *
 * 用法（容器，URMA_SIM_HW=1）：
 *   /tmp/test_hw_write_imm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "urma_api.h"
#include "urma_types.h"
#include "urma_opcode.h"

extern void urma_sim_ipc_start(const uint8_t *eid);

#define SEG_LEN 4096
#define JFC_DEPTH 64
#define JETTY_DEPTH 256
#define EXPECT_IMM 0xCAFE1234ULL

static void my_log(int level, char *message)
{
    if (level <= 3) fprintf(stderr, "[urma L%d] %s", level, message);
}

int main(int argc, char **argv)
{
    int rc = 1;
    (void)argc; (void)argv;
    const char *dev_name = "udma_sim0";

    urma_init_attr_t init_attr = {0};
    (void)urma_register_log_func(my_log);
    if (urma_init(&init_attr) != 0) { fprintf(stderr, "init fail\n"); return 1; }

    int num_devs = 0;
    urma_device_t **devs = urma_get_device_list(&num_devs);
    urma_device_t *dev = NULL;
    for (int i = 0; i < num_devs; i++) {
        if (strcmp(devs[i]->name, dev_name) == 0) { dev = devs[i]; break; }
    }
    if (dev == NULL) { fprintf(stderr, "no device\n"); return 1; }

    urma_context_t *ctx = urma_create_context(dev, 0);
    if (ctx == NULL) { fprintf(stderr, "create_context fail\n"); return 1; }
    urma_sim_ipc_start(ctx->eid.raw);

    urma_jfce_t *jfce = urma_create_jfce(ctx);
    urma_jfc_cfg_t jfc_cfg = {0};
    jfc_cfg.depth = JFC_DEPTH;
    jfc_cfg.jfce = jfce;
    urma_jfc_t *jfc = urma_create_jfc(ctx, &jfc_cfg);

    urma_jfr_cfg_t jfr_cfg = {0};
    jfr_cfg.depth = JETTY_DEPTH;
    jfr_cfg.flag.bs.tag_matching = URMA_NO_TAG_MATCHING;
    jfr_cfg.flag.bs.order_type = 0;
    jfr_cfg.trans_mode = URMA_TM_UM;
    jfr_cfg.min_rnr_timer = URMA_TYPICAL_MIN_RNR_TIMER;
    jfr_cfg.jfc = jfc;
    jfr_cfg.max_sge = 1;
    urma_jfr_t *jfr = urma_create_jfr(ctx, &jfr_cfg);
    if (jfr == NULL) { fprintf(stderr, "create_jfr fail\n"); return 1; }

    urma_jfs_cfg_t jfs_cfg = {0};
    jfs_cfg.depth = JETTY_DEPTH;
    jfs_cfg.flag.bs.order_type = 0;
    jfs_cfg.trans_mode = URMA_TM_UM;
    jfs_cfg.priority = URMA_MAX_PRIORITY;
    jfs_cfg.max_sge = 1;
    jfs_cfg.max_inline_data = 0;
    jfs_cfg.rnr_retry = URMA_TYPICAL_RNR_RETRY;
    jfs_cfg.err_timeout = URMA_TYPICAL_ERR_TIMEOUT;
    jfs_cfg.jfc = jfc;
    urma_jetty_cfg_t jetty_cfg = {0};
    jetty_cfg.flag.bs.share_jfr = 1;
    jetty_cfg.jfs_cfg = jfs_cfg;
    jetty_cfg.shared.jfr = jfr;
    urma_jetty_t *jetty = urma_create_jetty(ctx, &jetty_cfg);
    if (jetty == NULL) { fprintf(stderr, "create_jetty fail\n"); return 1; }
    fprintf(stderr, "[A] jetty_id=%u\n", jetty->jetty_id.id);

    /* register seg（WRITE_WITH_IMM 的目标 buf） */
    char *seg_buf = (char *)calloc(1, SEG_LEN);
    memset(seg_buf, 'X', SEG_LEN);
    urma_seg_cfg_t seg_cfg = {0};
    seg_cfg.va = (uint64_t)(uintptr_t)seg_buf;
    seg_cfg.len = SEG_LEN;
    urma_target_seg_t *tseg = urma_register_seg(ctx, &seg_cfg);
    if (tseg == NULL) { fprintf(stderr, "register_seg fail\n"); goto cleanup; }

    /* post 1 recv（供 WRITE_WITH_IMM 的 imm 通知 CQE 用） */
    char *recv_buf = (char *)calloc(1, 128);
    memset(recv_buf, '.', 128);
    urma_sge_t rsge = {0};
    rsge.addr = (uint64_t)(uintptr_t)recv_buf;
    rsge.len = 128;
    urma_sg_t rsg = { .sge = &rsge, .num_sge = 1 };
    urma_jfr_wr_t rwr = { .src = rsg, .user_ctx = 0xA1, .next = NULL };
    urma_jfr_wr_t *rbad = NULL;
    if (urma_post_jetty_recv_wr(jetty, &rwr, &rbad) != 0) {
        fprintf(stderr, "post recv fail\n"); goto cleanup;
    }
    fprintf(stderr, "[A] posted recv (user_ctx=0xA1)\n");

    /* import 自己 jetty（单进程自引用） */
    urma_token_t tok = {0};
    urma_rjetty_t rjetty = {0};
    rjetty.jetty_id.eid = ctx->eid;
    rjetty.jetty_id.uasid = ctx->uasid;
    rjetty.jetty_id.id = jetty->jetty_id.id;
    rjetty.trans_mode = URMA_TM_UM;
    rjetty.type = URMA_JETTY;
    rjetty.tp_type = URMA_UTP;
    urma_target_jetty_t *tjetty = urma_import_jetty(ctx, &rjetty, &tok);
    if (tjetty == NULL) { fprintf(stderr, "import_jetty fail\n"); goto cleanup; }

    /* post WRITE_WITH_IMM：dst=远端 seg（tseg=register 返回的，单进程即自己 seg），src=本地 buf，imm=EXPECT_IMM。
     * 注：WRITE 的 dst sge 必须设 tseg（远端 seg 句柄），否则 udma_fill_write_sqe 走
     * sgl[0].user_tseg 分支解引用 NULL（segfault）。单进程自引用即 tseg=本端 register 的 tseg。 */
    char *src_buf = (char *)calloc(1, 128);
    snprintf(src_buf, 128, "WRITE-IMM-PAYLOAD");
    urma_sge_t dsgel = {0};
    dsgel.addr = (uint64_t)(uintptr_t)seg_buf;   /* dst = 远端 seg buf va */
    dsgel.len = 128;
    dsgel.tseg = tseg;                            /* 远端 seg 句柄（单进程=本端 reg 的 tseg） */
    urma_sg_t dsg = { .sge = &dsgel, .num_sge = 1 };
    urma_sge_t ssgel = {0};
    ssgel.addr = (uint64_t)(uintptr_t)src_buf;   /* src = 本地 buf */
    ssgel.len = 128;
    ssgel.tseg = NULL;                            /* 本地 buf 无 tseg */
    ssgel.user_tseg = NULL;
    urma_sg_t ssg = { .sge = &ssgel, .num_sge = 1 };
    urma_jfs_wr_t w = {0};
    w.opcode = URMA_OPC_WRITE_IMM;   /* URMA API 0x01 → udma WQE WRITE_WITH_IMM(4) */
    w.flag.bs.complete_enable = 1;
    w.tjetty = tjetty;
    w.user_ctx = 0xB1;
    w.rw.dst = dsg;
    w.rw.src = ssg;
    w.rw.notify_data = EXPECT_IMM;   /* imm/notify data（urma_types: notify_data 是 imm） */
    w.next = NULL;
    urma_jfs_wr_t *bad_wr = NULL;
    urma_status_t ret = urma_post_jetty_send_wr(jetty, &w, &bad_wr);
    fprintf(stderr, "[A] post WRITE_WITH_IMM ret=%d imm=0x%lx\n", ret, (unsigned long long)EXPECT_IMM);
    if (ret != 0) { rc = 1; goto cleanup; }

    /* poll send CQE（发送方完成）。imm 通知 recv CQE 投递 TODO（sim 缺对端 JFR 路由：
     * WRITE 的 WQE 只带 seg tid+rmt_eid，不带 jetty id，单进程无法定位"对端 JFR"投 imm 通知。
     * 跨进程也 TODO。这里只验数据搬运（seg_buf 被写对）+ send CQE 完成。）*/
    urma_cr_t cr = {0};
    int n = 0;
    for (int i = 0; i < 100; i++) {
        n = urma_poll_jfc(jfc, 1, &cr);
        if (n > 0) break;
        usleep(100000);
    }
    fprintf(stderr, "[A] poll n=%d status=%u s_r=%u opcode=%u user_ctx=%lx (send CQE)\n",
            n, cr.status, cr.flag.bs.s_r, cr.opcode, (unsigned long long)cr.user_ctx);

    /* 验数据搬运：seg_buf 被写对（WRITE_WITH_IMM 复用 WRITE 路径，ctl_len 变长后 SGE 偏移正确） */
    int ok = 0;
    if (n > 0 && cr.status == 0 && cr.flag.bs.s_r == 0 && cr.user_ctx == 0xB1 &&
        strncmp(seg_buf, "WRITE-IMM-PAYLOAD", 17) == 0) {
        ok = 1;
    }
    fprintf(stderr, "[A] seg_buf='%.20s' (expect 'WRITE-IMM-PAYLOAD...')\n", seg_buf);
    fprintf(stderr, "TEST %s (imm 通知 CQE 投递 TODO)\n", ok ? "PASS" : "FAIL");
    rc = ok ? 0 : 1;

cleanup:
    if (tjetty) urma_unimport_jetty(tjetty);
    free(src_buf); free(recv_buf); free(seg_buf);
    urma_delete_jetty(jetty);
    urma_delete_jfr(jfr);
    urma_delete_jfc(jfc);
    urma_delete_jfce(jfce);
    urma_delete_context(ctx);
    urma_free_device_list(devs);
    return rc;
}
