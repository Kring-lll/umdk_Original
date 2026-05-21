/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: FC SGE pool management for shared Jetty
 * Create: 2026-05-19
 */

#include "umq_ub_fc_sge_pool.h"
#include "umq_api.h"
#include "umq_vlog.h"
#include <stdlib.h>
#include <string.h>

umq_ub_fc_sge_pool_t *umq_ub_fc_sge_pool_create(void)
{
    umq_ub_fc_sge_pool_t *pool = (umq_ub_fc_sge_pool_t *)calloc(1, sizeof(umq_ub_fc_sge_pool_t));
    if (pool == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "calloc fc sge pool failed\n");
        return NULL;
    }

    pool->bitmap = urpc_bitmap_alloc(UMQ_UB_FC_SGE_POOL_MAX_SLOTS);
    if (pool->bitmap == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "alloc fc sge pool bitmap failed\n");
        goto FREE_POOL;
    }

    pool->qbuf_array = (umq_buf_t **)calloc(UMQ_UB_FC_SGE_QBUF_ARRAY_SIZE, sizeof(umq_buf_t *));
    if (pool->qbuf_array == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "calloc fc sge qbuf array failed\n");
        goto FREE_BITMAP;
    }
    pool->qbuf_count = 0;

    pool->lock = util_mutex_lock_create(0);
    if (pool->lock == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "create fc sge pool lock failed\n");
        goto FREE_QBUF_ARRAY;
    }

    return pool;

FREE_QBUF_ARRAY:
    free(pool->qbuf_array);
FREE_BITMAP:
    urpc_bitmap_free(pool->bitmap);
FREE_POOL:
    free(pool);
    return NULL;
}

void umq_ub_fc_sge_pool_destroy(umq_ub_fc_sge_pool_t *pool)
{
    if (pool == NULL) {
        return;
    }

    for (uint32_t i = 0; i < UMQ_UB_FC_SGE_QBUF_ARRAY_SIZE; i++) {
        if (pool->qbuf_array[i] != NULL) {
            umq_buf_free(pool->qbuf_array[i]);
            pool->qbuf_array[i] = NULL;
        }
    }

    if (pool->qbuf_array != NULL) {
        free(pool->qbuf_array);
    }
    if (pool->bitmap != NULL) {
        urpc_bitmap_free(pool->bitmap);
    }
    if (pool->lock != NULL) {
        (void)util_mutex_lock_destroy(pool->lock);
    }
    free(pool);
}

static int umq_ub_fc_sge_qbuf_ensure(umq_ub_fc_sge_pool_t *pool, uint32_t qbuf_idx)
{
    if (qbuf_idx >= UMQ_UB_FC_SGE_QBUF_ARRAY_SIZE) {
        return -UMQ_ERR_EINVAL;
    }

    if (pool->qbuf_array[qbuf_idx] != NULL) {
        return UMQ_SUCCESS;
    }

    umq_buf_t *qbuf = umq_buf_alloc(UMQ_UB_FC_SGE_QBUF_BLOCK_SIZE, 1, UMQ_INVALID_HANDLE, NULL);
    if (qbuf == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "alloc fc sge qbuf at index %u failed\n", qbuf_idx);
        return -UMQ_ERR_ENOMEM;
    }

    pool->qbuf_array[qbuf_idx] = qbuf;
    pool->qbuf_count++;
    return UMQ_SUCCESS;
}

