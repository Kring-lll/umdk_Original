/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * 方式1（仿真硬件）端到端单进程测试。
 *
 * 流程（URMA_SIM_HW=1，真 udma 跑，sim 拦 mmap/ummu + 轮询）：
 *   urma_init → urma_get_device_list → urma_create_context
 *   → urma_create_jfce → urma_create_jfc → urma_create_jfs(UM 模式)
 *   → urma_alloc_token_id
 *   → urma_import_jetty(伪造 rjetty：自引用本端 eid，UM + tp_type=UTP)
 *   → urma_register_seg(src_buf)（ummu_grant 拦截登记 tid→src_va）
 *   → 构 urma_jfs_wr(URMA_OPC_READ, dst sge=本地 dst_buf, src sge=src_buf+src_tseg)
 *   → urma_post_jfs_wr：真 udma 写 WQE 到 sim 假 queue + 写 doorbell
 *   → sim 轮询线程：发现 pi 变化 → 解 WQE(opcode=6 READ) → memcpy(dst=dst_buf, src=src_buf) → 造 CQE
 *   → urma_poll_jfc：真 udma 读 sim 造的 CQE → 返回 cr(status=SUCCESS, user_ctx 回填)
 *
 * 验证点：READ 后 dst_buf 内容 'Z'→'A'（memcpy 把 src 搬到 dst），
 *         且 poll 拿到 n=1, status=SUCCESS, user_ctx=0x1234。
 *
 * 编译运行（容器内）：
 *   gcc -I/umdk/src/urma/lib/urma/core/include \
 *       -I/umdk/src/urma/common/include \
 *       -L/umdk/src/build_noasan/urma/lib/urma/core -lurma \
 *       -L/umdk/src/build_noasan/urma/common -lurma_common \
 *       -Wl,-rpath,/umdk/src/build_noasan/urma/lib/urma/core \
 *       -Wl,-rpath,/umdk/src/build_noasan/urma/common \
 *       /umdk/sim/urma/examples/test_hw_post.c -o /tmp/test_hw_post
 *   URMA_SIM_HW=1 LD_PRELOAD=/umdk/sim/urma/build/liburma_sim.so \
 *     URMA_SIM_CONFIG=/umdk/sim/urma/urma_sim_config.json \
 *     LD_LIBRARY_PATH=/umdk/src/build_noasan/urma/lib/urma/core:/umdk/src/build_noasan/urma/common \
 *     /tmp/test_hw_post
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "urma_api.h"
#include "urma_types.h"
#include "urma_opcode.h"   /* URMA_OPC_READ */

/* urma 日志回调：打到 stderr（默认走 syslog，容器里看不到）。
 * 只打 ERROR 及以上，避免 INFO 日志刷屏；排查时设 URMA_LOG_LEVEL=7 看 info。 */
static void my_log(int level, char *message)
{
    if (level <= 3) {   /* URMA_VLOG_LEVEL_ERR=3 */
        fprintf(stderr, "[urma L%d] %s", level, message);
    }
}

#define SEG_LEN 4096
#define JFC_DEPTH 64
#define JFS_DEPTH 64

