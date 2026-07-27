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

ctx_t *setup(int dev_idx, urma_transport_mode_t trans_mode, urma_tp_type_t tp_type)
{
    static int urma_inited = 0;
    ctx_t *c = calloc(1, sizeof(*c));
    if (c == NULL) { fprintf(stderr, "[setup] alloc fail\n"); return NULL; }
    c->tp_type = tp_type;

    if (!urma_inited) {
        urma_init_attr_t init_attr = {0};
        (void)urma_register_log_func(bl_log);
        if (urma_init(&init_attr) != 0) { fprintf(stderr, "[setup] init fail\n"); teardown(c); return NULL; }
        urma_inited = 1;
    }

    int num_devs = 0;
    c->devs = urma_get_device_list(&num_devs);
    if (dev_idx < 0 || dev_idx >= num_devs) { teardown(c); return NULL; }

    c->ctx = urma_create_context(c->devs[dev_idx], 0);
    if (c->ctx == NULL) { teardown(c); return NULL; }
    urma_sim_ipc_start(c->ctx->eid.raw);
    ASSERT_NONEMPTY("ctx", c->ctx);

    c->jfce = urma_create_jfce(c->ctx);
    urma_jfc_cfg_t jfc_cfg = {0}; jfc_cfg.depth = BL_JFC_DEPTH; jfc_cfg.jfce = c->jfce;
    c->jfc = urma_create_jfc(c->ctx, &jfc_cfg);
    if (c->jfc == NULL) { teardown(c); return NULL; }
    ASSERT_NONEMPTY("jfc", c->jfc);

    /* jfr：UM 无 tag matching */
    urma_jfr_cfg_t jfr_cfg = {0};
    jfr_cfg.depth = BL_JETTY_DEPTH;
    jfr_cfg.flag.bs.tag_matching = URMA_NO_TAG_MATCHING;
    jfr_cfg.flag.bs.order_type = 0;
    jfr_cfg.trans_mode = trans_mode;
    jfr_cfg.min_rnr_timer = URMA_TYPICAL_MIN_RNR_TIMER;
    jfr_cfg.jfc = c->jfc;
    jfr_cfg.max_sge = 1;
    c->jfr = urma_create_jfr(c->ctx, &jfr_cfg);
    if (c->jfr == NULL) { teardown(c); return NULL; }
    ASSERT_NONEMPTY("jfr", c->jfr);

    /* jetty(share_jfr)：含 sq+jfr，支持 send/recv + read/write */
    urma_jfs_cfg_t jfs_cfg = {0};
    jfs_cfg.depth = BL_JETTY_DEPTH;
    jfs_cfg.flag.bs.order_type = 0;
    jfs_cfg.trans_mode = trans_mode;
    jfs_cfg.priority = URMA_MAX_PRIORITY;
    jfs_cfg.max_sge = 4;
    jfs_cfg.max_rsge = 4;
    jfs_cfg.max_inline_data = 64;
    jfs_cfg.rnr_retry = URMA_TYPICAL_RNR_RETRY;
    jfs_cfg.err_timeout = URMA_TYPICAL_ERR_TIMEOUT;
    jfs_cfg.jfc = c->jfc;
    urma_jetty_cfg_t jetty_cfg = {0};
    jetty_cfg.flag.bs.share_jfr = 1;
    jetty_cfg.jfs_cfg = jfs_cfg;
    jetty_cfg.shared.jfr = c->jfr;
    c->jetty = urma_create_jetty(c->ctx, &jetty_cfg);
    if (c->jetty == NULL) { teardown(c); return NULL; }
    ASSERT_NONEMPTY("jetty", c->jetty);

    return c;
}

