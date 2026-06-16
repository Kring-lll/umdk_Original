/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: define qbuf pool config check function
 * Create: 2026-6-12
 */

#ifndef UMQ_QBUF_POOL_CFG_H
#define UMQ_QBUF_POOL_CFG_H

#include "umq_qbuf_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct umq_qbuf_pool_plan {
    uint32_t tiny_block_size;
    uint64_t tiny_io_buf_size;
    uint64_t normal_io_buf_size;
    uint64_t normal_pool_budget_size;
} umq_qbuf_pool_plan_t;

int umq_qbuf_pool_cfg_check(const umq_init_cfg_t *cfg, umq_qbuf_pool_plan_t *plan);

#ifdef __cplusplus
}
#endif

#endif
