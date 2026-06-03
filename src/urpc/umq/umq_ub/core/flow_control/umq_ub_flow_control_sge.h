/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: UMQ UB flow control SGE memory management
 * Create: 2026-5-28
 * Note:
 * History: 2026-5-28
 */

#ifndef UMQ_UB_FLOW_CONTROL_SGE_H
#define UMQ_UB_FLOW_CONTROL_SGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Flow control SGE macros */
#define UMQ_UB_FLOW_CONTROL_SGE_BITMAP_SIZE (64 * 1024)
#define UMQ_UB_FLOW_CONTROL_SGE_BYTES_PER_SLOT 4
#define UMQ_UB_FLOW_CONTROL_SGE_QBUF_SIZE (4096)
#define UMQ_UB_FLOW_CONTROL_SGE_QBUF_COUNT (UMQ_UB_FLOW_CONTROL_SGE_BITMAP_SIZE *       \
                                            UMQ_UB_FLOW_CONTROL_SGE_BYTES_PER_SLOT /     \
                                            UMQ_UB_FLOW_CONTROL_SGE_QBUF_SIZE)
#define UMQ_UB_FLOW_CONTROL_SGE_SLOTS_PER_QBUF (UMQ_UB_FLOW_CONTROL_SGE_QBUF_SIZE /      \
                                                UMQ_UB_FLOW_CONTROL_SGE_BYTES_PER_SLOT)
#define UMQ_UB_FLOW_CONTROL_SGE_SHARED_RECV_QBUF_MAX 32
#define UMQ_UB_FLOW_CONTROL_SGE_SHARED_RECV_BYTES_PER_SLOT 4
#define UMQ_UB_FLOW_CONTROL_SGE_SHARED_RECV_SLOTS_PER_QBUF (UMQ_UB_FLOW_CONTROL_SGE_QBUF_SIZE /        \
                                                            UMQ_UB_FLOW_CONTROL_SGE_SHARED_RECV_BYTES_PER_SLOT)

/* Forward declarations for types defined in umq_ub_private.h */
typedef struct umq_ub_flow_control_sge_slot umq_ub_flow_control_sge_slot_t;
typedef struct umq_ub_flow_control_sge_mgr umq_ub_flow_control_sge_mgr_t;
typedef struct umq_ub_flow_control_share_recv umq_ub_flow_control_share_recv_t;

/* Flow control SGE memory management APIs */
int umq_ub_flow_control_sge_mgr_init(umq_ub_flow_control_sge_mgr_t *mgr);
void umq_ub_flow_control_sge_mgr_uninit(umq_ub_flow_control_sge_mgr_t *mgr);
int umq_ub_flow_control_sge_alloc(umq_ub_flow_control_sge_mgr_t *mgr, umq_ub_flow_control_sge_slot_t *slot);
void umq_ub_flow_control_sge_free(umq_ub_flow_control_sge_mgr_t *mgr, umq_ub_flow_control_sge_slot_t *slot);
int umq_ub_flow_control_share_recv_init(umq_ub_flow_control_share_recv_t *recv, uint32_t qbuf_cnt);
void umq_ub_flow_control_share_recv_uninit(umq_ub_flow_control_share_recv_t *recv);

#ifdef __cplusplus
}
#endif

#endif /* UMQ_UB_FLOW_CONTROL_SGE_H */
