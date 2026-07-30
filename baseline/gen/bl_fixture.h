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
    urma_jetty_t *jetty;    /* share_jfr jetty（RC/RM/UM 都可用） */
    urma_device_t **devs;  /* urma_get_device_list 返回，teardown 释放 */
    urma_tp_type_t tp_type; /* 保存 setup 时的 tp_type（RTP/CTP） */
} ctx_t;

/* 一把梭建链：init + 找设备 + create_context + 起 IPC + jfce/jfc/jfr/jetty（share_jfr）。
 * dev_idx：第几台设备（0=udma_sim0，1=udma_sim1...）
 * trans_mode：UM/RC/RM
 * tp_type：URMA_UTP（默认）或 URMA_CTP（共享 TP）
 * 用 jetty(share_jfr) 而非裸 jfs——jetty 含 sq+jfr，支持 send/recv。
 * 内部 ASSERT_NONEMPTY 验非空。
 * 失败返回 NULL。 */
ctx_t *setup(int dev_idx, urma_transport_mode_t trans_mode, urma_tp_type_t tp_type);

/* 便捷：UM+UTP（最常用，兼容旧调用） */
ctx_t *setup_default(int dev_idx);

/* 探测有无真设备（不依赖 sim）。0=有真设备可测，非 0=无设备。
 * no-sim 模式用它判断该不该跑功能测试，避免"无设备假 PASS"。 */
int probe_device(void);

/* register_seg：给 buf 注册 seg。buf 由 caller 分配。
 * 返回 target_seg，失败 NULL。 */
urma_target_seg_t *register_seg(ctx_t *c, void *buf, size_t len);

/* import 自引用 jetty（UM 模式，用 jetty_id 自引用）。
 * 返回 target_jetty，失败 NULL。 */
urma_target_jetty_t *import_self(ctx_t *c);

/* import + bind 对端 jetty（RC/RM 模式，跨进程）。
 * 先 import_jetty 再 bind_jetty 建链。
 * 返回 target_jetty，失败 NULL。 */
urma_target_jetty_t *bind_remote(ctx_t *c, urma_eid_t rmt_eid,
    uint32_t rmt_uasid, uint32_t rmt_jetty_id);

/* import 对端 jetty（不 bind，UM 模式跨进程）。
 * 返回 target_jetty，失败 NULL。 */
urma_target_jetty_t *import_jetty_remote(ctx_t *c, urma_eid_t rmt_eid,
    uint32_t rmt_uasid, uint32_t rmt_jetty_id);

/* import 对端 seg（跨进程，用对端 eid+uasid+token_id+va+len）。
 * 返回 target_seg，失败 NULL。 */
urma_target_seg_t *import_seg_remote(ctx_t *c, urma_eid_t rmt_eid,
    uint32_t rmt_uasid, uint32_t rmt_token_id, uint64_t rmt_va, uint64_t rmt_len);

/* post recv WQE（SEND 接收端用）。recv_buf+len+user_ctx。
 * 返回 0 成功。 */
int post_recv(ctx_t *c, void *recv_buf, size_t len, uint64_t user_ctx);

/* post SEND（双边，需对端已 post recv）。
 * data_buf+len+user_ctx+tj（对端 jetty）。
 * 返回 0 成功。 */
int post_send(ctx_t *c, urma_target_jetty_t *tj, void *data_buf, size_t len, uint64_t user_ctx);

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

/* 获取 jetty_id（给 OOB 传给对端用） */
uint32_t get_jetty_id(ctx_t *c);

/* 逆序释放全部（jetty/jfr/jfs/jfc/jfce/ctx/devs）。c 本身 free。安全传 NULL。 */
void teardown(ctx_t *c);

#ifdef __cplusplus
}
#endif

#endif /* BL_FIXTURE_H */
