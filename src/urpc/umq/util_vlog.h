/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: Provide vlog module
 * Create: 2025-07-29
 */

#ifndef UTIL_VLOG_H
#define UTIL_VLOG_H

#include <stdio.h>
#include <syslog.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UTIL_VLOG_SIZE              (1024)
#define UTIL_VLOG_NAME_STR_LEN      (64)

typedef enum util_vlog_level {
    UTIL_VLOG_LEVEL_EMERG = 0,
    UTIL_VLOG_LEVEL_ALERT,
    UTIL_VLOG_LEVEL_CRIT,
    UTIL_VLOG_LEVEL_ERR,
    UTIL_VLOG_LEVEL_WARN,
    UTIL_VLOG_LEVEL_NOTICE,
    UTIL_VLOG_LEVEL_INFO,
    UTIL_VLOG_LEVEL_DEBUG,
    UTIL_VLOG_LEVEL_MAX,
} util_vlog_level_t;

typedef struct util_vlog_ctx {
    util_vlog_level_t level;
    char vlog_name[UTIL_VLOG_NAME_STR_LEN];
} util_vlog_ctx_t;

#define UTIL_VLOG(__ctx, __level, ...)  \
    if (!util_vlog_drop(__ctx, __level)) {  \
        util_vlog_output(__ctx, __level, __func__, __LINE__, ##__VA_ARGS__);    \
    }

#define UTIL_VLOG_ERR(__ctx, __format, ...) UTIL_VLOG(__ctx, UTIL_VLOG_LEVEL_ERR,  __format, ##__VA_ARGS__)
#define UTIL_VLOG_WARN(__ctx, __format, ...) UTIL_VLOG(__ctx, UTIL_VLOG_LEVEL_WARN, __format, ##__VA_ARGS__)
#define UTIL_VLOG_NOTICE(__ctx, __format, ...) UTIL_VLOG(__ctx, UTIL_VLOG_LEVEL_NOTICE, __format, ##__VA_ARGS__)
#define UTIL_VLOG_INFO(__ctx, __format, ...) UTIL_VLOG(__ctx, UTIL_VLOG_LEVEL_INFO, __format, ##__VA_ARGS__)
#define UTIL_VLOG_DEBUG(__ctx, __format, ...)  UTIL_VLOG(__ctx, UTIL_VLOG_LEVEL_DEBUG, __format, ##__VA_ARGS__)

int util_vlog_create_context(util_vlog_level_t level, char *vlog_name, util_vlog_ctx_t *ctx);

static inline bool util_vlog_drop(util_vlog_ctx_t *ctx, util_vlog_level_t level)
{
    return level > ctx->level;
}

void util_vlog_output(
    util_vlog_ctx_t *ctx, util_vlog_level_t level, const char *function, int line, const char *format, ...);

util_vlog_level_t util_vlog_level_converter_from_str(const char *str, util_vlog_level_t default_level);
const char *util_vlog_level_converter_to_str(util_vlog_level_t level);

#ifdef __cplusplus
}
#endif

#endif