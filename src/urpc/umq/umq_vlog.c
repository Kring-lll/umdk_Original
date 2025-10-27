/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: Provide the umq vlog module
 * Create: 2025-08-04
 */

#include "umq_vlog.h"

static util_vlog_ctx_t g_umq_vlog_ctx = { UTIL_VLOG_LEVEL_INFO, "UMQ" };

util_vlog_ctx_t *umq_get_log_ctx(void)
{
    return (util_vlog_ctx_t *)&g_umq_vlog_ctx;
}