// SPDX-License-Identifier: MIT
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
//
// baseline 测试 fixture（C++ 版）：封装 sim 建链/释放/操作样板。
// ctx_t 带析构函数——出作用域自动释放，RAII。

#pragma once
#include <stdint.h>
#include <stddef.h>
#include "urma_api.h"
#include "urma_types.h"
#include "urma_opcode.h"

struct ctx_t {
    urma_context_t *ctx = nullptr;
    urma_jfce_t *jfce = nullptr;
    urma_jfc_t *jfc = nullptr;
    urma_jfs_t *jfs = nullptr;
    urma_jfr_t *jfr = nullptr;
    urma_jetty_t *jetty = nullptr;
    urma_device_t **devs = nullptr;
    urma_tp_type_t tp_type = URMA_UTP;
    ~ctx_t();
};

ctx_t *setup(int dev_idx, urma_transport_mode_t trans_mode, urma_tp_type_t tp_type);
ctx_t *setup_default(int dev_idx);
urma_target_seg_t *register_seg(ctx_t *c, void *buf, size_t len);
urma_target_jetty_t *import_self(ctx_t *c);
urma_target_jetty_t *bind_remote(ctx_t *c, urma_eid_t rmt_eid,
    uint32_t rmt_uasid, uint32_t rmt_jetty_id);
urma_target_jetty_t *import_jetty_remote(ctx_t *c, urma_eid_t rmt_eid,
    uint32_t rmt_uasid, uint32_t rmt_jetty_id);
urma_target_seg_t *import_seg_remote(ctx_t *c, urma_eid_t rmt_eid,
    uint32_t rmt_uasid, uint32_t rmt_token_id, uint64_t rmt_va, uint64_t rmt_len);
int post_rw(ctx_t *c, urma_target_jetty_t *tj, urma_opcode_t opcode,
            void *local, void *remote_buf, urma_target_seg_t *remote_tseg,
            size_t len, uint64_t user_ctx, uint64_t extra1, uint64_t extra2);
int post_read(ctx_t *c, urma_target_jetty_t *tj, void *dst, void *src_buf,
              urma_target_seg_t *src_tseg, size_t len, uint64_t user_ctx);
int post_write(ctx_t *c, urma_target_jetty_t *tj, void *src_buf,
               urma_target_seg_t *dst_tseg, void *dst_buf, size_t len, uint64_t user_ctx);
int post_cas(ctx_t *c, urma_target_jetty_t *tj, void *atomic_buf,
            urma_target_seg_t *tseg, void *orig_buf, uint64_t cmp, uint64_t swap,
            size_t len, uint64_t user_ctx);
int post_faa(ctx_t *c, urma_target_jetty_t *tj, void *atomic_buf,
             urma_target_seg_t *tseg, void *orig_buf, uint64_t operand,
             size_t len, uint64_t user_ctx);
int post_recv(ctx_t *c, void *recv_buf, size_t len, uint64_t user_ctx);
int post_send(ctx_t *c, urma_target_jetty_t *tj, void *data_buf, size_t len, uint64_t user_ctx);
int poll_cr(ctx_t *c, urma_cr_t *cr);
uint32_t get_jetty_id(ctx_t *c);
void teardown(ctx_t *c);
