/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: define qbuf pool helper function
 * Create: 2026-6-26
 * Note:
 * History: 2026-6-26
 */

#ifndef UMQ_QBUF_POOL_HELPER_H
#define UMQ_QBUF_POOL_HELPER_H

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

int umq_qbuf_alloc(uint32_t request_size, uint32_t num, umq_alloc_option_t *option, umq_buf_list_t *list);
int umq_qbuf_pool_cfg_check(const umq_init_cfg_t *cfg, umq_qbuf_pool_plan_t *plan);

#ifdef __cplusplus
}
#endif

#endif
