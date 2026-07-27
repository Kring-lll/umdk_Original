/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * 跨进程 SEND_WITH_IMM 验证：B 发带 imm_data 的 SEND，A poll recv CQE 检查 imm_data 回填。
 * 验证 sim A 类 opcode（SEND_WITH_IMM=1）的 imm 闭环：
 *   B post SEND_WITH_IMM(imm=0xDEADBEEF) → WQE 偏移40(=rmt_addr_l/h) 放 imm
 *   → sim process_wqe 取 imm → ipc_send 传 imm（协议头扩展）
 *   → 对端 handle_conn 收 imm → jfr_deliver_send 传 imm → produce_recv_cqe 填 data_l/h
 *   → A poll recv CQE：cr.opcode==SEND_WITH_IMM && cr.imm_data==0xDEADBEEF
 *
 * 用法（容器，URMA_SIM_HW=1 + LD_PRELOAD）：
 *   /tmp/test_ipc_send_imm A    # server，post recv，poll 验 imm
 *   /tmp/test_ipc_send_imm B    # client，import A jetty，post SEND_WITH_IMM
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "urma_api.h"
#include "urma_types.h"
#include "urma_opcode.h"

extern void urma_sim_ipc_start(const uint8_t *eid);

#define SEG_LEN 4096
#define JFC_DEPTH 64
#define JETTY_DEPTH 256
#define RECV_BUF_LEN 128
#define OOB_FILE "/tmp/urma_sim_oob_sendimm"
#define EXPECT_IMM 0xDEADBEEFULL

static void my_log(int level, char *message)
{
    if (level <= 3) fprintf(stderr, "[urma L%d] %s", level, message);
}

typedef struct {
    urma_eid_t eid;
    uint32_t uasid;
    uint32_t jetty_id;
} oob_info_t;

