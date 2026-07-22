/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * baseline 测试 fixture 实现：封装 sim 测试重复的建链/释放/操作样板。
 */

#include "bl_fixture.h"
#include "baseline_macros.h"   /* ASSERT_NONEMPTY：内部验非空，测试作者不必再写 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C"
#endif
void urma_sim_ipc_start(const uint8_t *eid);

#define BL_JFC_DEPTH 64
#define BL_JETTY_DEPTH 256

/* 内部：日志函数（复用各测试的 my_log 风格） */
static void bl_log(int level, char *message)
{
    if (level <= 3) fprintf(stderr, "[urma L%d] %s", level, message);
}

ctx_t *setup(int dev_idx, urma_transport_mode_t trans_mode)
{
    ctx_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        fprintf(stderr, "[setup] alloc ctx fail\n");
        return NULL;
    }

    urma_init_attr_t init_attr = {0};
    (void)urma_register_log_func(bl_log);
    if (urma_init(&init_attr) != 0) {
        fprintf(stderr, "[setup] urma_init fail\n");
        teardown(c); return NULL;
    }

    int num_devs = 0;
    c->devs = urma_get_device_list(&num_devs);
    if (dev_idx < 0 || dev_idx >= num_devs) {
        fprintf(stderr, "[setup] device idx %d out of range (num_devs=%d)\n", dev_idx, num_devs);
        teardown(c); return NULL;
    }
    urma_device_t *dev = c->devs[dev_idx];

    c->ctx = urma_create_context(dev, 0);
    if (c->ctx == NULL) {
        fprintf(stderr, "[setup] create_context fail\n");
        teardown(c); return NULL;
    }
    urma_sim_ipc_start(c->ctx->eid.raw);
    ASSERT_NONEMPTY("ctx", c->ctx);

    c->jfce = urma_create_jfce(c->ctx);
    urma_jfc_cfg_t jfc_cfg = {0};
    jfc_cfg.depth = BL_JFC_DEPTH;
    jfc_cfg.jfce = c->jfce;
    c->jfc = urma_create_jfc(c->ctx, &jfc_cfg);
    if (c->jfc == NULL) {
        fprintf(stderr, "[setup] create_jfc fail\n");
        teardown(c); return NULL;
    }
    ASSERT_NONEMPTY("jfc", c->jfc);

    /* jfs：trans_mode 由入参定，其他用默认。要改 max_sge/max_inline 等可另开接口 */
    urma_jfs_cfg_t jfs_cfg = {0};
    jfs_cfg.depth = BL_JETTY_DEPTH;
    jfs_cfg.flag.value = 0;
    jfs_cfg.trans_mode = trans_mode;
    jfs_cfg.priority = 0;
    jfs_cfg.max_sge = 4;
    jfs_cfg.max_rsge = 4;
    jfs_cfg.max_inline_data = 64;
    jfs_cfg.rnr_retry = 0;
    jfs_cfg.err_timeout = 0;
    jfs_cfg.jfc = c->jfc;
    c->jfs = urma_create_jfs(c->ctx, &jfs_cfg);
    if (c->jfs == NULL) {
        fprintf(stderr, "[setup] create_jfs fail\n");
        teardown(c); return NULL;
    }
    ASSERT_NONEMPTY("jfs", c->jfs);

    return c;
    teardown(c);
    return NULL;
}

urma_target_seg_t *register_seg(ctx_t *c, void *buf, size_t len)
{
    if (c == NULL || c->ctx == NULL || buf == NULL || len == 0) {
        fprintf(stderr, "[register_seg] bad args\n");
        return NULL;
    }
    urma_seg_cfg_t seg_cfg = {0};
    seg_cfg.va = (uint64_t)(uintptr_t)buf;
    seg_cfg.len = len;
    urma_target_seg_t *tseg = urma_register_seg(c->ctx, &seg_cfg);
    if (tseg == NULL) {
        fprintf(stderr, "[register_seg] register_seg fail\n");
        return NULL;
    }
    ASSERT_NONEMPTY("seg", tseg);   /* 内部验非空，测试作者不必再写 */
    return tseg;
}

urma_target_jetty_t *import_self(ctx_t *c)
{
    if (c == NULL || c->ctx == NULL || c->jfs == NULL) {
        fprintf(stderr, "[import_self] bad ctx\n");
        return NULL;
    }
    /* 自引用：用 jfs_id 当 jetty id（UM 模式，仿 test_hw_post） */
    urma_token_t tok = {0};
    urma_rjetty_t rjetty = {0};
    rjetty.jetty_id.eid = c->ctx->eid;
    rjetty.jetty_id.uasid = c->ctx->uasid;
    rjetty.jetty_id.id = c->jfs->jfs_id.id;
    rjetty.trans_mode = URMA_TM_UM;
    rjetty.type = URMA_JETTY;
    rjetty.tp_type = URMA_UTP;
    urma_target_jetty_t *tj = urma_import_jetty(c->ctx, &rjetty, &tok);
    if (tj == NULL) {
        fprintf(stderr, "[import_self] import_jetty fail\n");
        return NULL;
    }
    ASSERT_NONEMPTY("tjetty", tj);   /* 内部验非空 */
    return tj;
}

