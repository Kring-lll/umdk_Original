/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * 多进程 SEND 验证：1 个 server 接收 N 个 client 并发 SEND。
 *
 * 验证 sim 的 IPC 是"按 eid 任意互联"的对等模型，不写死双边：
 *   - 4 个进程各持一台不同 eid 的设备（udma_sim0/1/2/3，guid 192.168.10/20/30/40.1）。
 *   - server(udma_sim0) 建 jetty(share_jfr, UM) + post N 个 recv WQE，
 *     每个 recv buf 用不同 user_ctx（0xC1/0xC2/0xC3）标识槽位。
 *     把 jetty_id + eid 写 OOB 文件，然后 poll 累计收 N 个 recv CQE。
 *   - 每个 client(udma_sim1/2/3) 读 OOB → import_jetty 指向 server →
 *     post SEND，payload 形如 "Cx:hello"（Cx = client 编号）。
 *   - server 收齐 N 个 CQE 后，逐个核对每个 recv buf 的前缀与 user_ctx 对应：
 *     user_ctx=0xC1 的槽应是 client1 的 "C1:..."。
 *
 * 期望：3 个 client 并发 SEND，server 收到 3 个 recv CQE（s_r=1, opcode=SEND），
 *       每个 recv buf 内容正确，user_ctx 与 client 一一对应。
 *
 * 用法（容器内，URMA_SIM_HW=1 + LD_PRELOAD liburma_sim.so）：
 *   URMA_SIM_HW=1 LD_PRELOAD=... /tmp/test_ipc_multi_send server
 *   URMA_SIM_HW=1 LD_PRELOAD=... /tmp/test_ipc_multi_send client 1
 *   URMA_SIM_HW=1 LD_PRELOAD=... /tmp/test_ipc_multi_send client 2
 *   URMA_SIM_HW=1 LD_PRELOAD=... /tmp/test_ipc_multi_send client 3
 * 或用 examples/run_ipc_multi_send.sh 一键起 4 进程。
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
#define OOB_FILE "/tmp/urma_sim_oob_multi"
#define NUM_CLIENTS 3

/* 每个 client 对应的 recv 槽 user_ctx 与期望 payload 前缀 */
static const uint64_t CLIENT_CTX[NUM_CLIENTS] = { 0xC1, 0xC2, 0xC3 };
static const char *CLIENT_TAG[NUM_CLIENTS] = { "C1", "C2", "C3" };

static void my_log(int level, char *message)
{
    if (level <= 3) {
        fprintf(stderr, "[urma L%d] %s", level, message);
    }
}

typedef struct {
    urma_eid_t eid;
    uint32_t uasid;
    uint32_t jetty_id;
} oob_info_t;

/* 按参数取设备名。server=udma_sim0, client i=udma_sim{i}（i=1..3）。 */
static const char *dev_name_for(const char *role, int client_idx)
{
    static const char *names[4] = { "udma_sim0", "udma_sim1", "udma_sim2", "udma_sim3" };
    if (strcmp(role, "server") == 0) {
        return names[0];
    }
    if (client_idx >= 1 && client_idx <= 3) {
        return names[client_idx];
    }
    return names[0];
}

