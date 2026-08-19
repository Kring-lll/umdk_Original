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

#include "urma_private.h"

static atomic_uint g_ubagg_switch_cnt;

/* Cumulative SQE total per link [local_idx][target_idx].
 * Tracks every WR submitted by the bond datapath, independent of CR-driven
 * decrement of the in-flight sqe_cnt[][]. Monotonically non-decreasing. */
static atomic_ullong g_ubagg_sqe_total[URMA_UBAGG_SQE_TOTAL_DIM][URMA_UBAGG_SQE_TOTAL_DIM];

void urma_ubagg_switch_init(void)
{
    atomic_init(&g_ubagg_switch_cnt, 0);
    urma_ubagg_sqe_total_init();
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

void urma_ubagg_sqe_total_inc(uint32_t local_idx, uint32_t target_idx, uint32_t wr_count)
{
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