/* 统一 post：READ/WRITE/CAS/FAA 共用。
 * READ/WRITE：local=本端 buf，remote_tseg=远端 seg，remote_buf=远端 va
 * CAS/FAA：local=本端存原值 buf，remote_tseg=原子变量 seg，remote_buf=原子变量 va
 * extra1/extra2：CAS 用 cmp/swap，FAA 用 operand（extra2 不用）
 * opcode 决定走 rw.dst/src 还是 cas.dst/faa.dst（都是 sge 指针）*/
int post_rw(ctx_t *c, urma_target_jetty_t *tj, urma_opcode_t opcode,
            void *local, void *remote_buf, urma_target_seg_t *remote_tseg,
            size_t len, uint64_t user_ctx, uint64_t extra1, uint64_t extra2)
{
    if (!c || !c->jfs || !tj || !local || !remote_buf || !remote_tseg) return -1;
    urma_sge_t rsg = {0}; rsg.addr = (uint64_t)(uintptr_t)remote_buf; rsg.len = len; rsg.tseg = remote_tseg;
    urma_sge_t lsg = {0}; lsg.addr = (uint64_t)(uintptr_t)local; lsg.len = len;
    urma_jfs_wr_t wr = {0};
    wr.opcode = opcode; wr.flag.bs.complete_enable = 1;
    wr.tjetty = tj; wr.user_ctx = user_ctx;
    if (opcode == URMA_OPC_CAS) {
        wr.cas.dst = &rsg; wr.cas.src = &lsg;
        wr.cas.cmp_data = extra1; wr.cas.swap_data = extra2;
    } else if (opcode == URMA_OPC_FADD) {
        wr.faa.dst = &rsg; wr.faa.src = &lsg;
        wr.faa.operand = extra1;
    } else {
        /* READ/WRITE：rw.dst/src 是 urma_sg_t（含 sge 指针+num）。
         * READ：dst=本端 local，src=远端 remote（带 tseg）
         * WRITE：dst=远端 remote（带 tseg），src=本端 local */
        urma_sg_t rsg_wrap = { .sge = &rsg, .num_sge = 1 };
        urma_sg_t lsg_wrap = { .sge = &lsg, .num_sge = 1 };
        if (opcode == URMA_OPC_READ) {
            wr.rw.dst = lsg_wrap; wr.rw.src = rsg_wrap;  /* dst=local, src=remote */
        } else {
            wr.rw.dst = rsg_wrap; wr.rw.src = lsg_wrap;  /* dst=remote, src=local */
        }
    }
    urma_jfs_wr_t *bad = NULL;
    return urma_post_jfs_wr(c->jfs, &wr, &bad);
}

/* 便捷包装：保持调用简洁 */
int post_read(ctx_t *c, urma_target_jetty_t *tj, void *dst, void *src_buf,
              urma_target_seg_t *src_tseg, size_t len, uint64_t user_ctx)
{
    /* READ：local=dst（本端），remote=src_buf+src_tseg（远端源） */
    return post_rw(c, tj, URMA_OPC_READ, dst, src_buf, src_tseg, len, user_ctx, 0, 0);
}

int post_write(ctx_t *c, urma_target_jetty_t *tj, void *src_buf,
               urma_target_seg_t *dst_tseg, void *dst_buf, size_t len, uint64_t user_ctx)
{
    /* WRITE：local=src_buf（本端），remote=dst_buf+dst_tseg（远端目标） */
    return post_rw(c, tj, URMA_OPC_WRITE, src_buf, dst_buf, dst_tseg, len, user_ctx, 0, 0);
}

int post_cas(ctx_t *c, urma_target_jetty_t *tj, void *atomic_buf,
            urma_target_seg_t *tseg, void *orig_buf, uint64_t cmp, uint64_t swap,
            size_t len, uint64_t user_ctx)
{
    /* CAS：local=orig_buf（原值回写），remote=atomic_buf+tseg */
    return post_rw(c, tj, URMA_OPC_CAS, orig_buf, atomic_buf, tseg, len, user_ctx, cmp, swap);
}

int post_faa(ctx_t *c, urma_target_jetty_t *tj, void *atomic_buf,
             urma_target_seg_t *tseg, void *orig_buf, uint64_t operand,
             size_t len, uint64_t user_ctx)
{
    /* FAA：local=orig_buf（原值回写），remote=atomic_buf+tseg */
    return post_rw(c, tj, URMA_OPC_FADD, orig_buf, atomic_buf, tseg, len, user_ctx, operand, 0);
}

int poll_cr(ctx_t *c, urma_cr_t *cr)
{
    if (c == NULL || c->jfc == NULL || cr == NULL) {
        fprintf(stderr, "[poll] bad args\n");
        return -1;
    }
    for (int i = 0; i < 100; i++) {
        int n = urma_poll_jfc(c->jfc, 1, cr);
        if (n > 0) return n;
        usleep(100000);
    }
    return 0;
}

void teardown(ctx_t *c)
{
    if (c == NULL) return;
    /* 逆序释放（与建链相反） */
    if (c->jfs) urma_delete_jfs(c->jfs);
    if (c->jfc) urma_delete_jfc(c->jfc);
    if (c->jfce) urma_delete_jfce(c->jfce);
    if (c->ctx) urma_delete_context(c->ctx);
    if (c->devs) urma_free_device_list(c->devs);
    free(c);
}
