/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: UMQ UB flow control SGE memory management
 * Create: 2026-5-28
 * Note:
 * History: 2026-5-28
 */

#include "umq_ub_private.h"
#include "umq_vlog.h"
#include "umq_qbuf_pool.h"
#include "urpc_bitmap.h"
#include "util_lock.h"

int umq_ub_flow_control_sge_mgr_init(umq_ub_flow_control_sge_mgr_t *mgr)
{
    mgr->bitmap = urpc_bitmap_alloc(UMQ_UB_FLOW_CONTROL_SGE_BITMAP_SIZE);
    if (mgr->bitmap == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "flow control sge bitmap alloc failed\n");
        return -UMQ_ERR_ENOMEM;
    }

    mgr->lock = util_mutex_lock_create(UTIL_MUTEX_ATTR_EXCLUSIVE);
    if (mgr->lock == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "flow control sge lock create failed\n");
        urpc_bitmap_free(mgr->bitmap);
        mgr->bitmap = NULL;
        return -UMQ_ERR_ENOMEM;
    }

    for (uint32_t i = 0; i < UMQ_UB_FLOW_CONTROL_SGE_QBUF_COUNT; i++) {
        mgr->qbuf_array[i] = NULL;
    }

    return UMQ_SUCCESS;
}

void umq_ub_flow_control_sge_mgr_uninit(umq_ub_flow_control_sge_mgr_t *mgr)
{
    for (uint32_t i = 0; i < UMQ_UB_FLOW_CONTROL_SGE_QBUF_COUNT; i++) {
        if (mgr->qbuf_array[i] != NULL) {
            umq_buf_free(mgr->qbuf_array[i]);
            mgr->qbuf_array[i] = NULL;
        }
    }

    if (mgr->bitmap != NULL) {
        urpc_bitmap_free(mgr->bitmap);
        mgr->bitmap = NULL;
    }

    if (mgr->lock != NULL) {
        (void)util_mutex_lock_destroy(mgr->lock);
        mgr->lock = NULL;
    }
}

static bool umq_ub_flow_control_sge_qbuf_slots_all_free(umq_ub_flow_control_sge_mgr_t *mgr, uint32_t qbuf_idx)
{
    uint32_t base_bit = qbuf_idx * UMQ_UB_FLOW_CONTROL_SGE_SLOTS_PER_QBUF;
    for (uint32_t i = 0; i < UMQ_UB_FLOW_CONTROL_SGE_SLOTS_PER_QBUF; i++) {
        if (urpc_bitmap_is_set(mgr->bitmap, base_bit + i)) {
            return false;
        }
    }
    return true;
}

int umq_ub_flow_control_sge_alloc(umq_ub_flow_control_sge_mgr_t *mgr, umq_ub_flow_control_sge_slot_t *slot)
{
    (void)util_mutex_lock(mgr->lock);

    unsigned long bit = urpc_bitmap_find_next_zero_bit(mgr->bitmap, UMQ_UB_FLOW_CONTROL_SGE_BITMAP_SIZE, 0);
    if (bit >= UMQ_UB_FLOW_CONTROL_SGE_BITMAP_SIZE) {
        (void)util_mutex_unlock(mgr->lock);
        UMQ_VLOG_ERR(VLOG_UMQ, "flow control sge bitmap full, no free slot\n");
        return -ENOSPC;
    }

    uint32_t qbuf_idx = (uint32_t)bit / UMQ_UB_FLOW_CONTROL_SGE_SLOTS_PER_QBUF;
    uint32_t qbuf_offset = (uint32_t)bit % UMQ_UB_FLOW_CONTROL_SGE_SLOTS_PER_QBUF;

    if (mgr->qbuf_array[qbuf_idx] == NULL) {
        mgr->qbuf_array[qbuf_idx] = umq_buf_alloc(UMQ_UB_FLOW_CONTROL_SGE_QBUF_SIZE, 1, UMQ_INVALID_HANDLE, NULL);
        if (mgr->qbuf_array[qbuf_idx] == NULL) {
            (void)util_mutex_unlock(mgr->lock);
            UMQ_VLOG_ERR(VLOG_UMQ, "flow control sge qbuf alloc failed\n");
            return -UMQ_ERR_ENOMEM;
        }
    }

    urpc_bitmap_set1(mgr->bitmap, bit);

    (void)util_mutex_unlock(mgr->lock);

    slot->addr = (char *)mgr->qbuf_array[qbuf_idx]->buf_data + qbuf_offset * UMQ_UB_FLOW_CONTROL_SGE_BYTES_PER_SLOT;
    slot->bitmap_idx = (uint32_t)bit;

    return UMQ_SUCCESS;
}

void umq_ub_flow_control_sge_free(umq_ub_flow_control_sge_mgr_t *mgr, umq_ub_flow_control_sge_slot_t *slot)
{
    if (slot->addr == NULL || slot->bitmap_idx >= UMQ_UB_FLOW_CONTROL_SGE_BITMAP_SIZE) {
        return;
    }

    (void)util_mutex_lock(mgr->lock);

    urpc_bitmap_set0(mgr->bitmap, slot->bitmap_idx);

    uint32_t qbuf_idx = slot->bitmap_idx / UMQ_UB_FLOW_CONTROL_SGE_SLOTS_PER_QBUF;
    if (mgr->qbuf_array[qbuf_idx] != NULL && umq_ub_flow_control_sge_qbuf_slots_all_free(mgr, qbuf_idx)) {
        umq_buf_free(mgr->qbuf_array[qbuf_idx]);
        mgr->qbuf_array[qbuf_idx] = NULL;
    }

    (void)util_mutex_unlock(mgr->lock);

    slot->addr = NULL;
    slot->bitmap_idx = UMQ_UB_FLOW_CONTROL_SGE_BITMAP_SIZE;
}

int umq_ub_flow_control_share_recv_init(umq_ub_flow_control_share_recv_t *recv, uint32_t qbuf_cnt)
{
    for (uint32_t i = 0; i < qbuf_cnt; i++) {
        recv->qbuf_array[i] = umq_buf_alloc(UMQ_UB_FLOW_CONTROL_SGE_QBUF_SIZE, 1, UMQ_INVALID_HANDLE, NULL);
        if (recv->qbuf_array[i] == NULL) {
            UMQ_VLOG_ERR(VLOG_UMQ, "flow control share recv qbuf[%u] alloc failed\n", i);
            for (uint32_t j = 0; j < i; j++) {
                umq_buf_free(recv->qbuf_array[j]);
                recv->qbuf_array[j] = NULL;
            }
            recv->qbuf_cnt = 0;
            return -UMQ_ERR_ENOMEM;
        }
    }

    recv->qbuf_cnt = qbuf_cnt;
    return UMQ_SUCCESS;
}

void umq_ub_flow_control_share_recv_uninit(umq_ub_flow_control_share_recv_t *recv)
{
    for (uint32_t i = 0; i < recv->qbuf_cnt; i++) {
        if (recv->qbuf_array[i] != NULL) {
            umq_buf_free(recv->qbuf_array[i]);
            recv->qbuf_array[i] = NULL;
        }
    }
    recv->qbuf_cnt = 0;
}
