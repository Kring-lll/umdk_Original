/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Public header file of UMQ function
 * Create: 2026-2-4
 * Note:
 * History: 2026-2-4
 */

#include "umq_errno.h"
#include "umq_tp_stats_api.h"

static int umq_tp_ub_plus_fc_stats_get(uint64_t umqh_tp, umq_flow_control_stats_t *fc_stats)
{
    return UMQ_SUCCESS;
}

int umq_tp_ub_plus_qbuf_pool_stats_get(uint64_t umqh_tp, umq_qbuf_pool_stats_t *qbuf_pool_stats)
{
    return UMQ_SUCCESS;
}

int umq_tp_ub_plus_info_stats_get(uint64_t umqh_tp, umq_info_stats_t *umq_info_stats)
{
    return UMQ_SUCCESS;
}

int umq_tp_ub_plus_io_stats_get(uint64_t umqh_tp, umq_packet_state_t *packet_state)
{
    return UMQ_SUCCESS;
}

int umq_tp_ub_plus_io_stats_reset(uint64_t umqh_tp)
{
    return UMQ_SUCCESS;
}

static umq_stats_ops_t g_umq_ub_plus_stats_ops = {
    .mode = UMQ_TRANS_MODE_UB,
    .umq_tp_fc_stats_get = umq_tp_ub_plus_fc_stats_get,
    .umq_tp_qbuf_pool_stats_get = umq_tp_ub_plus_qbuf_pool_stats_get,
    .umq_tp_info_stats_get = umq_tp_ub_plus_info_stats_get,
    .umq_tp_io_stats_get = umq_tp_ub_plus_io_stats_get,
    .umq_tp_io_stats_reset = umq_tp_ub_plus_io_stats_reset,
};

umq_stats_ops_t *umq_ub_plus_stats_ops_get(void)
{
    return &g_umq_ub_plus_stats_ops;
}