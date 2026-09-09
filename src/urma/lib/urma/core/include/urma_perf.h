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
#include "urma_opcode.h"

#ifdef __cplusplus
extern "C" {
#endif

#define URMA_PERF_THREAD_MAX_NUM        (128u)

#ifndef URMA_UBAGG_SQE_TOTAL_DIM
#define URMA_UBAGG_SQE_TOTAL_DIM 20
#endif

/* BOND_SQE_INFLIGHT_DIFF samples are single-digit counts that would be
 * dropped by the perf bucket granularity (bucket 0 covers [0,1) and
 * urma_step_perf skips delta==0), so they are scaled up by this factor
 * when recorded and divided back by the same factor when displayed.
 * Keep both sides consistent via this single macro.
 * NOTE: this metric is a count, not a duration. Under URMA_PERF_USE_CNTVCT
 * the dump path must NOT convert its raw values from CNTVCT ticks to ns
 * (urma_perf_value_to_ns() handles this); otherwise samples get inflated
 * by 1e9/CNTFRQ (x10 on a 100MHz counter) on top of this scale. */
#define URMA_PERF_INFLIGHT_DIFF_SCALE   10

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
    BOND_SEND_RTT,
    BOND_SEND_RESEND_DELAY,           /* delta from post_send to resend trigger */
    BOND_SEND_RTT_AFTER_RESEND,       /* delta from last resend to cr success (only for resent WRs) */
    BOND_SQE_INFLIGHT_DIFF,           /* max-min spread of per-port in-flight WR count at post_send */
    URMA_PERF_RECORD_TYPE_MAX,
} urma_perf_record_type_t;

typedef struct urma_perf_stats {
    uint64_t retry_count;
    /* Count of WRs completed (successful poll) during the time window since
     * the previous urma_get_perf_info() call. Snapshot+reset on each dump.
     * Recorded only while perf is enabled (urma_start_perf()). */
    uint64_t wr_complete_cnt;
    /* Per-path average WR RTT [local_idx][target_idx] over the time window
     * since the previous urma_get_perf_info() call. Read from the global
     * windowed RTT accumulators (snapshot+reset on each dump). */
    uint64_t rtt_avg[URMA_UBAGG_SQE_TOTAL_DIM][URMA_UBAGG_SQE_TOTAL_DIM];
    struct {
        urma_perf_record_type_t type;
        uint64_t sample_num;
        uint64_t average;
        uint64_t mininum;
        uint64_t maxinum;
        uint64_t p90;
        uint64_t p99;
        uint64_t p9999;
        uint64_t max_under_127ms;
    } type_record[URMA_PERF_RECORD_TYPE_MAX];
} urma_perf_stats_t;

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
 * Get performance statistics information.
 * @param[in] perf_buf: Buffer to store performance information, user needs to allocate the memory;
 * @param[in] length: Pointer to buffer length, input as buffer size, output as actual data length;
 * Return: 0 on success, other value on error
 */
urma_status_t urma_get_perf_info(char *perf_buf, uint32_t *length);

/**
 * Start path statistics monitoring (switch for the path perf interface):
 * enables recording of the per-path in-flight WR counts and the per-vjetty
 * in-flight jetty count reported by urma_get_path_info(), and resets the
 * windowed per-path RTT accumulators (the RTT window itself is reported via
 * urma_get_perf_info() and its accumulation is controlled by
 * urma_start_perf()/urma_stop_perf()). For accurate in-flight counts, call
 * this before posting traffic; toggling it while WRs are in flight skews
 * the counters.
 * Return: 0 on success, other value on error
 */
urma_status_t urma_start_path_perf(void);

/**
 * Stop path statistics monitoring. In-flight WR recording halts and the last
 * snapshot remains available via urma_get_path_info(). Per-path RTT
 * accumulation is not affected by this call; it follows
 * urma_start_perf()/urma_stop_perf().
 * Return: 0 on success, other value on error
 */
urma_status_t urma_stop_path_perf(void);

/**
 * Get global path statistics as a formatted string. Reports, at the instant of
 * the call: (1) current in-flight WR count per path [local_idx,target_idx],
 * and (2) the count of virtual jetties that currently have in-flight WRs.
 * @param[in] path_buf: Buffer to store the string, caller-allocated;
 * @param[in] length: Pointer to buffer length, input as size, output as actual;
 * Return: 0 on success, other value on error
 */
urma_status_t urma_get_path_info(char *path_buf, uint32_t *length);

#ifdef __cplusplus
}
#endif

#endif