int main(int argc, char **argv)
{
    int rc = 1;
    int is_a = (argc > 1 && strcmp(argv[1], "A") == 0);
    const char *dev_name = is_a ? "udma_sim0" : "udma_sim1";

    urma_init_attr_t init_attr = {0};
    (void)urma_register_log_func(my_log);
    if (urma_init(&init_attr) != 0) { fprintf(stderr, "[%s] init fail\n", dev_name); return 1; }

    int num_devs = 0;
    urma_device_t **devs = urma_get_device_list(&num_devs);
    urma_device_t *dev = NULL;
    for (int i = 0; i < num_devs; i++) {
        if (strcmp(devs[i]->name, dev_name) == 0) { dev = devs[i]; break; }
    }
    if (dev == NULL) { fprintf(stderr, "[%s] no device\n", dev_name); return 1; }

    urma_context_t *ctx = urma_create_context(dev, 0);
    if (ctx == NULL) { fprintf(stderr, "[%s] create_context fail\n", dev_name); return 1; }
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
    if (jfr == NULL) { fprintf(stderr, "[%s] create_jfr fail\n", dev_name); return 1; }

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
    if (jetty == NULL) { fprintf(stderr, "[%s] create_jetty fail\n", dev_name); return 1; }

    if (is_a) {
        char *recv_buf = (char *)calloc(1, RECV_BUF_LEN);
        memset(recv_buf, '.', RECV_BUF_LEN);
        urma_sge_t sge = {0};
        sge.addr = (uint64_t)(uintptr_t)recv_buf;
        sge.len = RECV_BUF_LEN;
        urma_sg_t sg = { .sge = &sge, .num_sge = 1 };
        urma_jfr_wr_t wr = { .src = sg, .user_ctx = 0xA1, .next = NULL };
        urma_jfr_wr_t *bad = NULL;
        if (urma_post_jetty_recv_wr(jetty, &wr, &bad) != 0) {
            fprintf(stderr, "[A] post recv fail\n");
            goto cleanup;
        }
        fprintf(stderr, "[A] posted recv (user_ctx=0xA1)\n");

        oob_info_t info = {0};
        info.eid = ctx->eid;
        info.uasid = ctx->uasid;
        info.jetty_id = jetty->jetty_id.id;
        FILE *f = fopen(OOB_FILE, "wb");
        fwrite(&info, sizeof(info), 1, f);
        fclose(f);
        fprintf(stderr, "[A] wrote OOB jetty=%u, waiting for B's SEND_WITH_IMM...\n", info.jetty_id);

        urma_cr_t cr = {0};
        int n = 0;
        for (int i = 0; i < 100; i++) {
            n = urma_poll_jfc(jfc, 1, &cr);
            if (n > 0) break;
            usleep(100000);
        }
        fprintf(stderr, "[A] poll n=%d status=%u s_r=%u opcode=%u user_ctx=%lx imm=%lx\n",
                n, cr.status, cr.flag.bs.s_r, cr.opcode,
                (unsigned long long)cr.user_ctx, (unsigned long long)cr.imm_data);
        int ok = 0;
        if (n > 0 && cr.status == 0 && cr.flag.bs.s_r == 1 &&
            cr.opcode == URMA_CR_OPC_SEND_WITH_IMM && cr.user_ctx == 0xA1 &&
            cr.imm_data == EXPECT_IMM) {
            if (strncmp(recv_buf, "Hello IMM", 9) == 0) ok = 1;
        }
        fprintf(stderr, "[A] recv_buf='%.12s' (expect 'Hello IMM...')\n", recv_buf);
        fprintf(stderr, "[A] expect: opcode=%u imm=0x%lx\n",
                (unsigned)URMA_CR_OPC_SEND_WITH_IMM, (unsigned long long)EXPECT_IMM);
        fprintf(stderr, "TEST %s\n", ok ? "PASS" : "FAIL");
        rc = ok ? 0 : 1;
        free(recv_buf);
    } else {
        oob_info_t info = {0};
        for (int i = 0; i < 100; i++) {
            FILE *f = fopen(OOB_FILE, "rb");
            if (f) { fread(&info, sizeof(info), 1, f); fclose(f); break; }
            usleep(50000);
        }
        if (info.jetty_id == 0) { fprintf(stderr, "[B] no OOB\n"); goto cleanup; }

        urma_token_t tok = {0};
        urma_rjetty_t rjetty = {0};
        rjetty.jetty_id.eid = info.eid;
        rjetty.jetty_id.uasid = info.uasid;
        rjetty.jetty_id.id = info.jetty_id;
        rjetty.trans_mode = URMA_TM_UM;
        rjetty.type = URMA_JETTY;
        rjetty.tp_type = URMA_UTP;
        urma_target_jetty_t *tjetty = urma_import_jetty(ctx, &rjetty, &tok);
        if (tjetty == NULL) { fprintf(stderr, "[B] import fail\n"); goto cleanup; }

        char *send_buf = (char *)calloc(1, RECV_BUF_LEN);
        snprintf(send_buf, RECV_BUF_LEN, "Hello IMM from B");
        urma_sge_t sge = {0};
        sge.addr = (uint64_t)(uintptr_t)send_buf;
        sge.len = RECV_BUF_LEN;
        urma_sg_t sg = { .sge = &sge, .num_sge = 1 };
        urma_send_wr_t send_wr = { .src = sg, .imm_data = EXPECT_IMM };
        urma_jfs_wr_t w = {0};
        w.opcode = URMA_OPC_SEND_IMM;      /* URMA API opcode 0x41 → udma WQE SEND_WITH_IMM(1) */
        w.flag.bs.complete_enable = 1;
        w.tjetty = tjetty;
        w.user_ctx = 0xB1;
        w.send = send_wr;
        w.next = NULL;
        urma_jfs_wr_t *bad_wr = NULL;
        urma_status_t ret = urma_post_jetty_send_wr(jetty, &w, &bad_wr);
        fprintf(stderr, "[B] post SEND_WITH_IMM ret=%d imm=0x%lx\n", ret, (unsigned long long)EXPECT_IMM);
        if (ret == 0) {
            urma_cr_t cr = {0};
            for (int i = 0; i < 80; i++) {
                int n = urma_poll_jfc(jfc, 1, &cr);
                if (n > 0) { fprintf(stderr, "[B] send CQE s_r=%u\n", cr.flag.bs.s_r); break; }
                usleep(100000);
            }
        }
        urma_unimport_jetty(tjetty);
        free(send_buf);
        rc = 0;
    }
cleanup:
    urma_delete_jetty(jetty);
    urma_delete_jfr(jfr);
    urma_delete_jfc(jfc);
    urma_delete_jfce(jfce);
    urma_delete_context(ctx);
    urma_free_device_list(devs);
    if (is_a) unlink(OOB_FILE);
    return rc;
}
