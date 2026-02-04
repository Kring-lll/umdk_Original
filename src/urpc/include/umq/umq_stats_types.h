/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Public header file of UMQ function types
 * Create: 2026-2-4
 * Note:
 * History: 2026-2-4
 */

#ifndef UMQ_STATS_TYPES_H
#define UMQ_STATS_TYPES_H
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>

#include "umq_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UMQ_STATS_QBUF_POOL_TYPE_MAX 10

typedef struct umq_qbuf_pool_stats {
    umq_buf_mode_t mode;                      // split or combine
    uint64_t total_size;                      // qbuf pool total size
    uint64_t total_block_num;
    uint32_t block_size;
    uint32_t headroom_size;
    uint32_t data_size;                       // combine: block_size - umq_buf_t_size, split: block_size
    uint32_t buf_size;                        // combine: block_size, split: block_size + umq_buf_t_size
    uint32_t umq_buf_t_size;                  // size of umq_buf_t
    union {
        struct {
            uint64_t block_num_with_data;     // number of available buf in data area
            uint64_t size_with_data;          // available buf size in data area
            uint64_t block_num_without_data;  // number of available buf in non-data area
            uint64_t size_without_data;       // available buf size in non-data area
        } split;
        struct {
            uint64_t block_num_with_data;
            uint64_t size_with_data;
        } combine;
    } available_mem;
} umq_qbuf_pool_stats_t;

typedef struct umq_qbuf_pool_stats_list {
    umq_qbuf_pool_stats_t qbuf_pool_stats[UMQ_STATS_QBUF_POOL_TYPE_MAX];
    uint32_t num;
} umq_qbuf_pool_stats_list_t;

typedef struct umq_info_stats {
    umq_trans_mode_t trans_mode;
    union {
        struct {
            uint32_t umq_id;
            umq_eid_info_t eid;
            char dev_name[UMQ_DEV_NAME_SIZE];
            uint32_t local_io_jetty_id;
            uint32_t local_fc_jetty_id;
            uint32_t remote_io_jetty_id;
            uint32_t remote_fc_jetty_id;
        } ub;
    };
} umq_info_stats_t;

#define UMQ_PERF_QUANTILE_MAX_NUM (8u)

typedef struct umq_perf_stats {
    struct {
        uint64_t accumulation;
        uint64_t min;
        uint64_t max;
        uint64_t cnt;
        uint64_t bucket[UMQ_PERF_QUANTILE_MAX_NUM + 1];
    } type_record[UMQ_PERF_RECORD_TYPE_MAX];
    bool is_used;
} umq_perf_stats_t;

typedef struct umq_perf_stats_cfg {
    uint64_t thresh_array[UMQ_PERF_QUANTILE_MAX_NUM];
    uint32_t thresh_num;
} umq_perf_stats_cfg_t;

#ifdef __cplusplus
}
#endif

#endif