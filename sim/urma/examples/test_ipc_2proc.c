/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * 跨进程 IPC 验证（方式1 扩展）：A/B 两个进程，各持一个 sim 设备，
 * 通过 sim 的 unix-socket IPC 实时传数据，验证突破单进程边界。
 *
 * 设计：
 *   proc_A: udma_sim0(eid=...c0a80a01) register_seg(buf_A='A')
 *           urma_sim_ipc_start(A的eid) 起 IPC socket
 *           把 seg(eid+tid+va) 写到 OOB 文件给 B
 *   proc_B: udma_sim1(eid=...c0a81401) register_seg(buf_B='Z')
 *           urma_sim_ipc_start(B的eid) 起 IPC socket
 *           读 OOB 文件拿 A 的 seg → import_seg
 *           post READ(A 的 seg) → sim 走 IPC 读 A 的 buf → memcpy 进 B 的 buf
 *           验证 B 的 buf 从 'Z' 变 'A'
 *
 * 寻址：A 解 WQE 拿 rmt_eid=对方 eid → 推算对方 socket 路径 → 连。
 *       tid 只在对方进程内查对方自己的 tid→buf 表。
 *
 * 用法（容器内，开两个终端或后台）：
 *   # 进程A（先起，等B）
 *   URMA_SIM_HW=1 LD_PRELOAD=... /tmp/test_ipc_2proc A
 *   # 进程B
 *   URMA_SIM_HW=1 LD_PRELOAD=... /tmp/test_ipc_2proc B
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

/* sim IPC 接口（定义在 liburma_sim.so，链接期解析，运行时 LD_PRELOAD 拦截系统调用） */
extern void urma_sim_ipc_start(const uint8_t *eid);

#define SEG_LEN 4096
#define JFC_DEPTH 64
#define JFS_DEPTH 64
#define OOB_FILE "/tmp/urma_sim_oob"

/* urma 日志回调：只打 ERROR 及以上到 stderr */
static void my_log(int level, char *message)
{
    if (level <= 3) {
        fprintf(stderr, "[urma L%d] %s", level, message);
    }
}

typedef struct {
    urma_eid_t eid;
    uint32_t uasid;
    uint64_t va;
    uint64_t len;
    uint32_t token_id;   /* A 的 seg.token_id（B import 后 tid = token_id>>8） */
} oob_info_t;

