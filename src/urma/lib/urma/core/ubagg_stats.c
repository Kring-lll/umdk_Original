/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2021-2025. All rights reserved.
 * Description: ubagg statistics
 * Author: Zhang Dianhao
 * Create: 2026-04-05
 * Note:
 * History: 2026-04-05   Create File
 */

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "urma_private.h"

static atomic_uint g_ubagg_switch_cnt;

/* Cumulative SQE total per link [local_idx][target_idx].
 * Tracks every WR submitted by the bond datapath, independent of CR-driven
 * decrement of the in-flight sqe_cnt[][]. Monotonically non-decreasing. */
static atomic_ullong g_ubagg_sqe_total[URMA_UBAGG_SQE_TOTAL_DIM][URMA_UBAGG_SQE_TOTAL_DIM];

/* Global current in-flight WR count per path [local_idx][target_idx].
 * Mirrors the sum of all bondp_comp_t::sqe_cnt across comps. Reported via
 * urma_get_path_info(), so it belongs to the path perf interface and is only
 * tracked while path perf is enabled (urma_ubagg_path_is_enabled()). */
static atomic_uint g_ubagg_inflight[URMA_UBAGG_SQE_TOTAL_DIM][URMA_UBAGG_SQE_TOTAL_DIM];

/* Global count of virtual jetties (comps) that currently have >= 1 in-flight WR.
 * Maintained via 0<->non-zero transition of each comp's per-pjetty
 * inflight_per_pjetty[local_idx] counters. Each pjetty is tracked
 * independently so multiple active pjetties are all counted. Reported via
 * urma_get_path_info(), so it is only tracked while path perf is enabled. */
static atomic_uint g_ubagg_pjetty_inflight_cnt;

/* Windowed RTT accumulators per path [local_idx][target_idx], reset after each
 * urma_get_perf_info() dump so each call reports the avg RTT of the adjacent
 * time window since the previous call. Only accumulated while urma perf is
 * enabled (urma_perf_is_enabled(); urma_start_path_perf() merely resets the
 * window). */
static atomic_ullong g_ubagg_rtt_sum[URMA_UBAGG_SQE_TOTAL_DIM][URMA_UBAGG_SQE_TOTAL_DIM];
static atomic_uint g_ubagg_rtt_cnt[URMA_UBAGG_SQE_TOTAL_DIM][URMA_UBAGG_SQE_TOTAL_DIM];
/* Switch for the path perf interface (urma_start_path_perf()/urma_stop_path_perf()):
 * gates recording of per-path in-flight WR and per-pjetty in-flight counts. */
static atomic_bool g_ubagg_path_enabled;

/* Windowed count of successfully completed WRs since the previous urma_get_perf_info()
 * dump. Reset on each dump so it reports the adjacent time window. */
static atomic_uint g_ubagg_wr_complete_cnt;

void urma_ubagg_switch_init(void)
{
    atomic_init(&g_ubagg_switch_cnt, 0);
    urma_ubagg_sqe_total_init();
    urma_ubagg_path_init();
}

void urma_ubagg_switch_inc(void)
{
    (void)atomic_fetch_add(&g_ubagg_switch_cnt, 1);
}

uint32_t urma_ubagg_switch_get(void)
{
    return atomic_load(&g_ubagg_switch_cnt);
}

void urma_ubagg_sqe_total_init(void)
{
    for (uint32_t i = 0; i < URMA_UBAGG_SQE_TOTAL_DIM; i++) {
        for (uint32_t j = 0; j < URMA_UBAGG_SQE_TOTAL_DIM; j++) {
            atomic_init(&g_ubagg_sqe_total[i][j], 0);
        }
    }
}

/* sqe_total is reported via urma_get_perf_info(), so it belongs to the original
 * perf interface and is only recorded while urma perf is enabled
 * (urma_start_perf()/urma_stop_perf()). */
void urma_ubagg_sqe_total_inc(uint32_t local_idx, uint32_t target_idx, uint32_t wr_count)
{
    if (!urma_perf_is_enabled()) {
        return;
    }
    if (local_idx >= URMA_UBAGG_SQE_TOTAL_DIM || target_idx >= URMA_UBAGG_SQE_TOTAL_DIM) {
        return;
    }
    (void)atomic_fetch_add(&g_ubagg_sqe_total[local_idx][target_idx], (unsigned long long)wr_count);
}

uint64_t urma_ubagg_sqe_total_get(uint32_t local_idx, uint32_t target_idx)
{
    if (local_idx >= URMA_UBAGG_SQE_TOTAL_DIM || target_idx >= URMA_UBAGG_SQE_TOTAL_DIM) {
        return 0;
    }
    return (uint64_t)atomic_load(&g_ubagg_sqe_total[local_idx][target_idx]);
}

void urma_ubagg_path_init(void)
{
    for (uint32_t i = 0; i < URMA_UBAGG_SQE_TOTAL_DIM; i++) {
        for (uint32_t j = 0; j < URMA_UBAGG_SQE_TOTAL_DIM; j++) {
            atomic_init(&g_ubagg_inflight[i][j], 0);
            atomic_init(&g_ubagg_rtt_sum[i][j], 0);
            atomic_init(&g_ubagg_rtt_cnt[i][j], 0);
        }
    }
    atomic_init(&g_ubagg_pjetty_inflight_cnt, 0);
    atomic_init(&g_ubagg_path_enabled, false);
    atomic_init(&g_ubagg_wr_complete_cnt, 0);
}

