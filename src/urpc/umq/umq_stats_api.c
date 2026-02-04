/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: realize func for umq api
 * Create: 2026-2-4
 */

#include "umq_errno.h"
#include "umq_stats_api.h"

 // fc stats
int umq_fc_stats_get(uint64_t umqh, umq_flow_control_stats_t *fc_stats)
{
    return UMQ_SUCCESS;
}

// qbuf pool stats
int umq_qbuf_pool_stats_get(uint64_t umqh, umq_qbuf_pool_stats_t *qbuf_pool_stats)
{
    return UMQ_SUCCESS;
}

int umq_info_stats_get(uint64_t umqh, umq_info_stats_t *stats)
{
    return UMQ_SUCCESS;
}

// io stats
int umq_io_stats_get(uint64_t umqh, umq_packet_state_t *packet_state)
{
    return UMQ_SUCCESS;
}

int umq_io_stats_reset(uint64_t umqh)
{
    return UMQ_SUCCESS;
}

// perf stats
int umq_perf_stats_get(umq_perf_stats_t *umq_perf_stats)
{
    return UMQ_SUCCESS;
}

int umq_perf_stats_reset(umq_perf_stats_cfg_t *perf_stats_cfg)
{
    return UMQ_SUCCESS;
}