int umq_ub_fc_sge_alloc(umq_ub_fc_sge_pool_t *pool, umq_ub_fc_sge_slot_t *slot)
{
    if (pool == NULL || slot == NULL) {
        return -UMQ_ERR_EINVAL;
    }

    (void)util_mutex_lock(pool->lock);

    unsigned long pos = urpc_bitmap_find_next_zero_bit(pool->bitmap, UMQ_UB_FC_SGE_POOL_MAX_SLOTS, 0);
    if (pos >= UMQ_UB_FC_SGE_POOL_MAX_SLOTS) {
        (void)util_mutex_unlock(pool->lock);
        UMQ_VLOG_ERR(VLOG_UMQ, "fc sge pool bitmap is full\n");
        return -UMQ_ERR_ENOMEM;
    }

    uint32_t qbuf_idx = (uint32_t)(pos * UMQ_UB_FC_SGE_SLOT_SIZE) / UMQ_UB_FC_SGE_QBUF_BLOCK_SIZE;
    int ret = umq_ub_fc_sge_qbuf_ensure(pool, qbuf_idx);
    if (ret != UMQ_SUCCESS) {
        (void)util_mutex_unlock(pool->lock);
        return ret;
    }

    urpc_bitmap_set1(pool->bitmap, pos);

    uint32_t offset = (uint32_t)(pos * UMQ_UB_FC_SGE_SLOT_SIZE) % UMQ_UB_FC_SGE_QBUF_BLOCK_SIZE;
    umq_buf_t *qbuf = pool->qbuf_array[qbuf_idx];
    slot->addr = (void *)((uintptr_t)qbuf->buf_data + offset);
    slot->len = UMQ_UB_FC_SGE_SLOT_SIZE;
    slot->bitmap_pos = (uint32_t)pos;

    (void)util_mutex_unlock(pool->lock);
    return UMQ_SUCCESS;
}

void umq_ub_fc_sge_free(umq_ub_fc_sge_pool_t *pool, umq_ub_fc_sge_slot_t *slot)
{
    if (pool == NULL || slot == NULL || slot->bitmap_pos >= UMQ_UB_FC_SGE_POOL_MAX_SLOTS) {
        return;
    }

    (void)util_mutex_lock(pool->lock);
    urpc_bitmap_set0(pool->bitmap, slot->bitmap_pos);

    uint32_t qbuf_idx = (uint32_t)(slot->bitmap_pos * UMQ_UB_FC_SGE_SLOT_SIZE) / UMQ_UB_FC_SGE_QBUF_BLOCK_SIZE;
    bool qbuf_empty = true;
    uint32_t start_pos = qbuf_idx * UMQ_UB_FC_SGE_SLOTS_PER_QBUF;
    for (uint32_t i = 0; i < UMQ_UB_FC_SGE_SLOTS_PER_QBUF; i++) {
        if (urpc_bitmap_is_set(pool->bitmap, start_pos + i)) {
            qbuf_empty = false;
            break;
        }
    }

    if (qbuf_empty && pool->qbuf_array[qbuf_idx] != NULL) {
        umq_buf_free(pool->qbuf_array[qbuf_idx]);
        pool->qbuf_array[qbuf_idx] = NULL;
        pool->qbuf_count--;
    }

    slot->addr = NULL;
    slot->len = 0;
    slot->bitmap_pos = UINT32_MAX;

    (void)util_mutex_unlock(pool->lock);
}

int umq_ub_fc_sge_ctx_init(umq_ub_fc_sge_pool_t *pool, umq_ub_fc_sge_ctx_t *ctx, bool need_rx)
{
    if (pool == NULL || ctx == NULL) {
        return -UMQ_ERR_EINVAL;
    }

    memset(ctx, 0, sizeof(umq_ub_fc_sge_ctx_t));

    int ret = umq_ub_fc_sge_alloc(pool, &ctx->tx_slot);
    if (ret != UMQ_SUCCESS) {
        UMQ_VLOG_ERR(VLOG_UMQ, "alloc fc sge tx slot failed\n");
        return ret;
    }

    if (need_rx) {
        ctx->rx_slot_count = 2;
        for (uint32_t i = 0; i < ctx->rx_slot_count; i++) {
            ret = umq_ub_fc_sge_alloc(pool, &ctx->rx_slots[i]);
            if (ret != UMQ_SUCCESS) {
                UMQ_VLOG_ERR(VLOG_UMQ, "alloc fc sge rx slot %u failed\n", i);
                goto FREE_SLOTS;
            }
        }
    }

    return UMQ_SUCCESS;

FREE_SLOTS:
    for (uint32_t i = 0; i < ctx->rx_slot_count; i++) {
        if (ctx->rx_slots[i].addr != NULL) {
            umq_ub_fc_sge_free(pool, &ctx->rx_slots[i]);
        }
    }
    umq_ub_fc_sge_free(pool, &ctx->tx_slot);
    return ret;
}

