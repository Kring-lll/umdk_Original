/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * 单进程原子操作验证：CAS + FAA。验证 sim B 类原子语义。
 *
 * 流程（单进程自引用，URMA_SIM_HW=1）：
 *   建 jetty+jfr → register_seg(atomic_buf，存原子变量) → import 自己 jetty
 *   CAS：atomic_buf 初值 0x100，cmp=0x100, swap=0x200
 *     → 远端变 0x200，原值 0x100 回本端 src buf
 *   FAA(FADD)：operand=5 → 远端 0x200+5=0x205，原值 0x200 回本端 src buf
 *   验：每次 poll CQE + atomic_buf 终值 + 本端收到的原值
 *
 * 用法（容器，URMA_SIM_HW=1）：
 *   /tmp/test_hw_atomic
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

static void my_log(int level, char *message)
{
    if (level <= 3) fprintf(stderr, "[urma L%d] %s", level, message);
}

/* poll 一个 CQE，超时返回 0 */
static int poll_one(urma_jfc_t *jfc, urma_cr_t *cr)
{
    for (int i = 0; i < 100; i++) {
        int n = urma_poll_jfc(jfc, 1, cr);
        if (n > 0) return n;
        usleep(100000);
    }
    return 0;
}

int main(void)
{
    int rc = 1;
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

    /* atomic_buf 作远端原子变量（seg） */
    uint64_t *atomic_buf = (uint64_t *)calloc(1, SEG_LEN);
    atomic_buf[0] = 0x100;   /* 初值 */
    urma_seg_cfg_t seg_cfg = {0};
    seg_cfg.va = (uint64_t)(uintptr_t)atomic_buf;
    seg_cfg.len = SEG_LEN;
    urma_target_seg_t *tseg = urma_register_seg(ctx, &seg_cfg);
    if (tseg == NULL) { fprintf(stderr, "register_seg fail\n"); goto cleanup; }

    /* 本端 buf：存 CAS/FAA 读回的原值 */
    uint64_t *orig_buf = (uint64_t *)calloc(1, 8);

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

    int ok = 1;
    urma_cr_t cr = {0};

    /* === CAS：atomic_buf=0x100, cmp=0x100, swap=0x200 → 远端应变 0x200，原值 0x100 回 orig_buf === */
    {
        urma_sge_t dsgel = {0};
        dsgel.addr = (uint64_t)(uintptr_t)atomic_buf;   /* dst = 远端原子地址 */
        dsgel.len = 8;
        dsgel.tseg = tseg;
        urma_sg_t dsg = { .sge = &dsgel, .num_sge = 1 };
        urma_sge_t ssgel = {0};
        ssgel.addr = (uint64_t)(uintptr_t)orig_buf;     /* src = 本端写回原值 buf */
        ssgel.len = 8;
        urma_sg_t ssg = { .sge = &ssgel, .num_sge = 1 };
        urma_jfs_wr_t w = {0};
        w.opcode = URMA_OPC_CAS;
        w.flag.bs.complete_enable = 1;
        w.tjetty = tjetty;
        w.user_ctx = 0xC1;
        w.cas.dst = &dsgel;
        w.cas.src = &ssgel;
        w.cas.cmp_data = 0x100;
        w.cas.swap_data = 0x200;
        urma_jfs_wr_t *bad = NULL;
        urma_status_t ret = urma_post_jetty_send_wr(jetty, &w, &bad);
        fprintf(stderr, "[CAS] post ret=%d (atomic=%lx orig=%lx)\n", ret,
                (unsigned long)atomic_buf[0], (unsigned long)orig_buf[0]);
        if (ret != 0) { ok = 0; goto done; }
        int n = poll_one(jfc, &cr);
        fprintf(stderr, "[CAS] poll n=%d status=%u user_ctx=%lx\n", n, cr.status, (unsigned long)cr.user_ctx);
        if (!(n > 0 && cr.status == 0 && cr.user_ctx == 0xC1)) { ok = 0; }
        if (atomic_buf[0] != 0x200) { fprintf(stderr, "[CAS] FAIL atomic=%lx (expect 0x200)\n", (unsigned long)atomic_buf[0]); ok = 0; }
        if (orig_buf[0] != 0x100) { fprintf(stderr, "[CAS] FAIL orig=%lx (expect 0x100)\n", (unsigned long)orig_buf[0]); ok = 0; }
    }

    /* === FAA(FADD)：atomic_buf 现在 0x200, operand=5 → 远端 0x205，原值 0x200 回 orig_buf === */
    {
        orig_buf[0] = 0;   /* 清空，验新原值 */
        urma_sge_t dsgel = {0};
        dsgel.addr = (uint64_t)(uintptr_t)atomic_buf;
        dsgel.len = 8;
        dsgel.tseg = tseg;
        urma_sg_t dsg = { .sge = &dsgel, .num_sge = 1 };
        urma_sge_t ssgel = {0};
        ssgel.addr = (uint64_t)(uintptr_t)orig_buf;
        ssgel.len = 8;
        urma_sg_t ssg = { .sge = &ssgel, .num_sge = 1 };
        urma_jfs_wr_t w = {0};
        w.opcode = URMA_OPC_FADD;
        w.flag.bs.complete_enable = 1;
        w.tjetty = tjetty;
        w.user_ctx = 0xC2;
        w.faa.dst = &dsgel;
        w.faa.src = &ssgel;
        w.faa.operand = 5;
        urma_jfs_wr_t *bad = NULL;
        urma_status_t ret = urma_post_jetty_send_wr(jetty, &w, &bad);
        fprintf(stderr, "[FAA] post ret=%d (atomic=%lx orig=%lx)\n", ret,
                (unsigned long)atomic_buf[0], (unsigned long)orig_buf[0]);
        if (ret != 0) { ok = 0; goto done; }
        int n = poll_one(jfc, &cr);
        fprintf(stderr, "[FAA] poll n=%d status=%u user_ctx=%lx\n", n, cr.status, (unsigned long)cr.user_ctx);
        if (!(n > 0 && cr.status == 0 && cr.user_ctx == 0xC2)) { ok = 0; }
        if (atomic_buf[0] != 0x205) { fprintf(stderr, "[FAA] FAIL atomic=%lx (expect 0x205)\n", (unsigned long)atomic_buf[0]); ok = 0; }
        if (orig_buf[0] != 0x200) { fprintf(stderr, "[FAA] FAIL orig=%lx (expect 0x200)\n", (unsigned long)orig_buf[0]); ok = 0; }
    }

    /* === CAS 不匹配：cmp=0xFFF（远端是 0x205）→ 不换，远端不变，原值 0x205 回 === */
    {
        orig_buf[0] = 0;
        urma_sge_t dsgel = {0};
        dsgel.addr = (uint64_t)(uintptr_t)atomic_buf;
        dsgel.len = 8;
        dsgel.tseg = tseg;
        urma_sg_t dsg = { .sge = &dsgel, .num_sge = 1 };
        urma_sge_t ssgel = {0};
        ssgel.addr = (uint64_t)(uintptr_t)orig_buf;
        ssgel.len = 8;
        urma_sg_t ssg = { .sge = &ssgel, .num_sge = 1 };
        urma_jfs_wr_t w = {0};
        w.opcode = URMA_OPC_CAS;
        w.flag.bs.complete_enable = 1;
        w.tjetty = tjetty;
        w.user_ctx = 0xC3;
        w.cas.dst = &dsgel;
        w.cas.src = &ssgel;
        w.cas.cmp_data = 0xFFF;   /* 不匹配远端 0x205 */
        w.cas.swap_data = 0xAAA;
        urma_jfs_wr_t *bad = NULL;
        urma_post_jetty_send_wr(jetty, &w, &bad);
        int n = poll_one(jfc, &cr);
        fprintf(stderr, "[CAS-nomatch] poll n=%d (atomic=%lx orig=%lx)\n", n,
                (unsigned long)atomic_buf[0], (unsigned long)orig_buf[0]);
        if (atomic_buf[0] != 0x205) { fprintf(stderr, "[CAS-nomatch] FAIL atomic=%lx (expect 0x205 不变)\n", (unsigned long)atomic_buf[0]); ok = 0; }
        if (orig_buf[0] != 0x205) { fprintf(stderr, "[CAS-nomatch] FAIL orig=%lx (expect 0x205 原值)\n", (unsigned long)orig_buf[0]); ok = 0; }
    }

done:
    fprintf(stderr, "TEST %s\n", ok ? "PASS" : "FAIL");
    rc = ok ? 0 : 1;
cleanup:
    if (tjetty) urma_unimport_jetty(tjetty);
    free(orig_buf); free(atomic_buf);
    urma_delete_jetty(jetty);
    urma_delete_jfr(jfr);
    urma_delete_jfc(jfc);
    urma_delete_jfce(jfce);
    urma_delete_context(ctx);
    urma_free_device_list(devs);
    return rc;
}