int main(int argc, char **argv)
{
    int rc = 1;
    urma_status_t ret;
    int num_devs = 0;

    (void)argc;
    (void)argv;

    urma_init_attr_t init_attr = {0};   /* token=0, uasid=0（系统随机分配 uasid） */
    /* 把 urma 日志（默认走 syslog，stderr 看不到）引到 stderr，定位 import 失败原因 */
    (void)urma_register_log_func(my_log);
    ret = urma_init(&init_attr);
    if (ret != 0) {
        fprintf(stderr, "urma_init failed: %d\n", ret);
        return 1;
    }

    urma_device_t **devs = urma_get_device_list(&num_devs);
    if (devs == NULL || num_devs == 0) {
        fprintf(stderr, "no device found\n");
        return 1;
    }
    fprintf(stderr, "found %d devices, use %s\n", num_devs, devs[0]->name);

    urma_context_t *ctx = urma_create_context(devs[0], 0);
    if (ctx == NULL) {
        fprintf(stderr, "create_context failed\n");
        return 1;
    }
    fprintf(stderr, "ctx=%p\n", (void *)ctx);

    urma_jfce_t *jfce = urma_create_jfce(ctx);
    if (jfce == NULL) {
        fprintf(stderr, "create_jfce failed\n");
        goto out_ctx;
    }

    urma_jfc_cfg_t jfc_cfg = {0};
    jfc_cfg.depth = JFC_DEPTH;
    jfc_cfg.flag.value = 0;
    jfc_cfg.ceqn = 0;
    jfc_cfg.jfce = jfce;
    jfc_cfg.user_ctx = 0;
    urma_jfc_t *jfc = urma_create_jfc(ctx, &jfc_cfg);
    if (jfc == NULL) {
        fprintf(stderr, "create_jfc failed\n");
        goto out_jfce;
    }
    fprintf(stderr, "jfc=%p id=%u\n", (void *)jfc, jfc->jfc_id.id);

    urma_jfs_cfg_t jfs_cfg = {0};
    jfs_cfg.depth = JFS_DEPTH;
    jfs_cfg.flag.value = 0;
    jfs_cfg.trans_mode = URMA_TM_UM;   /* UM：post 直接走 jfs，不需 jetty/tjetty */
    jfs_cfg.priority = 0;
    jfs_cfg.max_sge = 4;
    jfs_cfg.max_rsge = 4;
    jfs_cfg.max_inline_data = 64;
    jfs_cfg.rnr_retry = 0;
    jfs_cfg.err_timeout = 0;
    jfs_cfg.jfc = jfc;
    jfs_cfg.user_ctx = 0;
    urma_jfs_t *jfs = urma_create_jfs(ctx, &jfs_cfg);
    if (jfs == NULL) {
        fprintf(stderr, "create_jfs failed\n");
        goto out_jfc;
    }
    fprintf(stderr, "jfs=%p id=%u\n", (void *)jfs, jfs->jfs_id.id);

    urma_token_id_t *tid = urma_alloc_token_id(ctx);
    if (tid == NULL) {
        fprintf(stderr, "alloc_token_id failed\n");
        goto out_jfs;
    }

    /* import 一个伪造 target_jetty：udma_u_post_one_wr 在 UM 模式用 wr->tjetty 取
     * tp_id/rmt_eid，故 tjetty 非空。单进程伪造 rjetty（自引用 eid，UM 模式）。 */
    urma_rjetty_t rjetty = {0};
    rjetty.jetty_id.eid = ctx->eid;       /* 自引用本端 eid */
    rjetty.jetty_id.uasid = ctx->uasid;
    rjetty.jetty_id.id = jfs->jfs_id.id;  /* 用 jfs id 当假 jetty id */
    rjetty.trans_mode = URMA_TM_UM;
    rjetty.type = URMA_JETTY;
    rjetty.tp_type = URMA_UTP;              /* UM 模式必须 UTP（urma_check_tp_type_valid: UM+RTP→非法） */
    rjetty.flag.bs.token_policy = 0;       /* URMA_TOKEN_NONE */
    urma_token_t tok = {0};
    urma_target_jetty_t *tjetty = urma_import_jetty(ctx, &rjetty, &tok);
    if (tjetty == NULL) {
        fprintf(stderr, "import_jetty failed\n");
        goto out_tid;
    }
    fprintf(stderr, "tjetty=%p\n", (void *)tjetty);

    /* 多 WQE 验证：post N 个 READ，每个写到不同 dst 块、不同 user_ctx。
     * 验证 sim 多 CQE 交付：每个 poll 都拿到对应 user_ctx + status=SUCCESS，
     * 且 cq_ci 与真 udma cq.ci 同步（owner 在多 CQE 下不翻错）。 */
#define NUM_WR 4
    char *src_buf = (char *)calloc(1, SEG_LEN);
    char *dst_buf[NUM_WR];
    for (int i = 0; i < NUM_WR; i++) {
        dst_buf[i] = (char *)calloc(1, SEG_LEN);
        if (dst_buf[i] == NULL) {
            fprintf(stderr, "alloc dst[%d] failed\n", i);
            for (int j = 0; j < i; j++) free(dst_buf[j]);
            goto out_tjetty;
        }
        memset(dst_buf[i], 'Z', SEG_LEN);
    }
    if (src_buf == NULL) {
        fprintf(stderr, "alloc src failed\n");
        for (int i = 0; i < NUM_WR; i++) free(dst_buf[i]);
        goto out_tjetty;
    }
    memset(src_buf, 'A', SEG_LEN);

    /* register src seg：ummu_grant 拦截登记 tid→src_va */
    urma_seg_cfg_t src_cfg = {0};
    src_cfg.va = (uint64_t)(uintptr_t)src_buf;
    src_cfg.len = SEG_LEN;
    src_cfg.token_id = tid;
    src_cfg.flag.value = 0;
    src_cfg.flag.bs.access = (0x1 << 1) | (0x1 << 2);   /* READ|WRITE */
    src_cfg.flag.bs.token_id_valid = URMA_TOKEN_ID_VALID; /* token_id 非空，须置 valid */
    src_cfg.flag.bs.token_policy = 0;                    /* URMA_TOKEN_NONE */
    urma_target_seg_t *src_tseg = urma_register_seg(ctx, &src_cfg);
    if (src_tseg == NULL) {
        fprintf(stderr, "register src seg failed\n");
        goto out_buf;
    }

    /* 逐个 post NUM_WR 个 READ WQE：dst=dst_buf[i]，src=src_buf+src_tseg，user_ctx=0x10+i */
    urma_jfs_wr_t *bad_wr = NULL;
    int post_ok = 0;
    for (int i = 0; i < NUM_WR; i++) {
        urma_sge_t dst_sge = {0};
        dst_sge.addr = (uint64_t)(uintptr_t)dst_buf[i];
        dst_sge.len = SEG_LEN;
        dst_sge.tseg = NULL;
        dst_sge.user_tseg = NULL;

        urma_sge_t src_sge = {0};
        src_sge.addr = (uint64_t)(uintptr_t)src_buf;
        src_sge.len = SEG_LEN;
        src_sge.tseg = src_tseg;
        src_sge.user_tseg = NULL;

        urma_jfs_wr_t wr = {0};
        wr.opcode = URMA_OPC_READ;
        wr.flag.value = 0;
        wr.tjetty = tjetty;
        wr.user_ctx = 0x10 + i;   /* 每个 WQE 不同 user_ctx，poll 时验证回填 */
        wr.rw.dst.num_sge = 1;
        wr.rw.dst.sge = &dst_sge;
        wr.rw.src.num_sge = 1;
        wr.rw.src.sge = &src_sge;
        wr.next = NULL;

        ret = urma_post_jfs_wr(jfs, &wr, &bad_wr);
        if (ret != 0) {
            fprintf(stderr, "post[%d] failed ret=%d\n", i, ret);
            goto out_seg;
        }
        post_ok++;
    }
    fprintf(stderr, "post %d WQEs ok\n", post_ok);

    /* 等 sim 轮询线程处理 WQE + 造 CQE */
    usleep(50000);

    /* 逐个 poll NUM_WR 个 CQE，验证 user_ctx 顺序回填 + status=SUCCESS */
    int poll_ok = 0;
    for (int i = 0; i < NUM_WR; i++) {
        urma_cr_t cr = {0};
        int n = urma_poll_jfc(jfc, 1, &cr);
        fprintf(stderr, "poll[%d] n=%d status=%u user_ctx=%llx (expect %x)\n",
                i, n, cr.status, (unsigned long long)cr.user_ctx, 0x10 + i);
        if (n == 1 && cr.status == 0 && cr.user_ctx == (uint64_t)(0x10 + i)) {
            poll_ok++;
        }
    }

    /* 验证：每块 dst 都被改成 'A'（READ 从 src 搬到 dst） */
    int data_ok = 1;
    for (int i = 0; i < NUM_WR; i++) {
        if (dst_buf[i][0] != 'A' || dst_buf[i][SEG_LEN - 1] != 'A') {
            data_ok = 0;
            fprintf(stderr, "dst[%d][0]=%c dst[%d][end]=%c (expect 'A')\n",
                    i, dst_buf[i][0], i, dst_buf[i][SEG_LEN - 1]);
        }
    }
    int ok = (post_ok == NUM_WR && poll_ok == NUM_WR && data_ok);
    fprintf(stderr, "post=%d poll=%d data=%s\n", post_ok, poll_ok, data_ok ? "ok" : "fail");
    fprintf(stderr, "TEST %s\n", ok ? "PASS" : "FAIL");
    rc = ok ? 0 : 1;

out_seg:
    urma_unregister_seg(src_tseg);
out_buf:
    free(src_buf);
    for (int i = 0; i < NUM_WR; i++) free(dst_buf[i]);
out_tjetty:
    urma_unimport_jetty(tjetty);
out_tid:
    urma_free_token_id(tid);
out_jfs:
    urma_delete_jfs(jfs);
out_jfc:
    urma_delete_jfc(jfc);
out_jfce:
    urma_delete_jfce(jfce);
out_ctx:
    urma_delete_context(ctx);
    urma_free_device_list(devs);
    return rc;
}
