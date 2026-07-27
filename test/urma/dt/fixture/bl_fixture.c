/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */

#include "bl_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

__attribute__((weak)) void urma_sim_ipc_start(const uint8_t *eid);

#define BL_JFC_DEPTH 64
#define BL_JETTY_DEPTH 256

static void bl_log(int level, char *message)
{
    if (level <= 3) {
        fprintf(stderr, "[urma L%d] %s", level, message);
    }
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
        fprintf(stderr, "[setup] init fail\n");
        teardown(c);
        return NULL;
    }

    int num_devs = 0;
    c->devs = urma_get_device_list(&num_devs);
    fprintf(stderr, "[setup] num_devs=%d\n", num_devs);
    if (dev_idx < 0 || dev_idx >= num_devs) {
        fprintf(stderr, "[setup] dev idx\n");
        teardown(c);
        return NULL;
    }

    c->ctx = urma_create_context(c->devs[dev_idx], 0);
    if (c->ctx == NULL) {
        teardown(c);
        return NULL;
    }
    if (urma_sim_ipc_start != NULL) {
        urma_sim_ipc_start(c->ctx->eid.raw);
    }

    c->jfce = urma_create_jfce(c->ctx);
    urma_jfc_cfg_t jfc_cfg = {0};
    jfc_cfg.depth = BL_JFC_DEPTH;
    jfc_cfg.jfce = c->jfce;
    c->jfc = urma_create_jfc(c->ctx, &jfc_cfg);
    if (c->jfc == NULL) {
        teardown(c);
        return NULL;
    }

    urma_jfs_cfg_t jfs_cfg = {0};
    jfs_cfg.depth = BL_JETTY_DEPTH;
    jfs_cfg.flag.value = 0;
    jfs_cfg.trans_mode = trans_mode;
    jfs_cfg.max_sge = 4;
    jfs_cfg.max_rsge = 4;
    jfs_cfg.max_inline_data = 64;
    jfs_cfg.jfc = c->jfc;
    c->jfs = urma_create_jfs(c->ctx, &jfs_cfg);
    if (c->jfs == NULL) {
        teardown(c);
        return NULL;
    }

    return c;
}

urma_target_seg_t *register_seg(ctx_t *c, void *buf, size_t len)
{
    if (c == NULL || c->ctx == NULL || buf == NULL || len == 0) {
        return NULL;
    }

    urma_seg_cfg_t cfg = {0};
    cfg.va = (uint64_t)(uintptr_t)buf;
    cfg.len = len;
    return urma_register_seg(c->ctx, &cfg);
}

urma_target_jetty_t *import_self(ctx_t *c)
{
    if (c == NULL || c->ctx == NULL || c->jfs == NULL) {
        return NULL;
    }

    urma_token_t tok = {0};
    urma_rjetty_t rjetty = {0};
    rjetty.jetty_id.eid = c->ctx->eid;
    rjetty.jetty_id.uasid = c->ctx->uasid;
    rjetty.jetty_id.id = c->jfs->jfs_id.id;
    rjetty.trans_mode = URMA_TM_UM;
    rjetty.type = URMA_JETTY;
    rjetty.tp_type = URMA_UTP;
    return urma_import_jetty(c->ctx, &rjetty, &tok);
}

int post_rw(ctx_t *c, urma_target_jetty_t *tj, urma_opcode_t opcode,
            void *local, void *remote_buf, urma_target_seg_t *remote_tseg,
            size_t len, uint64_t user_ctx, uint64_t extra1, uint64_t extra2)
{
    if (c == NULL || c->jfs == NULL || tj == NULL || local == NULL ||
        remote_buf == NULL || remote_tseg == NULL) {
        return -1;
    }

    urma_sge_t rsg = {0};
    rsg.addr = (uint64_t)(uintptr_t)remote_buf;
    rsg.len = len;
    rsg.tseg = remote_tseg;

    urma_sge_t lsg = {0};
    lsg.addr = (uint64_t)(uintptr_t)local;
    lsg.len = len;

    urma_jfs_wr_t wr = {0};
    wr.opcode = opcode;
    wr.flag.bs.complete_enable = 1;
    wr.tjetty = tj;
    wr.user_ctx = user_ctx;

    if (opcode == URMA_OPC_CAS) {
        wr.cas.dst = &rsg;
        wr.cas.src = &lsg;
        wr.cas.cmp_data = extra1;
        wr.cas.swap_data = extra2;
    } else if (opcode == URMA_OPC_FADD) {
        wr.faa.dst = &rsg;
        wr.faa.src = &lsg;
        wr.faa.operand = extra1;
    } else {
        urma_sg_t rsg_wrap = {.sge = &rsg, .num_sge = 1};
        urma_sg_t lsg_wrap = {.sge = &lsg, .num_sge = 1};
        if (opcode == URMA_OPC_READ) {
            wr.rw.dst = lsg_wrap;
            wr.rw.src = rsg_wrap;
        } else {
            wr.rw.dst = rsg_wrap;
            wr.rw.src = lsg_wrap;
        }
    }

    urma_jfs_wr_t *bad = NULL;
    return urma_post_jfs_wr(c->jfs, &wr, &bad);
}

int post_read(ctx_t *c, urma_target_jetty_t *tj, void *dst, void *src_buf,
              urma_target_seg_t *src_tseg, size_t len, uint64_t user_ctx)
{
    return post_rw(c, tj, URMA_OPC_READ, dst, src_buf, src_tseg, len, user_ctx, 0, 0);
}

int post_write(ctx_t *c, urma_target_jetty_t *tj, void *src_buf,
               urma_target_seg_t *dst_tseg, void *dst_buf, size_t len, uint64_t user_ctx)
{
    return post_rw(c, tj, URMA_OPC_WRITE, src_buf, dst_buf, dst_tseg, len, user_ctx, 0, 0);
}

int post_cas(ctx_t *c, urma_target_jetty_t *tj, void *atomic_buf,
             urma_target_seg_t *tseg, void *orig_buf, uint64_t cmp, uint64_t swap,
             size_t len, uint64_t user_ctx)
{
    return post_rw(c, tj, URMA_OPC_CAS, orig_buf, atomic_buf, tseg, len, user_ctx, cmp, swap);
}

int post_faa(ctx_t *c, urma_target_jetty_t *tj, void *atomic_buf,
             urma_target_seg_t *tseg, void *orig_buf, uint64_t operand,
             size_t len, uint64_t user_ctx)
{
    return post_rw(c, tj, URMA_OPC_FADD, orig_buf, atomic_buf, tseg, len, user_ctx, operand, 0);
}

int poll_cr(ctx_t *c, urma_cr_t *cr)
{
    if (c == NULL || c->jfc == NULL || cr == NULL) {
        return -1;
    }

    for (int i = 0; i < 100; i++) {
        int n = urma_poll_jfc(c->jfc, 1, cr);
        if (n > 0) {
            return n;
        }
        usleep(100000);
    }
    return 0;
}

void teardown(ctx_t *c)
{
    if (c == NULL) {
        return;
    }
    if (c->jfs) urma_delete_jfs(c->jfs);
    if (c->jfc) urma_delete_jfc(c->jfc);
    if (c->jfce) urma_delete_jfce(c->jfce);
    if (c->ctx) urma_delete_context(c->ctx);
    if (c->devs) urma_free_device_list(c->devs);
    free(c);
}
