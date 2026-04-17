/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2021-2025. All rights reserved.
 * Description: Performance monitoring and profiling for URMA
 * Author: Tang Zhedong
 * Create: 2026-04-08
 * Note:
 * History: 2026-04-08 create file
 */

#ifndef URMA_PERF_H
#define URMA_PERF_H

#include <stdint.h>
#include <stdbool.h>
#include "urma_opcode.h"

#ifdef __cplusplus
extern "C" {
#endif

#define URMA_PERF_BUCKET_MAX_NUM     (32u)
#define URMA_PERF_THREAD_MAX_NUM          (64u)
#define URMA_PERF_MAX_THRESH_NS        (100000u)

typedef enum urma_perf_type {
    UB_JETTY_POST_SEND,
    BOND_JETTY_POST_SEND,
    UB_JFS_POST_SEND,
    BOND_JFS_POST_SEND,
    UB_JETTY_POST_RECV,
    BOND_JETTY_POST_RECV,
    UB_POST_JFR_RECV,
    BOND_POST_JFR_RECV,
    UB_POLL_JFC,
    BOND_POLL_JFC,
    UB_WAIT_JFC,
    BOND_WAIT_JFC,
    UB_ACK_JFC,
    UB_REARM_JFC,
    BOND_REARM_JFC,
    URMA_PERF_RECORD_TYPE_MAX,
} urma_perf_record_type_t;

typedef struct urma_perf_attr {
    uint32_t thresh_num;
    uint64_t thresh_array[URMA_PERF_BUCKET_MAX_NUM];
} urma_perf_attr_t;

typedef struct urma_perf_stats {
    uint64_t retry_count;
    struct {
        urma_perf_record_type_t type;
        uint64_t sample_num;
        uint64_t average;
        uint64_t mininum;
        uint64_t maxinum;
        uint64_t p50;
        uint64_t p90;
        uint64_t p99;
    } type_record[URMA_PERF_RECORD_TYPE_MAX];
} urma_perf_stats_t;

// for internal 
#define PERF_PROFILING_START(type) \
    uint64_t __perf_start_##type = 0; \
    do { \
        if (urma_perf_is_enabled()) { \
            __perf_start_##type = urma_get_perf_timestamp(); \
        } \
    } while (0)

#define PERF_PROFILING_END(type) \
    do { \
        if (urma_perf_is_enabled()) { \
            uint64_t _perf_end = urma_get_perf_timestamp(); \
            urma_step_perf(type, _perf_end - __perf_start_##type); \
        } \
    } while (0)

bool urma_perf_is_enabled();

uint64_t urma_get_perf_timestamp(void);

void urma_step_perf(urma_perf_record_type_t type, uint64_t latency);

// for external
/**
 * Start performance monitoring for urma devices.
 * Return: 0 on success, other value on error
 */
urma_status_t urma_start_perf(void);

/**
 * Stop performance monitoring for urma devices.
 * Return: 0 on success, other value on error
 */
urma_status_t urma_stop_perf(void);

/**
 * Configure performance monitoring attributes.
 * @param[in] perf_attr: Pointer to performance attributes structure;
 * Return: 0 on success, other value on error
 */
urma_status_t urma_config_perf_attr(urma_perf_attr_t *perf_attr);

/**
 * Get performance statistics information.
 * @param[in] perf_buf: Buffer to store performance information, user needs to allocate the memory;
 * @param[in] length: Pointer to buffer length, input as buffer size, output as actual data length;
 * Return: 0 on success, other value on error
 */
urma_status_t urma_get_perf_info(char *perf_buf, uint32_t *length);

/**
 * Get performance statistics information as string.
 * Return: 0 on success, other value on error
 */
urma_status_t urma_get_perf_info_to_string(void);


#ifdef __cplusplus
}
#endif

#endif