/* wr_complete_cnt is reported via urma_get_perf_info(), so it belongs to the
 * original perf interface and is only recorded while urma perf is enabled
 * (urma_start_perf()/urma_stop_perf()). */
void urma_ubagg_wr_complete_inc(void)
{
    if (!urma_perf_is_enabled()) {
        return;
    }
    (void)atomic_fetch_add(&g_ubagg_wr_complete_cnt, 1);
}

uint64_t urma_ubagg_wr_complete_snapshot_and_reset(void)
{
    return (uint64_t)atomic_exchange(&g_ubagg_wr_complete_cnt, 0);
}

void urma_ubagg_path_start(void)
{
    for (uint32_t i = 0; i < URMA_UBAGG_SQE_TOTAL_DIM; i++) {
        for (uint32_t j = 0; j < URMA_UBAGG_SQE_TOTAL_DIM; j++) {
            atomic_store(&g_ubagg_rtt_sum[i][j], 0);
            atomic_store(&g_ubagg_rtt_cnt[i][j], 0);
        }
    }
    atomic_store(&g_ubagg_path_enabled, true);
}

void urma_ubagg_path_stop(void)
{
    atomic_store(&g_ubagg_path_enabled, false);
}

bool urma_ubagg_path_is_enabled(void)
{
    return atomic_load(&g_ubagg_path_enabled);
}

void urma_ubagg_inflight_path_inc(uint32_t local_idx, uint32_t target_idx, uint32_t wr_count)
{
    if (local_idx >= URMA_UBAGG_SQE_TOTAL_DIM || target_idx >= URMA_UBAGG_SQE_TOTAL_DIM) {
        return;
    }
    (void)atomic_fetch_add(&g_ubagg_inflight[local_idx][target_idx], wr_count);
}

void urma_ubagg_inflight_path_dec(uint32_t local_idx, uint32_t target_idx, uint32_t wr_count)
{
    if (local_idx >= URMA_UBAGG_SQE_TOTAL_DIM || target_idx >= URMA_UBAGG_SQE_TOTAL_DIM) {
        return;
    }
    (void)atomic_fetch_sub(&g_ubagg_inflight[local_idx][target_idx], wr_count);
}

uint64_t urma_ubagg_inflight_path_get(uint32_t local_idx, uint32_t target_idx)
{
    if (local_idx >= URMA_UBAGG_SQE_TOTAL_DIM || target_idx >= URMA_UBAGG_SQE_TOTAL_DIM) {
        return 0;
    }
    return (uint64_t)atomic_load(&g_ubagg_inflight[local_idx][target_idx]);
}

void urma_ubagg_pjetty_inflight_inc(void)
{
    (void)atomic_fetch_add(&g_ubagg_pjetty_inflight_cnt, 1);
}

void urma_ubagg_pjetty_inflight_dec(void)
{
    (void)atomic_fetch_sub(&g_ubagg_pjetty_inflight_cnt, 1);
}

uint32_t urma_ubagg_pjetty_inflight_get(void)
{
    return atomic_load(&g_ubagg_pjetty_inflight_cnt);
}

void urma_ubagg_rtt_add(uint32_t local_idx, uint32_t target_idx, uint64_t rtt_ns)
{
    if (local_idx >= URMA_UBAGG_SQE_TOTAL_DIM || target_idx >= URMA_UBAGG_SQE_TOTAL_DIM) {
        return;
    }
    (void)atomic_fetch_add(&g_ubagg_rtt_sum[local_idx][target_idx], (unsigned long long)rtt_ns);
    (void)atomic_fetch_add(&g_ubagg_rtt_cnt[local_idx][target_idx], 1);
}

/* Snapshot the windowed RTT accumulators into caller-provided flat arrays
 * (indexed as [local_idx * DIM + target_idx]) and reset them so the next call
 * covers the adjacent time window. Concurrent CR-time adds may lose/gain at
 * most one sample across the snapshot boundary — acceptable for statistics. */
void urma_ubagg_rtt_snapshot_and_reset(uint64_t *sum_out, uint64_t *cnt_out)
{
    for (uint32_t i = 0; i < URMA_UBAGG_SQE_TOTAL_DIM; i++) {
        for (uint32_t j = 0; j < URMA_UBAGG_SQE_TOTAL_DIM; j++) {
            uint32_t idx = i * URMA_UBAGG_SQE_TOTAL_DIM + j;
            sum_out[idx] = (uint64_t)atomic_load(&g_ubagg_rtt_sum[i][j]);
            cnt_out[idx] = (uint64_t)atomic_load(&g_ubagg_rtt_cnt[i][j]);
            atomic_store(&g_ubagg_rtt_sum[i][j], 0);
            atomic_store(&g_ubagg_rtt_cnt[i][j], 0);
        }
    }
}
