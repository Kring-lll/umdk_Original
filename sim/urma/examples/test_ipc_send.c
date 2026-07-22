/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * 跨进程 SEND 验证（方式1 扩展）：B 给 A 发 SEND，A 的 JFR recv 槽收到数据。
 *
 * 设计（仿 urma_sample 的 share_jfr jetty + recv/post SEND）：
 *   proc_A: udma_sim0(eid=...c0a80a01) 起 IPC socket
 *           create jfc + jfr + jetty(share_jfr, UM+UTP)
 *           register_seg(buf_A)  供不需要，但 create 流程要
 *           post 2 个 recv WQE（recv buf，user_ctx=0xA1/0xA2）到 jetty
 *           把 jetty_id + eid 写 OOB 文件给 B，然后 poll jfc 等 recv CQE
 *   proc_B: udma_sim1(eid=...c0a81401) 起 IPC socket
 *           create jfc + jfr + jetty(share_jfr, UM+UTP)
 *           读 OOB 拿 A 的 jetty_id + eid → import_jetty(指向 A)
 *           post SEND("Hello from B") 给 A 的 jetty
 *   期望：A poll 拿到 recv CQE（s_r=1, opcode=SEND, user_ctx=0xA1），
 *         recv buf 内容 = "Hello from B"
 *
 * 寻址：B 解 WQE(SEND) 拿 rmt_eid=A 的 eid + rmt_jetty_id=A 的 jetty_id。
 *       跨进程 → sim IPC 发 SEND 请求给 A；A 查自己 jetty_id→JFR recv 槽 →
 *       memcpy 数据进 recv buf → 造 recv CQE（s_r=1, entry_idx=rqe_idx）。
 *       真 udma poll 读 recv CQE → cr.user_ctx = jfr->rq.wrid[entry_idx]（A post recv 时存的）。
 *
 * 用法（容器内，开两个终端或后台）：
 *   URMA_SIM_HW=1 LD_PRELOAD=... /tmp/test_ipc_send A
 *   URMA_SIM_HW=1 LD_PRELOAD=... /tmp/test_ipc_send B
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>

#include "urma_api.h"
#include "urma_types.h"
#include "urma_opcode.h"

extern void urma_sim_ipc_start(const uint8_t *eid);

#define SEG_LEN 4096
#define JFC_DEPTH 64
#define JETTY_DEPTH 256
#define RECV_BUF_LEN 128
#define OOB_FILE "/tmp/urma_sim_oob_send"

static void my_log(int level, char *message)
{
    if (level <= 3) {
        fprintf(stderr, "[urma L%d] %s", level, message);
    }
}

typedef struct {
    urma_eid_t eid;
    uint32_t uasid;
    uint32_t jetty_id;   /* A 的 jetty_id（B import_jetty 用） */
} oob_info_t;

