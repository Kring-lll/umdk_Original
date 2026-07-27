/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * 异常测试样例：注入 error CQE，验真 udma 错误转换链。
 *
 * 流程（单进程自引用，URMA_SIM_HW=1 + URMA_SIM_INJECT_STATUS=5）：
 *   建 jfc+jfs → register_seg → import 自引用 jetty → post 1 个 READ
 *   → sim 轮询解 WQE(READ) → produce_cqe 命中注入，造 status=5 的 error CQE
 *     （UDMA_CQE_TRANSACTION_ACK_TIMEOUT_ERR → 真 udma get_cr_status 转 URMA_CR_ACK_TIMEOUT_ERR）
 *   → poll 拿到 cr：status==URMA_CR_ACK_TIMEOUT_ERR（验错误转换链对）
 *
 * 这是最简异常样例——验证"sim 注入 error status → 真 udma 正确转换成 cr 错误码"。
 * 正常路径（不注入）cr.status==SUCCESS；注入后该是 ACK_TIMEOUT_ERR。
 *
 * 用法（容器，必须设 URMA_SIM_INJECT_STATUS=5）：
 *   URMA_SIM_INJECT_STATUS=5 /tmp/test_inject_cqe_err
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
#define JFS_DEPTH 64

static void my_log(int level, char *message)
{
    if (level <= 3) fprintf(stderr, "[urma L%d] %s", level, message);
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
    if (jfc == NULL) { fprintf(stderr, "create_jfc fail\n"); return 1; }

    /* jfs：UM 模式，post 直接走 jfs 不需 jetty（仿 test_hw_post） */
    urma_jfs_cfg_t jfs_cfg = {0};
    jfs_cfg.depth = JFS_DEPTH;
    jfs_cfg.flag.value = 0;
    jfs_cfg.trans_mode = URMA_TM_UM;
    jfs_cfg.priority = 0;
    jfs_cfg.max_sge = 4;
    jfs_cfg.max_rsge = 4;
    jfs_cfg.max_inline_data = 64;
    jfs_cfg.rnr_retry = 0;
    jfs_cfg.err_timeout = 0;
    jfs_cfg.jfc = jfc;
    urma_jfs_t *jfs = urma_create_jfs(ctx, &jfs_cfg);
    if (jfs == NULL) { fprintf(stderr, "create_jfs fail\n"); goto cleanup; }

    /* src seg（READ 远端源，单进程自引用） */
    char *src_buf = (char *)calloc(1, SEG_LEN);
    memset(src_buf, 'A', SEG_LEN);
    urma_seg_cfg_t src_cfg = {0};
    src_cfg.va = (uint64_t)(uintptr_t)src_buf;
    src_cfg.len = SEG_LEN;
    urma_target_seg_t *src_tseg = urma_register_seg(ctx, &src_cfg);
    if (src_tseg == NULL) { fprintf(stderr, "register_seg fail\n"); goto cleanup; }

    /* dst buf（READ 本端目标；注入 error 后数据不该被搬——验证错误时不搬） */
    char *dst_buf = (char *)calloc(1, SEG_LEN);
    memset(dst_buf, 'Z', SEG_LEN);

    /* import 自引用 jetty（UM 模式，用 jfs_id 当 jetty id，tjetty 给 wr 用） */
    urma_token_t tok = {0};
    urma_rjetty_t rjetty = {0};
    rjetty.jetty_id.eid = ctx->eid;
    rjetty.jetty_id.uasid = ctx->uasid;
    rjetty.jetty_id.id = jfs->jfs_id.id;
    rjetty.trans_mode = URMA_TM_UM;
    rjetty.type = URMA_JETTY;
    rjetty.tp_type = URMA_UTP;
    rjetty.flag.bs.token_policy = 0;
    urma_target_jetty_t *tjetty = urma_import_jetty(ctx, &rjetty, &tok);
    if (tjetty == NULL) { fprintf(stderr, "import_jetty fail\n"); goto cleanup; }

    /* post READ：dst=dst_buf，src=src_buf+src_tseg */
    urma_sge_t dst_sge = {0};
    dst_sge.addr = (uint64_t)(uintptr_t)dst_buf;
    dst_sge.len = SEG_LEN;
    urma_sg_t dst_sg = { .sge = &dst_sge, .num_sge = 1 };
    urma_sge_t src_sge = {0};
    src_sge.addr = (uint64_t)(uintptr_t)src_buf;
    src_sge.len = SEG_LEN;
    src_sge.tseg = src_tseg;
    urma_sg_t src_sg = { .sge = &src_sge, .num_sge = 1 };
    urma_jfs_wr_t wr = {0};
    wr.opcode = URMA_OPC_READ;
    wr.flag.bs.complete_enable = 1;
    wr.tjetty = tjetty;
    wr.user_ctx = 0xE1;
    wr.rw.dst = dst_sg;
    wr.rw.src = src_sg;
    urma_jfs_wr_t *bad_wr = NULL;
    urma_status_t ret = urma_post_jfs_wr(jfs, &wr, &bad_wr);
    fprintf(stderr, "[inject] post READ ret=%d (注入 status=5 ACK_TIMEOUT)\n", ret);
    if (ret != 0) goto cleanup;

    /* poll：注入 error CQE，cr.status 应是 URMA_CR_ACK_TIMEOUT_ERR（非 SUCCESS） */
    urma_cr_t cr = {0};
    int n = 0;
    for (int i = 0; i < 100; i++) {
        n = urma_poll_jfc(jfc, 1, &cr);
        if (n != 0) break;
        usleep(100000);
    }
    fprintf(stderr, "[inject] poll n=%d cr.status=%u (expect URMA_CR_ACK_TIMEOUT_ERR=%d)\n",
            n, cr.status, (int)URMA_CR_ACK_TIMEOUT_ERR);
    fprintf(stderr, "[inject] dst_buf[0]=%c (sim 先搬数据再造 error CQE，故搬运仍发生)\n", dst_buf[0]);

    /* 验：poll 返回非0 + cr.status==ACK_TIMEOUT_ERR（错误转换链对）+ user_ctx 回填
     * 注：sim process_wqe 先 memcpy 再造 CQE，故 error CQE 也会搬数据（与真硬件 error 时数据
     * 可能已部分传输一致）。不验"数据未搬"。 */
    int ok = 0;
    if (n > 0 && cr.status == URMA_CR_ACK_TIMEOUT_ERR && cr.user_ctx == 0xE1) {
        ok = 1;
    }
    fprintf(stderr, "TEST %s\n", ok ? "PASS" : "FAIL");
    rc = ok ? 0 : 1;

cleanup:
    if (tjetty) urma_unimport_jetty(tjetty);
    free(dst_buf); free(src_buf);
    if (jfs) urma_delete_jfs(jfs);
    urma_delete_jfc(jfc);
    urma_delete_jfce(jfce);
    urma_delete_context(ctx);
    urma_free_device_list(devs);
    return rc;
}
