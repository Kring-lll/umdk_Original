/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * 门禁式回归测试 demo：用 baseline_macros 标注要验的点，跑出 BASELINE|... 格式输出。
 * 跑一次 → 输出重定向到文件 → 生成器读输出 + 读本测试源 → 生成基准（expected）/比对。
 *
 * 这个 demo 验单进程 READ：建链 + post 1 READ + poll，关键点用宏标注。
 *
 * 用法（容器，URMA_SIM_HW=1）：
 *   gcc ... test_baseline_demo.c -o /tmp/test_baseline_demo
 *   /tmp/test_baseline_demo 2> /tmp/baseline_out.txt   # BASELINE 行进 stderr
 *   # 然后 gen_baseline 读 /tmp/baseline_out.txt + 本源生成基准
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "urma_api.h"
#include "urma_types.h"
#include "urma_opcode.h"
#include "baseline_macros.h"

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
    ASSERT_NONEMPTY("ctx", ctx);                 /* ctx 非空即可，不比地址 */

    urma_jfce_t *jfce = urma_create_jfce(ctx);
    urma_jfc_cfg_t jfc_cfg = {0};
    jfc_cfg.depth = JFC_DEPTH;
    jfc_cfg.jfce = jfce;
    urma_jfc_t *jfc = urma_create_jfc(ctx, &jfc_cfg);
    if (jfc == NULL) { fprintf(stderr, "create_jfc fail\n"); return 1; }
    ASSERT_NONEMPTY("jfc", jfc);

    urma_jfs_cfg_t jfs_cfg = {0};
    jfs_cfg.depth = JFS_DEPTH;
    jfs_cfg.flag.value = 0;
    jfs_cfg.trans_mode = URMA_TM_UM;
    jfs_cfg.max_sge = 4;
    jfs_cfg.max_rsge = 4;
    jfs_cfg.max_inline_data = 64;
    jfs_cfg.jfc = jfc;
    urma_jfs_t *jfs = urma_create_jfs(ctx, &jfs_cfg);
    if (jfs == NULL) { fprintf(stderr, "create_jfs fail\n"); return 1; }
    ASSERT_NONEMPTY("jfs", jfs);

    /* src seg（READ 远端源，单进程自引用） */
    char *src_buf = (char *)calloc(1, SEG_LEN);
    memset(src_buf, 'A', SEG_LEN);
    urma_seg_cfg_t src_cfg = {0};
    src_cfg.va = (uint64_t)(uintptr_t)src_buf;
    src_cfg.len = SEG_LEN;
    urma_target_seg_t *src_tseg = urma_register_seg(ctx, &src_cfg);
    if (src_tseg == NULL) {
        fprintf(stderr, "register_seg fail\n");
        urma_delete_jfs(jfs); urma_delete_jfc(jfc); urma_delete_jfce(jfce);
        urma_delete_context(ctx); urma_free_device_list(devs);
        return 1;
    }
    ASSERT_NONEMPTY("src_tseg", src_tseg);

    /* dst buf（READ 本端目标，初值 'Z'，post 后应被搬成 src 的 'A'） */
    char *dst_buf = (char *)calloc(1, SEG_LEN);
    memset(dst_buf, 'Z', SEG_LEN);

    /* import 自引用 jetty（UM 模式） */
    urma_token_t tok = {0};
    urma_rjetty_t rjetty = {0};
    rjetty.jetty_id.eid = ctx->eid;
    rjetty.jetty_id.uasid = ctx->uasid;
    rjetty.jetty_id.id = jfs->jfs_id.id;
    rjetty.trans_mode = URMA_TM_UM;
    rjetty.type = URMA_JETTY;
    rjetty.tp_type = URMA_UTP;
    urma_target_jetty_t *tjetty = urma_import_jetty(ctx, &rjetty, &tok);
    if (tjetty == NULL) {
        fprintf(stderr, "import_jetty fail\n");
        free(dst_buf); free(src_buf);
        urma_delete_jfs(jfs); urma_delete_jfc(jfc); urma_delete_jfce(jfce);
        urma_delete_context(ctx); urma_free_device_list(devs);
        return 1;
    }
    ASSERT_NONEMPTY("tjetty", tjetty);

    /* post READ：dst=dst_buf，src=src_buf+src_tseg，user_ctx=0x42 */
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
    wr.user_ctx = 0x42;
    wr.rw.dst = dst_sg;
    wr.rw.src = src_sg;
    urma_jfs_wr_t *bad_wr = NULL;
    urma_status_t ret = urma_post_jfs_wr(jfs, &wr, &bad_wr);
    WRITE_VAL("post_ret", ret);                 /* 期望 0（成功）逐次不变 */

    /* poll：验 status=SUCCESS(0) + user_ctx=0x42 + dst 被搬成 'A' */
    urma_cr_t cr = {0};
    int n = 0;
    for (int i = 0; i < 100; i++) {
        n = urma_poll_jfc(jfc, 1, &cr);
        if (n > 0) break;
        usleep(100000);
    }
    WRITE_VAL("poll_n", n);                       /* 期望 1 */
    WRITE_VAL("poll_status", cr.status);          /* 期望 0（SUCCESS） */
    WRITE_HEX("poll_user_ctx", cr.user_ctx);      /* 期望 0x42 */
    ASSERT_STR("dst_buf", dst_buf);               /* 期望 "AAA..."（src 是 'A'） */
    WRITE_VAL("dst_buf_first", dst_buf[0]);       /* 期望 'A'=65 */

    int ok = (ret == 0 && n > 0 && cr.status == 0 && cr.user_ctx == 0x42 &&
               dst_buf[0] == 'A');
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