int main(int argc, char **argv)
{
    int rc = 1;
    int is_a = (argc > 1 && strcmp(argv[1], "A") == 0);
    const char *dev_name = is_a ? "udma_sim0" : "udma_sim1";

    urma_init_attr_t init_attr = {0};
    (void)urma_register_log_func(my_log);
    if (urma_init(&init_attr) != 0) {
        fprintf(stderr, "[%s] urma_init failed\n", dev_name);
        return 1;
    }

    int num_devs = 0;
    urma_device_t **devs = urma_get_device_list(&num_devs);
    urma_device_t *dev = NULL;
    for (int i = 0; i < num_devs; i++) {
        if (strcmp(devs[i]->name, dev_name) == 0) { dev = devs[i]; break; }
    }
    if (dev == NULL) { fprintf(stderr, "[%s] no device\n", dev_name); return 1; }

    urma_context_t *ctx = urma_create_context(dev, 0);
    if (ctx == NULL) { fprintf(stderr, "[%s] create_context failed\n", dev_name); return 1; }
    urma_sim_ipc_start(ctx->eid.raw);
    fprintf(stderr, "[%s] ctx eid: %02x%02x...%02x%02x\n", dev_name,
            ctx->eid.raw[8], ctx->eid.raw[9], ctx->eid.raw[14], ctx->eid.raw[15]);

    urma_jfce_t *jfce = urma_create_jfce(ctx);
    urma_jfc_cfg_t jfc_cfg = {0};
    jfc_cfg.depth = JFC_DEPTH;
    jfc_cfg.jfce = jfce;
    urma_jfc_t *jfc = urma_create_jfc(ctx, &jfc_cfg);

    /* jfr：UM + 无 tag matching。order_type=0（urma_convert 会转 URMA_NO） */
    urma_jfr_cfg_t jfr_cfg = {0};
    jfr_cfg.depth = JETTY_DEPTH;
    jfr_cfg.flag.bs.tag_matching = URMA_NO_TAG_MATCHING;
    jfr_cfg.flag.bs.order_type = 0;
    jfr_cfg.trans_mode = URMA_TM_UM;
    jfr_cfg.min_rnr_timer = URMA_TYPICAL_MIN_RNR_TIMER;
    jfr_cfg.jfc = jfc;
    jfr_cfg.max_sge = 1;
    urma_jfr_t *jfr = urma_create_jfr(ctx, &jfr_cfg);
    if (jfr == NULL) { fprintf(stderr, "[%s] create_jfr failed\n", dev_name); return 1; }

    /* jetty：share_jfr，jfs UM+UTP */
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
    if (jetty == NULL) { fprintf(stderr, "[%s] create_jetty failed\n", dev_name); return 1; }
    fprintf(stderr, "[%s] jetty_id=%u\n", dev_name, jetty->jetty_id.id);

    if (is_a) {
        /* A：post 2 个 recv WQE，写 OOB 给 B，poll 等 recv CQE */
        char *recv_buf1 = (char *)calloc(1, RECV_BUF_LEN);
        char *recv_buf2 = (char *)calloc(1, RECV_BUF_LEN);
        memset(recv_buf1, '.', RECV_BUF_LEN);
        memset(recv_buf2, '.', RECV_BUF_LEN);

        urma_sge_t sge1 = {0};
        sge1.addr = (uint64_t)(uintptr_t)recv_buf1;
        sge1.len = RECV_BUF_LEN;
        urma_sg_t sg1 = { .sge = &sge1, .num_sge = 1 };
        urma_jfr_wr_t wr1 = { .src = sg1, .user_ctx = 0xA1, .next = NULL };
        urma_jfr_wr_t *bad = NULL;
        if (urma_post_jetty_recv_wr(jetty, &wr1, &bad) != 0) {
            fprintf(stderr, "[A] post recv1 failed\n");
            return 1;
        }
        urma_sge_t sge2 = {0};
        sge2.addr = (uint64_t)(uintptr_t)recv_buf2;
        sge2.len = RECV_BUF_LEN;
        urma_sg_t sg2 = { .sge = &sge2, .num_sge = 1 };
        urma_jfr_wr_t wr2 = { .src = sg2, .user_ctx = 0xA2, .next = NULL };
        if (urma_post_jetty_recv_wr(jetty, &wr2, &bad) != 0) {
            fprintf(stderr, "[A] post recv2 failed\n");
            return 1;
        }
        fprintf(stderr, "[A] posted 2 recv WQEs (user_ctx=0xA1,0xA2)\n");

        oob_info_t info = {0};
        info.eid = ctx->eid;
        info.uasid = ctx->uasid;
        info.jetty_id = jetty->jetty_id.id;
        FILE *f = fopen(OOB_FILE, "wb");
        fwrite(&info, sizeof(info), 1, f);
        fclose(f);
        fprintf(stderr, "[A] wrote OOB (jetty_id=%u), waiting for B's SEND...\n", info.jetty_id);

        /* poll recv CQE（最多等 10s） */
        urma_cr_t cr = {0};
        int n = 0;
        for (int i = 0; i < 100; i++) {
            n = urma_poll_jfc(jfc, 1, &cr);
            if (n > 0) break;
            usleep(100000);
        }
        fprintf(stderr, "[A] poll n=%d status=%u s_r=%u opcode=%u user_ctx=%llx\n",
                n, cr.status, cr.flag.bs.s_r, cr.opcode, (unsigned long long)cr.user_ctx);
        int ok = 0;
        if (n > 0 && cr.status == 0 && cr.flag.bs.s_r == 1 && cr.opcode == URMA_CR_OPC_SEND &&
            cr.user_ctx == 0xA1) {
            /* 验证 recv_buf1 收到 B 的消息 */
            if (strncmp(recv_buf1, "Hello from B", 12) == 0) {
                ok = 1;
            }
        }
        fprintf(stderr, "[A] recv_buf1='%.13s' (expect 'Hello from B')\n", recv_buf1);
        fprintf(stderr, "TEST %s\n", ok ? "PASS" : "FAIL");
        rc = ok ? 0 : 1;
        free(recv_buf1);
        free(recv_buf2);
    } else {
        /* B：读 A 的 OOB，import_jetty 指向 A，post SEND 给 A */
        oob_info_t info = {0};
        for (int i = 0; i < 100; i++) {
            FILE *f = fopen(OOB_FILE, "rb");
            if (f) { fread(&info, sizeof(info), 1, f); fclose(f); break; }
            usleep(50000);
        }
        if (info.jetty_id == 0) { fprintf(stderr, "[B] no OOB from A\n"); goto out; }
        fprintf(stderr, "[B] got A's jetty_id=%u, import_jetty\n", info.jetty_id);

        urma_token_t tok = {0};
        urma_rjetty_t rjetty = {0};
        rjetty.jetty_id.eid = info.eid;
        rjetty.jetty_id.uasid = info.uasid;
        rjetty.jetty_id.id = info.jetty_id;
        rjetty.trans_mode = URMA_TM_UM;
        rjetty.type = URMA_JETTY;
        rjetty.tp_type = URMA_UTP;
        urma_target_jetty_t *tjetty = urma_import_jetty(ctx, &rjetty, &tok);
        if (tjetty == NULL) { fprintf(stderr, "[B] import_jetty failed\n"); goto out; }

        /* post SEND 给 A */
        char *send_buf = (char *)calloc(1, RECV_BUF_LEN);
        snprintf(send_buf, RECV_BUF_LEN, "Hello from B (pid %d)", getpid());
        urma_sge_t sge = {0};
        sge.addr = (uint64_t)(uintptr_t)send_buf;
        sge.len = RECV_BUF_LEN;
        urma_sg_t sg = { .sge = &sge, .num_sge = 1 };
        urma_send_wr_t send_wr = { .src = sg };
        urma_jfs_wr_t wr = {0};
        wr.opcode = URMA_OPC_SEND;
        wr.flag.bs.complete_enable = 1;
        wr.tjetty = tjetty;
        wr.user_ctx = 0xB1;
        wr.send = send_wr;
        wr.next = NULL;
        urma_jfs_wr_t *bad_wr = NULL;
        urma_status_t ret = urma_post_jetty_send_wr(jetty, &wr, &bad_wr);
        fprintf(stderr, "[B] post SEND ret=%d\n", ret);
        /* 等 SEND 完成 CQE（发送方 send CQE） */
        if (ret == 0) {
            urma_cr_t cr = {0};
            for (int i = 0; i < 80; i++) {
                int n = urma_poll_jfc(jfc, 1, &cr);
                if (n > 0) {
                    fprintf(stderr, "[B] poll n=%d status=%u s_r=%u user_ctx=%llx\n",
                            n, cr.status, cr.flag.bs.s_r, (unsigned long long)cr.user_ctx);
                    break;
                }
                usleep(100000);
            }
        }
        urma_unimport_jetty(tjetty);
        free(send_buf);
        rc = 0;
    }

out:
    urma_delete_jetty(jetty);
    urma_delete_jfr(jfr);
    urma_delete_jfc(jfc);
    urma_delete_jfce(jfce);
    urma_delete_context(ctx);
    urma_free_device_list(devs);
    return rc;
}
