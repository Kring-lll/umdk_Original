// SPDX-License-Identifier: MIT
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
//
// baseline 测试 fixture（C++ 版）：封装 sim 建链/释放/操作样板。
// ctx_t 带析构函数——出作用域自动释放，RAII 不用 teardown/goto cleanup。
//
// 模块化函数（与 C 版同名，签名兼容）：
//   setup        —— 建链（返回 ctx_t*，需 delete；或直接 new ctx_t + 调 build()）
//   register_seg —— register_seg
//   import_self  —— import 自引用 jetty
//   post_read    —— post READ
//   poll_cr      —— poll 循环
// teardown 保留（兼容旧调用），但 ctx_t 出作用域析构自动调。

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
    urma_device_t **devs = nullptr;
    ~ctx_t();  // 析构自动释放——RAII
};

ctx_t *setup(int dev_idx, urma_transport_mode_t trans_mode);
urma_target_seg_t *register_seg(ctx_t *c, void *buf, size_t len);
urma_target_jetty_t *import_self(ctx_t *c);
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
int poll_cr(ctx_t *c, urma_cr_t *cr);
void teardown(ctx_t *c);