int main(int argc, char **argv)
{
    int rc = 1;
    urma_status_t ret;
    int num_devs = 0;
    int is_a = (argc > 1 && strcmp(argv[1], "A") == 0);
    const char *dev_name = is_a ? "udma_sim0" : "udma_sim1";

    urma_init_attr_t init_attr = {0};
    (void)urma_register_log_func(my_log);
    ret = urma_init(&init_attr);
    if (ret != 0) {
        fprintf(stderr, "[%s] urma_init failed: %d\n", dev_name, ret);
        return 1;
    }

    urma_device_t **devs = urma_get_device_list(&num_devs);
    if (devs == NULL || num_devs == 0) {
        fprintf(stderr, "[%s] no device\n", dev_name);
        return 1;
    }
    urma_device_t *dev = NULL;
    for (int i = 0; i < num_devs; i++) {
        if (strcmp(devs[i]->name, dev_name) == 0) { dev = devs[i]; break; }
    }
    if (dev == NULL) {
        fprintf(stderr, "[%s] device not found\n", dev_name);
        return 1;
    }
    fprintf(stderr, "[%s] found\n", dev_name);

    urma_context_t *ctx = urma_create_context(dev, 0);
    if (ctx == NULL) {
        fprintf(stderr, "[%s] create_context failed\n", dev_name);
        return 1;
    }
    /* 起本进程 IPC socket（路径含本进程 eid） */
    urma_sim_ipc_start(ctx->eid.raw);
    fprintf(stderr, "[%s] ctx eid hex: %02x%02x%02x%02x%02x%02x%02x%02x\n", dev_name,
            ctx->eid.raw[8], ctx->eid.raw[9], ctx->eid.raw[10], ctx->eid.raw[11],
            ctx->eid.raw[12], ctx->eid.raw[13], ctx->eid.raw[14], ctx->eid.raw[15]);

    urma_jfce_t *jfce = urma_create_jfce(ctx);
    urma_jfc_cfg_t jfc_cfg = {0};
    jfc_cfg.depth = JFC_DEPTH;
    jfc_cfg.jfce = jfce;
    urma_jfc_t *jfc = urma_create_jfc(ctx, &jfc_cfg);
    urma_jfs_cfg_t jfs_cfg = {0};
    jfs_cfg.depth = JFS_DEPTH;
    jfs_cfg.trans_mode = URMA_TM_UM;
    jfs_cfg.max_sge = 4;
    jfs_cfg.max_rsge = 4;
    jfs_cfg.jfc = jfc;
    urma_jfs_t *jfs = urma_create_jfs(ctx, &jfs_cfg);

    urma_token_id_t *tid_obj = urma_alloc_token_id(ctx);
    char *buf = (char *)calloc(1, SEG_LEN);
    memset(buf, is_a ? 'A' : 'Z', SEG_LEN);   /* A 端填 'A'，B 端填 'Z' */

    urma_seg_cfg_t seg_cfg = {0};
    seg_cfg.va = (uint64_t)(uintptr_t)buf;
    seg_cfg.len = SEG_LEN;
    seg_cfg.token_id = tid_obj;
    seg_cfg.flag.bs.access = (0x1 << 1) | (0x1 << 2);
    seg_cfg.flag.bs.token_id_valid = URMA_TOKEN_ID_VALID;
    urma_target_seg_t *tseg = urma_register_seg(ctx, &seg_cfg);
    fprintf(stderr, "[%s] registered seg va=%p\n", dev_name, (void *)buf);

    if (is_a) {
        /* A: 把自己的 seg 信息写 OOB 文件给 B */
        oob_info_t info = {0};
        info.eid = ctx->eid;
        info.uasid = ctx->uasid;
        info.va = (uint64_t)(uintptr_t)buf;
        info.len = SEG_LEN;
        info.token_id = tseg->seg.token_id;   /* A 的 token_id，B import 后 tid=token_id>>8 */
        FILE *f = fopen(OOB_FILE, "wb");
        fwrite(&info, sizeof(info), 1, f);
        fclose(f);
        fprintf(stderr, "[A] wrote OOB (token_id=0x%x), waiting 30s for B to READ me...\n",
                info.token_id);
        sleep(30);   /* 保持 IPC socket 活着，让 B 能连 */
        fprintf(stderr, "[A] done\n");
    } else {
        /* B: 读 A 的 OOB，import A 的 seg，post READ(A) */
        oob_info_t info = {0};
        for (int i = 0; i < 100; i++) {
            FILE *f = fopen(OOB_FILE, "rb");
            if (f) { fread(&info, sizeof(info), 1, f); fclose(f); break; }
            usleep(50000);
        }
        if (info.eid.raw[15] == 0) {
            fprintf(stderr, "[B] no OOB info from A\n");
            goto out;
        }
        fprintf(stderr, "[B] got A's seg (token_id=0x%x), import it\n", info.token_id);
        /* 构造 A 的 remote_seg（含 A 的 eid + token_id + va） */
        urma_seg_t remote_seg = {0};
        remote_seg.ubva.eid = info.eid;
        remote_seg.ubva.uasid = info.uasid;
        remote_seg.ubva.va = info.va;
        remote_seg.len = info.len;
        remote_seg.token_id = info.token_id;   /* B import 后 imp_tseg.tid = token_id>>8 = A 的 tid */
        urma_token_t tok = {0};
        urma_target_seg_t *imp_tseg = urma_import_seg(ctx, &remote_seg, &tok, 0,
            (urma_import_seg_flag_t){.bs.access = (0x1<<1)|(0x1<<2)});
        if (imp_tseg == NULL) { fprintf(stderr, "[B] import_seg failed\n"); goto out; }

        /* 伪造 rjetty 指向 A（用 A 的 eid），post READ 把 A 的数据读进 B 的 buf */
        urma_rjetty_t rjetty = {0};
        rjetty.jetty_id.eid = info.eid;
        rjetty.jetty_id.uasid = info.uasid;
        rjetty.jetty_id.id = jfs->jfs_id.id;
        rjetty.trans_mode = URMA_TM_UM;
        rjetty.type = URMA_JETTY;
        rjetty.tp_type = URMA_UTP;
        urma_target_jetty_t *tjetty = urma_import_jetty(ctx, &rjetty, &tok);
        if (tjetty == NULL) { fprintf(stderr, "[B] import_jetty failed\n"); goto out; }

        urma_sge_t dst_sge = {0};
        dst_sge.addr = (uint64_t)(uintptr_t)buf;   /* B 本地 dst */
        dst_sge.len = SEG_LEN;
        dst_sge.tseg = NULL;
        urma_sge_t src_sge = {0};
        src_sge.addr = info.va;   /* A 的 va（跨进程无意义，sim 用 rmt_eid+tid 走 IPC） */
        src_sge.len = SEG_LEN;
        src_sge.tseg = imp_tseg;

        urma_jfs_wr_t wr = {0};
        wr.opcode = URMA_OPC_READ;
        wr.tjetty = tjetty;
        wr.user_ctx = 0x1234;
        wr.rw.dst.num_sge = 1;
        wr.rw.dst.sge = &dst_sge;
        wr.rw.src.num_sge = 1;
        wr.rw.src.sge = &src_sge;
        urma_jfs_wr_t *bad_wr = NULL;
        ret = urma_post_jfs_wr(jfs, &wr, &bad_wr);
        fprintf(stderr, "[B] post READ ret=%d\n", ret);
        usleep(200000);   /* 等 IPC 往返 + 轮询造 CQE */
        urma_cr_t cr = {0};
        int n = urma_poll_jfc(jfc, 1, &cr);
        fprintf(stderr, "[B] poll n=%d status=%u user_ctx=%llx\n",
                n, cr.status, (unsigned long long)cr.user_ctx);
        int ok = (buf[0] == 'A' && buf[SEG_LEN-1] == 'A');
        fprintf(stderr, "[B] buf[0]=%c buf[end]=%c (expect 'A')\n", buf[0], buf[SEG_LEN-1]);
        fprintf(stderr, "TEST %s\n", ok ? "PASS" : "FAIL");
        rc = ok ? 0 : 1;
        urma_unimport_jetty(tjetty);
        urma_unimport_seg(imp_tseg);
    }

out:
    urma_unregister_seg(tseg);
    free(buf);
    urma_free_token_id(tid_obj);
    urma_delete_jfs(jfs);
    urma_delete_jfc(jfc);
    urma_delete_jfce(jfce);
    urma_delete_context(ctx);
    urma_free_device_list(devs);
    return rc;
}
