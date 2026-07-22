/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * baseline 测试 fixture：封装 sim 测试重复的建链/释放/操作样板，自带 ASSERT。
 * 写测试源时直接调这些函数，不必每次手抄 init→create_context→jfc→jfs→seg→import 那套。
 *
 * 模块化点（每个测试都会用到的样板）：
 *   setup        —— 一把梭建链（init+设备+ctx+ipc+jfce+jfc+jfs+jfr+jetty+seg 容器），失败 ASSERT 退出
 *   register_seg —— register_seg（含 buf 分配）
 *   import_self  —— import 自引用 jetty（UM 模式）
 *   post_read   —— post READ（构造 wr + sge + post）
 *   poll        —— poll 循环封装（重试 100 次）
 *   teardown    —— 逆序释放全部（不漏不乱序，不用手写 cleanup 链）
 *
 * 函数失败时打 stderr + 返回错误码/NULL（带 ASSERT 语义：调者不必再判空）。
 */

#ifndef BL_FIXTURE_H
#define BL_FIXTURE_H

#include <stdint.h>
#include <stddef.h>
#include "urma_api.h"
#include "urma_types.h"
#include "urma_opcode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ctx_t：测试一次运行的容器，聚合所有建链对象，避免测试作者管一堆局部变量。 */
typedef struct {
    urma_context_t *ctx;
    urma_jfce_t *jfce;
    urma_jfc_t *jfc;
    urma_jfs_t *jfs;
    urma_jfr_t *jfr;       /* share_jfr 模式下 jetty 共享的 jfr */
    urma_jetty_t *jetty;    /* UM 模式 jetty */
    urma_device_t **devs;  /* urma_get_device_list 返回，teardown 释放 */
} ctx_t;

/* 一把梭建链：init + 找设备 + create_context + 起 IPC + jfce/jfc/jfs。
 * dev_idx：第几台设备（0=udma_sim0，1=udma_sim1...）——测试一般不关心哪台，
 *          只在测仿真与实际环境差异时才需特定 idx。
 * trans_mode：传输模式（URMA_TM_UM/RC/RM），不同测试可能改。其他 jfs 字段用默认。
 * 内部对 ctx/jfc/jfs 调 ASSERT_NONEMPTY 验非空——测试作者不必再写 NONEMPTY。
 * 失败打 stderr + 返回 NULL。 */
ctx_t *setup(int dev_idx, urma_transport_mode_t trans_mode);

/* register_seg：给 buf 注册 seg。buf 由 caller 分配（或传 NULL 让 fixture calloc）。
 * 返回 target_seg，失败 NULL。 */
urma_target_seg_t *register_seg(ctx_t *c, void *buf, size_t len);

/* import 自引用 jetty（UM 模式，用 jfs_id 当 jetty id）。
 * 返回 target_jetty，失败 NULL。 */
urma_target_jetty_t *import_self(ctx_t *c);

/* 统一 post：READ/WRITE/CAS/FAA 共用。opcode 定行为。
 * local=本端 buf，remote_buf=远端 va，remote_tseg=远端 seg，len，user_ctx。
 * extra1/extra2：CAS 用 cmp/swap，FAA 用 operand。 */
int post_rw(ctx_t *c, urma_target_jetty_t *tj, urma_opcode_t opcode,
            void *local, void *remote_buf, urma_target_seg_t *remote_tseg,
            size_t len, uint64_t user_ctx, uint64_t extra1, uint64_t extra2);

/* 便捷包装（内部调 post_rw）： */
int post_read(ctx_t *c, urma_target_jetty_t *tj, void *dst, void *src_buf,
              urma_target_seg_t *src_tseg, size_t len, uint64_t user_ctx);

/* post WRITE：src=本端 buf，dst_tseg=远端 seg，dst_buf=远端 buf va，len，user_ctx。 */
int post_write(ctx_t *c, urma_target_jetty_t *tj, void *src_buf,
               urma_target_seg_t *dst_tseg, void *dst_buf, size_t len, uint64_t user_ctx);

/* post CAS：atomic_buf=远端原子变量（tseg 注册），orig_buf=本端存原值，
 * cmp_data/swap_data，len（4/8），user_ctx。 */
int post_cas(ctx_t *c, urma_target_jetty_t *tj, void *atomic_buf,
            urma_target_seg_t *tseg, void *orig_buf, uint64_t cmp, uint64_t swap,
            size_t len, uint64_t user_ctx);

/* post FAA：atomic_buf=远端原子变量（tseg 注册），orig_buf=本端存原值，
 * operand，len（4/8），user_ctx。 */
int post_faa(ctx_t *c, urma_target_jetty_t *tj, void *atomic_buf,
             urma_target_seg_t *tseg, void *orig_buf, uint64_t operand,
             size_t len, uint64_t user_ctx);

/* poll 封装：重试最多 100 次（每次间隔 100ms），拿到 CQE 返回 n（>0）。 */
int poll_cr(ctx_t *c, urma_cr_t *cr);

/* 逆序释放全部（jetty/jfr/jfs/jfc/jfce/ctx/devs）。c 本身 free。安全传 NULL。 */
void teardown(ctx_t *c);

#ifdef __cplusplus
}
#endif

#endif /* BL_FIXTURE_H */
