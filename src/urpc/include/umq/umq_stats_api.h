/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Public header file of UMQ function
 * Create: 2026-2-4
 * Note:
 * History: 2026-2-4
 */

#ifndef UMQ_STATS_API_H
#define UMQ_STATS_API_H

#include "umq_stats_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Thread safety function
 * Get flow control statistical results.
 * @param[in] umqh: umq handle
 * @param[out] fc_stats: flow control statistical results
 * Return 0 on success, error code on failure
 */
int umq_stats_fc_get(uint64_t umqh, umq_flow_control_stats_t *fc_stats);

/**
 * Thread safety function
 * Get qbuf pool statistical results.
 * @param[in] umqh: umq handle
 * @param[out] qbuf_pool_stats: qbuf pool statistical results
 * Return 0 on success, error code on failure
 */
int umq_stats_qbuf_pool_get(uint64_t umqh, umq_qbuf_pool_stats_list_t *qbuf_pool_stats);

/**
 * Thread safety function
 * Get umq info statistical results.
 * @param[in] umqh: umq handle
 * @param[out] umq_info_stats: umq info statistical results
 * Return 0 on success, error code on failure
 */
int umq_stats_info_get(uint64_t umqh, umq_info_stats_t *umq_info_stats);

/**
 * Thread safety function
 * Get io packet statistical results.
 * @param[in] umqh: umq handle
 * @param[out] packet_state: io packet statistical results
 * Return 0 on success, error code on failure
 */
int umq_stats_io_get(uint64_t umqh, umq_packet_state_t *packet_state);

/**
 * Reset the I/O statistical counters.
 * @param[in] umqh: umq handle
 * Return 0 on success, error code on failure
 */
int umq_stats_io_reset(uint64_t umqh);

/**
 * Thread safety function
 * Get io packet statistical results.
 * @param[out] umq_perf_stats: perf statistical results
 * Return 0 on success, error code on failure
 */
int umq_stats_perf_get(umq_perf_stats_t *umq_perf_stats);

/**
 * Reset the perf statistical counters and config.
 * @param[out] perf_stats_cfg: perf statistical results
 * Return 0 on success, error code on failure
 */
int umq_stats_perf_reset(umq_perf_stats_cfg_t *perf_stats_cfg);

#ifdef __cplusplus
}
#endif

#endif