void umq_ub_fc_sge_ctx_uninit(umq_ub_fc_sge_pool_t *pool, umq_ub_fc_sge_ctx_t *ctx)
{
    if (pool == NULL || ctx == NULL) {
        return;
    }

    for (uint32_t i = 0; i < ctx->rx_slot_count; i++) {
        if (ctx->rx_slots[i].addr != NULL) {
            umq_ub_fc_sge_free(pool, &ctx->rx_slots[i]);
        }
    }
    ctx->rx_slot_count = 0;

    if (ctx->tx_slot.addr != NULL) {
        umq_ub_fc_sge_free(pool, &ctx->tx_slot);
    }
}

int umq_ub_fc_shared_jfr_rx_sge_init(umq_ub_fc_sge_pool_t *pool, uint32_t depth,
    umq_buf_t ***qbufs, uint32_t *qbuf_count, void ***sge_addrs, uint32_t *sge_count)
{
    if (pool == NULL || depth == 0) {
        return -UMQ_ERR_EINVAL;
    }

    uint32_t num_qbufs = (depth * UMQ_UB_FC_SGE_MSG_SIZE + UMQ_UB_FC_SGE_QBUF_BLOCK_SIZE - 1) /
        UMQ_UB_FC_SGE_QBUF_BLOCK_SIZE;
    umq_buf_t **bufs = (umq_buf_t **)calloc(num_qbufs, sizeof(umq_buf_t *));
    if (bufs == NULL) {
        return -UMQ_ERR_ENOMEM;
    }

    void **addrs = (void **)calloc(depth, sizeof(void *));
    if (addrs == NULL) {
        free(bufs);
        return -UMQ_ERR_ENOMEM;
    }

    for (uint32_t i = 0; i < num_qbufs; i++) {
        bufs[i] = umq_buf_alloc(UMQ_UB_FC_SGE_QBUF_BLOCK_SIZE, 1, UMQ_INVALID_HANDLE, NULL);
        if (bufs[i] == NULL) {
            UMQ_VLOG_ERR(VLOG_UMQ, "alloc shared jfr rx sge qbuf %u failed\n", i);
            goto FREE_ALL;
        }
    }

    uint32_t addr_idx = 0;
    for (uint32_t i = 0; i < num_qbufs && addr_idx < depth; i++) {
        uint32_t slots_in_qbuf = UMQ_UB_FC_SGE_QBUF_BLOCK_SIZE / UMQ_UB_FC_SGE_MSG_SIZE;
        for (uint32_t j = 0; j < slots_in_qbuf && addr_idx < depth; j++) {
            addrs[addr_idx] = (void *)((uintptr_t)bufs[i]->buf_data + j * UMQ_UB_FC_SGE_MSG_SIZE);
            addr_idx++;
        }
    }

    *qbufs = bufs;
    *qbuf_count = num_qbufs;
    *sge_addrs = addrs;
    *sge_count = addr_idx;
    return UMQ_SUCCESS;

FREE_ALL:
    for (uint32_t i = 0; i < num_qbufs; i++) {
        if (bufs[i] != NULL) {
            umq_buf_free(bufs[i]);
        }
    }
    free(bufs);
    free(addrs);
    return -UMQ_ERR_ENOMEM;
}

void umq_ub_fc_shared_jfr_rx_sge_uninit(umq_buf_t **qbufs, uint32_t qbuf_count,
    void **sge_addrs, uint32_t sge_count)
{
    (void)sge_count;

    if (qbufs != NULL) {
        for (uint32_t i = 0; i < qbuf_count; i++) {
            if (qbufs[i] != NULL) {
                umq_buf_free(qbufs[i]);
            }
        }
        free(qbufs);
    }
    if (sge_addrs != NULL) {
        free(sge_addrs);
    }
}