ctx_t *setup_default(int dev_idx)
{
    return setup(dev_idx, URMA_TM_UM, URMA_UTP);
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
    if (c == NULL || c->ctx == NULL || c->jetty == NULL) {
        fprintf(stderr, "[import_self] bad ctx\n");
        return NULL;
    }
    /* 自引用：用 jetty_id 自引用（UM 模式） */
    urma_token_t tok = {0};
    urma_rjetty_t rjetty = {0};
    rjetty.jetty_id.eid = c->ctx->eid;
    rjetty.jetty_id.uasid = c->ctx->uasid;
    rjetty.jetty_id.id = c->jetty->jetty_id.id;
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

urma_target_jetty_t *import_jetty_remote(ctx_t *c, urma_eid_t rmt_eid,
    uint32_t rmt_uasid, uint32_t rmt_jetty_id)
{
    if (!c || !c->ctx) return NULL;
    urma_token_t tok = {0};
    urma_rjetty_t rjetty = {0};
    rjetty.jetty_id.eid = rmt_eid;
    rjetty.jetty_id.uasid = rmt_uasid;
    rjetty.jetty_id.id = rmt_jetty_id;
    rjetty.trans_mode = URMA_TM_UM;
    rjetty.type = URMA_JETTY;
    rjetty.tp_type = URMA_UTP;
    return urma_import_jetty(c->ctx, &rjetty, &tok);
}

urma_target_seg_t *import_seg_remote(ctx_t *c, urma_eid_t rmt_eid,
    uint32_t rmt_uasid, uint32_t rmt_token_id, uint64_t rmt_va, uint64_t rmt_len)
{
    if (!c || !c->ctx) return NULL;
    urma_seg_t remote_seg = {0};
    remote_seg.ubva.eid = rmt_eid;
    remote_seg.ubva.uasid = rmt_uasid;
    remote_seg.ubva.va = rmt_va;
    remote_seg.len = rmt_len;
    remote_seg.token_id = rmt_token_id;
    urma_token_t tok = {0};
    return urma_import_seg(c->ctx, &remote_seg, &tok, 0,
        (urma_import_seg_flag_t){.bs.access = (0x1<<1)|(0x1<<2)});
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
    if (!c || !c->jetty || !tj || !local || !remote_buf || !remote_tseg) return -1;
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
    return urma_post_jetty_send_wr(c->jetty, &wr, &bad);
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

urma_target_jetty_t *bind_remote(ctx_t *c, urma_eid_t rmt_eid,
    uint32_t rmt_uasid, uint32_t rmt_jetty_id)
{
    if (!c || !c->ctx || !c->jetty) return NULL;
    urma_token_t tok = {0};
    urma_rjetty_t rjetty = {0};
    rjetty.jetty_id.eid = rmt_eid;
    rjetty.jetty_id.uasid = rmt_uasid;
    rjetty.jetty_id.id = rmt_jetty_id;
    rjetty.trans_mode = URMA_TM_RC;
    rjetty.type = URMA_JETTY;
    rjetty.tp_type = c->tp_type;
    urma_target_jetty_t *tj = urma_import_jetty(c->ctx, &rjetty, &tok);
    if (tj == NULL) return NULL;
    if (urma_bind_jetty(c->jetty, tj) != 0) {
        urma_unimport_jetty(tj);
        return NULL;
    }
    ASSERT_NONEMPTY("tjetty", tj);
    return tj;
}

int post_recv(ctx_t *c, void *recv_buf, size_t len, uint64_t user_ctx)
{
    if (!c || !c->jetty || !recv_buf) return -1;
    urma_sge_t sge = {0};
    sge.addr = (uint64_t)(uintptr_t)recv_buf;
    sge.len = len;
    urma_sg_t sg = { .sge = &sge, .num_sge = 1 };
    urma_jfr_wr_t wr = { .src = sg, .user_ctx = user_ctx, .next = NULL };
    urma_jfr_wr_t *bad = NULL;
    return urma_post_jetty_recv_wr(c->jetty, &wr, &bad);
}

int post_send(ctx_t *c, urma_target_jetty_t *tj, void *data_buf, size_t len, uint64_t user_ctx)
{
    if (!c || !c->jetty || !tj || !data_buf) return -1;
    urma_sge_t sge = {0};
    sge.addr = (uint64_t)(uintptr_t)data_buf;
    sge.len = len;
    urma_sg_t sg = { .sge = &sge, .num_sge = 1 };
    urma_send_wr_t send_wr = { .src = sg };
    urma_jfs_wr_t wr = {0};
    wr.opcode = URMA_OPC_SEND;
    wr.flag.bs.complete_enable = 1;
    wr.tjetty = tj;
    wr.user_ctx = user_ctx;
    wr.send = send_wr;
    wr.next = NULL;
    urma_jfs_wr_t *bad = NULL;
    return urma_post_jetty_send_wr(c->jetty, &wr, &bad);
}

uint32_t get_jetty_id(ctx_t *c)
{
    if (!c || !c->jetty) return 0;
    return c->jetty->jetty_id.id;
}

void teardown(ctx_t *c)
{
    if (c == NULL) return;
    /* 逆序释放（与建链相反） */
    if (c->jetty) urma_delete_jetty(c->jetty);
    if (c->jfr) urma_delete_jfr(c->jfr);
    if (c->jfs) urma_delete_jfs(c->jfs);
    if (c->jfc) urma_delete_jfc(c->jfc);
    if (c->jfce) urma_delete_jfce(c->jfce);
    if (c->ctx) urma_delete_context(c->ctx);
    if (c->devs) urma_free_device_list(c->devs);
    free(c);
}