int main(int argc, char **argv)
{
    int rc = 1;
    if (argc < 2) {
        fprintf(stderr, "usage: %s server | client <1..%d>\n", argv[0], NUM_CLIENTS);
        return 1;
    }
    int is_server = (strcmp(argv[1], "server") == 0);
    int client_idx = 0;
    if (!is_server) {
        if (strcmp(argv[1], "client") != 0 || argc < 3) {
            fprintf(stderr, "usage: %s client <1..%d>\n", argv[0], NUM_CLIENTS);
            return 1;
        }
        client_idx = atoi(argv[2]);
        if (client_idx < 1 || client_idx > NUM_CLIENTS) {
            fprintf(stderr, "client idx must be 1..%d\n", NUM_CLIENTS);
            return 1;
        }
    }
    const char *dev_name = dev_name_for(argv[1], client_idx);
    const char *role = is_server ? "server" : "client";

    urma_init_attr_t init_attr = {0};
    (void)urma_register_log_func(my_log);
    if (urma_init(&init_attr) != 0) {
        fprintf(stderr, "[%s] urma_init failed\n", role);
        return 1;
    }

    int num_devs = 0;
    urma_device_t **devs = urma_get_device_list(&num_devs);
    urma_device_t *dev = NULL;
    for (int i = 0; i < num_devs; i++) {
        if (strcmp(devs[i]->name, dev_name) == 0) { dev = devs[i]; break; }
    }
    if (dev == NULL) { fprintf(stderr, "[%s] no device %s\n", role, dev_name); return 1; }

    urma_context_t *ctx = urma_create_context(dev, 0);
    if (ctx == NULL) { fprintf(stderr, "[%s] create_context failed\n", role); return 1; }
    urma_sim_ipc_start(ctx->eid.raw);
    fprintf(stderr, "[%s] %s ctx eid: %02x%02x...%02x%02x\n", role, dev_name,
            ctx->eid.raw[8], ctx->eid.raw[9], ctx->eid.raw[14], ctx->eid.raw[15]);

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
    if (jfr == NULL) { fprintf(stderr, "[%s] create_jfr failed\n", role); return 1; }

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
    if (jetty == NULL) { fprintf(stderr, "[%s] create_jetty failed\n", role); return 1; }
    fprintf(stderr, "[%s] jetty_id=%u\n", role, jetty->jetty_id.id);

    if (is_server) {
        /* server：为每个 client 预 post 一个 recv WQE，user_ctx 标识 client。
         * 注意：UM 无 tag matching，recv 槽按先进先出被 SEND 消费，
         * 无法保证"client i 的 SEND 恰好落进第 i 个槽"。所以核对时
         * 不固定槽位↔client，而是检查"3 个 recv buf 收齐了 C1/C2/C3 三个前缀"，
         * 并用 user_ctx 把 buf 与 CQE 对应起来。 */
        char *recv_buf[NUM_CLIENTS];
        for (int i = 0; i < NUM_CLIENTS; i++) {
            recv_buf[i] = (char *)calloc(1, RECV_BUF_LEN);
            memset(recv_buf[i], '.', RECV_BUF_LEN);
            urma_sge_t sge = {0};
            sge.addr = (uint64_t)(uintptr_t)recv_buf[i];
            sge.len = RECV_BUF_LEN;
            urma_sg_t sg = { .sge = &sge, .num_sge = 1 };
            urma_jfr_wr_t wr = { .src = sg, .user_ctx = CLIENT_CTX[i], .next = NULL };
            urma_jfr_wr_t *bad = NULL;
            if (urma_post_jetty_recv_wr(jetty, &wr, &bad) != 0) {
                fprintf(stderr, "[server] post recv%d failed\n", i);
                goto cleanup;
            }
        }
        fprintf(stderr, "[server] posted %d recv WQEs (user_ctx=0x%lx/0x%lx/0x%lx)\n",
                NUM_CLIENTS,
                (unsigned long)CLIENT_CTX[0], (unsigned long)CLIENT_CTX[1], (unsigned long)CLIENT_CTX[2]);

        oob_info_t info = {0};
        info.eid = ctx->eid;
        info.uasid = ctx->uasid;
        info.jetty_id = jetty->jetty_id.id;
        FILE *f = fopen(OOB_FILE, "wb");
        fwrite(&info, sizeof(info), 1, f);
        fclose(f);
        fprintf(stderr, "[server] wrote OOB (jetty_id=%u), waiting for %d clients' SEND...\n",
                info.jetty_id, NUM_CLIENTS);

        /* poll 累计收 NUM_CLIENTS 个 recv CQE。
         * 每个 CQE 的 user_ctx 指向某个 recv 槽，用它在 recv_buf 里定位数据。 */
        int got = 0;
        int seen[NUM_CLIENTS] = {0};   /* 哪几个 client 前缀已核对到 */
        urma_cr_t cr = {0};
        for (int iter = 0; iter < 300 && got < NUM_CLIENTS; iter++) {  /* 最多 30s */
            int n = urma_poll_jfc(jfc, 1, &cr);
            if (n <= 0) {
                usleep(100000);
                continue;
            }
            fprintf(stderr, "[server] poll CQE#%d status=%u s_r=%u opcode=%u user_ctx=%lx\n",
                    got, cr.status, cr.flag.bs.s_r, cr.opcode, (unsigned long)cr.user_ctx);
            if (cr.status != 0 || cr.flag.bs.s_r != 1 || cr.opcode != URMA_CR_OPC_SEND) {
                fprintf(stderr, "[server] CQE#%d unexpected, abort\n", got);
                break;
            }
            /* user_ctx = CLIENT_CTX[i] 对应 recv_buf[i] */
            int slot = -1;
            for (int i = 0; i < NUM_CLIENTS; i++) {
                if ((uint64_t)cr.user_ctx == CLIENT_CTX[i]) { slot = i; break; }
            }
            if (slot < 0) {
                fprintf(stderr, "[server] CQE user_ctx=%lx unmatched\n", (unsigned long)cr.user_ctx);
                break;
            }
            /* recv_buf[slot] 应含某 client 的前缀 "Cx:..."，核对是哪个 client */
            for (int c = 0; c < NUM_CLIENTS; c++) {
                if (strncmp(recv_buf[slot], CLIENT_TAG[c], 2) == 0) {
                    seen[c] = 1;
                    fprintf(stderr, "[server] slot%d(user_ctx=0x%lx) got '%.6s' from client%d\n",
                            slot, (unsigned long)cr.user_ctx, recv_buf[slot], c + 1);
                    break;
                }
            }
            got++;
        }

        int ok = 0;
        if (got == NUM_CLIENTS) {
            ok = 1;
            for (int c = 0; c < NUM_CLIENTS; c++) {
                if (!seen[c]) {
                    fprintf(stderr, "[server] client%d's message NOT received\n", c + 1);
                    ok = 0;
                }
            }
        }
        fprintf(stderr, "[server] got %d/%d CQEs, clients seen: %d/%d/%d\n",
                got, NUM_CLIENTS, seen[0], seen[1], seen[2]);
        fprintf(stderr, "TEST %s\n", ok ? "PASS" : "FAIL");
        rc = ok ? 0 : 1;

        for (int i = 0; i < NUM_CLIENTS; i++) {
            free(recv_buf[i]);
        }
    } else {
        /* client i：读 server OOB → import_jetty → post SEND("Ci:hello") */
        oob_info_t info = {0};
        for (int i = 0; i < 100; i++) {
            FILE *f = fopen(OOB_FILE, "rb");
            if (f) { fread(&info, sizeof(info), 1, f); fclose(f); break; }
            usleep(50000);
        }
        if (info.jetty_id == 0) {
            fprintf(stderr, "[client%d] no OOB from server\n", client_idx);
            goto cleanup;
        }
        fprintf(stderr, "[client%d] got server jetty_id=%u, import_jetty\n", client_idx, info.jetty_id);

        urma_token_t tok = {0};
        urma_rjetty_t rjetty = {0};
        rjetty.jetty_id.eid = info.eid;
        rjetty.jetty_id.uasid = info.uasid;
        rjetty.jetty_id.id = info.jetty_id;
        rjetty.trans_mode = URMA_TM_UM;
        rjetty.type = URMA_JETTY;
        rjetty.tp_type = URMA_UTP;
        urma_target_jetty_t *tjetty = urma_import_jetty(ctx, &rjetty, &tok);
        if (tjetty == NULL) {
            fprintf(stderr, "[client%d] import_jetty failed\n", client_idx);
            goto cleanup;
        }

        char *send_buf = (char *)calloc(1, RECV_BUF_LEN);
        snprintf(send_buf, RECV_BUF_LEN, "%s:hello-from-pid%d", CLIENT_TAG[client_idx - 1], getpid());
        urma_sge_t sge = {0};
        sge.addr = (uint64_t)(uintptr_t)send_buf;
        sge.len = RECV_BUF_LEN;
        urma_sg_t sg = { .sge = &sge, .num_sge = 1 };
        urma_send_wr_t send_wr = { .src = sg };
        urma_jfs_wr_t wr = {0};
        wr.opcode = URMA_OPC_SEND;
        wr.flag.bs.complete_enable = 1;
        wr.tjetty = tjetty;
        wr.user_ctx = 0xD0 + client_idx;
        wr.send = send_wr;
        wr.next = NULL;
        urma_jfs_wr_t *bad_wr = NULL;
        urma_status_t ret = urma_post_jetty_send_wr(jetty, &wr, &bad_wr);
        fprintf(stderr, "[client%d] post SEND ret=%d (%s)\n", client_idx, ret, send_buf);
        if (ret == 0) {
            urma_cr_t cr = {0};
            for (int i = 0; i < 80; i++) {
                int n = urma_poll_jfc(jfc, 1, &cr);
                if (n > 0) {
                    fprintf(stderr, "[client%d] send CQE status=%u s_r=%u user_ctx=%lx\n",
                            client_idx, cr.status, cr.flag.bs.s_r, (unsigned long)cr.user_ctx);
                    break;
                }
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
    unlink(OOB_FILE);   /* server 退出时清 OOB，便于重跑 */
    return rc;
}
