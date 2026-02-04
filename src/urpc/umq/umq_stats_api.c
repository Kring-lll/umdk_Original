/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: realize func for umq api
 * Create: 2026-2-4
 */

#include "umq_errno.h"
#include "umq_stats_api.h"

int umq_stats_fc_get(uint64_t umqh, umq_flow_control_stats_t *fc_stats)
{
    return UMQ_SUCCESS;
}

int umq_stats_qbuf_pool_get(uint64_t umqh, umq_qbuf_pool_stats_t *qbuf_pool_stats)
{
    return UMQ_SUCCESS;
}

int umq_stats_info_get(uint64_t umqh, umq_info_stats_t *stats)
{
    return UMQ_SUCCESS;
}

int umq_stats_io_get(uint64_t umqh, umq_packet_state_t *packet_state)
{
    return UMQ_SUCCESS;
}

int umq_stats_io_reset(uint64_t umqh)
{
    return UMQ_SUCCESS;
}

int umq_stats_perf_get(umq_perf_stats_t *umq_perf_stats)
{
    return UMQ_SUCCESS;
}

int umq_stats_perf_reset(umq_perf_stats_cfg_t *perf_stats_cfg)
{
    return UMQ_SUCCESS;
}