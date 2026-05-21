/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: FC SGE pool management for shared Jetty
 * Create: 2026-05-19
 */

#ifndef UMQ_UB_FC_SGE_POOL_H
#define UMQ_UB_FC_SGE_POOL_H

#include <stdint.h>
#include <stdbool.h>
#include "urpc_bitmap.h"
#include "util_lock.h"
#include "umq_errno.h"
#include "umq_qbuf_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UMQ_UB_FC_SGE_POOL_MAX_SLOTS (64 * 1024)
#define UMQ_UB_FC_SGE_SLOT_SIZE 8
#define UMQ_UB_FC_SGE_MSG_SIZE 4
#define UMQ_UB_FC_SGE_QBUF_BLOCK_SIZE 4096
#define UMQ_UB_FC_SGE_SLOTS_PER_QBUF (UMQ_UB_FC_SGE_QBUF_BLOCK_SIZE / UMQ_UB_FC_SGE_SLOT_SIZE)
#define UMQ_UB_FC_SGE_QBUF_ARRAY_SIZE (UMQ_UB_FC_SGE_POOL_MAX_SLOTS / UMQ_UB_FC_SGE_SLOTS_PER_QBUF)

typedef struct umq_ub_fc_sge_pool {
    urpc_bitmap_t bitmap;
    umq_buf_t **qbuf_array;
    uint32_t qbuf_count;
    util_external_mutex_lock *lock;
} umq_ub_fc_sge_pool_t;

typedef struct umq_ub_fc_sge_slot {
    void *addr;
    uint32_t len;
    uint32_t bitmap_pos;
} umq_ub_fc_sge_slot_t;

typedef struct umq_ub_fc_sge_ctx {
    umq_ub_fc_sge_slot_t tx_slot;
    umq_ub_fc_sge_slot_t rx_slots[2];
    uint32_t rx_slot_count;
} umq_ub_fc_sge_ctx_t;

umq_ub_fc_sge_pool_t *umq_ub_fc_sge_pool_create(void);
void umq_ub_fc_sge_pool_destroy(umq_ub_fc_sge_pool_t *pool);
int umq_ub_fc_sge_alloc(umq_ub_fc_sge_pool_t *pool, umq_ub_fc_sge_slot_t *slot);
void umq_ub_fc_sge_free(umq_ub_fc_sge_pool_t *pool, umq_ub_fc_sge_slot_t *slot);
int umq_ub_fc_sge_ctx_init(umq_ub_fc_sge_pool_t *pool, umq_ub_fc_sge_ctx_t *ctx, bool need_rx);
void umq_ub_fc_sge_ctx_uninit(umq_ub_fc_sge_pool_t *pool, umq_ub_fc_sge_ctx_t *ctx);

int umq_ub_fc_shared_jfr_rx_sge_init(umq_ub_fc_sge_pool_t *pool, uint32_t depth,
    umq_buf_t ***qbufs, uint32_t *qbuf_count, void ***sge_addrs, uint32_t *sge_count);
void umq_ub_fc_shared_jfr_rx_sge_uninit(umq_buf_t **qbufs, uint32_t qbuf_count,
    void **sge_addrs, uint32_t sge_count);

#ifdef __cplusplus
}
#endif

#endif
