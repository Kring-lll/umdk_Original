/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: Provide the umq vlog module
 * Create: 2025-08-04
 */

#include "umq_vlog.h"

static util_vlog_ctx_t g_umq_vlog_ctx = {
    .level = UTIL_VLOG_LEVEL_INFO,
    .vlog_name = "UMQ",
    .vlog_output_func = default_vlog_output,
    .rate_limited = {
        .interval_ms = UTIL_VLOG_PRINT_PERIOD_MS,
        .num = UTIL_VLOG_PRINT_TIMES,
    }
};

util_vlog_ctx_t *umq_get_log_ctx(void)
{
    return (util_vlog_ctx_t *)&g_umq_vlog_ctx;
}