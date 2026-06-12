/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: realize qbuf pool config check function
 * Create: 2026-6-12
 */

#include "umq_qbuf_pool_cfg.h"
#include "umq_tiny_qbuf_pool.h"

static uint64_t umq_normal_pool_supported_block_count(umq_buf_mode_t mode, uint64_t normal_pool_size,
    bool disable_scale_cap)
{
    uint32_t block_size = umq_buf_size_small();
    if (block_size == 0) {
        return 0;
    }

    if (mode == UMQ_BUF_SPLIT) {
        uint64_t one_block_size = disable_scale_cap ?
            ((UMQ_EMPTY_HEADER_COEFFICIENT + 1) * (uint64_t)sizeof(umq_buf_t) + block_size) :
            ((uint64_t)sizeof(umq_buf_t) + block_size);
        return normal_pool_size / one_block_size;
    }
    if (mode == UMQ_BUF_COMBINE) {
        return normal_pool_size / block_size;
    }
    return 0;
}

static uint64_t umq_without_data_expand_mem_size(const umq_buf_pool_cfg_t *cfg, umq_buf_mode_t mode)
{
    if (mode != UMQ_BUF_SPLIT) {
        return 0;
    }

    uint32_t expansion_block_count = cfg->expansion_block_count == 0 ?
        QBUF_POOL_DEFAULT_EXPANSION_COUNT : cfg->expansion_block_count;
    return (uint32_t)sizeof(umq_buf_t) * UMQ_EMPTY_HEADER_COEFFICIENT * expansion_block_count;
}

int umq_qbuf_pool_cfg_check(const umq_init_cfg_t *cfg, umq_qbuf_pool_plan_t *plan)
{
    if (cfg == NULL || plan == NULL) {
        return -UMQ_ERR_EINVAL;
    }
    (void)memset(plan, 0, sizeof(*plan));

    if (cfg->buf_pool_cfg.enable_tiny_pool) {
        uint64_t tiny_block_count = cfg->buf_pool_cfg.tiny_pool_block_count == 0 ?
            TINY_QBUF_POOL_DEFAULT_BLOCK_COUNT : cfg->buf_pool_cfg.tiny_pool_block_count;
        plan->tiny_block_size = umq_tiny_buf_block_size_bytes(cfg->buf_pool_cfg.tiny_pool_block_size);
        plan->tiny_io_buf_size = tiny_block_count * plan->tiny_block_size;
        if (cfg->buf_mode == UMQ_BUF_SPLIT) {
            plan->tiny_io_buf_size += tiny_block_count * sizeof(umq_buf_t);
        }
    }

    if (cfg->buf_pool_cfg.umq_mem_pool_init_size <= plan->tiny_io_buf_size) {
        UMQ_VLOG_ERR(VLOG_UMQ, "umq mem pool init size %llu is too small for tiny pool size %llu\n",
            (unsigned long long)cfg->buf_pool_cfg.umq_mem_pool_init_size,
            (unsigned long long)plan->tiny_io_buf_size);
        return -UMQ_ERR_EINVAL;
    }

    plan->normal_io_buf_size = cfg->buf_pool_cfg.umq_mem_pool_init_size - plan->tiny_io_buf_size;
    if (cfg->buf_pool_cfg.normal_pool_block_count != 0) {
        uint64_t supported_block_count = umq_normal_pool_supported_block_count(
            cfg->buf_mode, plan->normal_io_buf_size, cfg->buf_pool_cfg.disable_scale_cap);
        if (cfg->buf_pool_cfg.normal_pool_block_count > supported_block_count) {
            UMQ_VLOG_ERR(VLOG_UMQ,
                "normal pool block count %u exceeds supported block count %llu by normal init size %llu\n",
                cfg->buf_pool_cfg.normal_pool_block_count, (unsigned long long)supported_block_count,
                (unsigned long long)plan->normal_io_buf_size);
            return -UMQ_ERR_EINVAL;
        }
    }

    if (!cfg->buf_pool_cfg.disable_scale_cap && cfg->buf_pool_cfg.umq_buf_pool_max_size > QBUF_POOL_MEM_SIZE_MAX) {
        UMQ_VLOG_INFO(VLOG_UMQ, "the maximum value of expansion mem size max %llu exceed %llu\n",
            cfg->buf_pool_cfg.umq_buf_pool_max_size, QBUF_POOL_MEM_SIZE_MAX);
        return -UMQ_ERR_EINVAL;
    }

    uint64_t max_umq_buf_pool_size = cfg->buf_pool_cfg.umq_buf_pool_max_size == 0 ?
        QBUF_POOL_DEFAULT_EXPANSION_MEM_SIZE : cfg->buf_pool_cfg.umq_buf_pool_max_size;
    uint64_t init_required_size = cfg->buf_pool_cfg.umq_mem_pool_init_size;
    if (max_umq_buf_pool_size < init_required_size) {
        UMQ_VLOG_INFO(VLOG_UMQ,
            "max buf pool size %llu is too small to support initial buf pool, required %llu\n",
            max_umq_buf_pool_size, init_required_size);
        return -UMQ_ERR_EINVAL;
    }

    uint64_t without_data_expand_mem_size = umq_without_data_expand_mem_size(&cfg->buf_pool_cfg, cfg->buf_mode);
    if (!cfg->buf_pool_cfg.disable_scale_cap &&
        max_umq_buf_pool_size < init_required_size + without_data_expand_mem_size) {
        UMQ_VLOG_INFO(VLOG_UMQ,
            "max buf pool size %llu is too small to support expand without data buf, required %llu\n",
            max_umq_buf_pool_size, init_required_size + without_data_expand_mem_size);
        return -UMQ_ERR_EINVAL;
    }

    plan->normal_pool_budget_size = max_umq_buf_pool_size - plan->tiny_io_buf_size;
    return UMQ_SUCCESS;
}
