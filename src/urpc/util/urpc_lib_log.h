/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: urpc lib log
 * Create: 2024-3-12
 */

#ifndef URPC_LIB_LOG_H
#define URPC_LIB_LOG_H

#include <stdio.h>
#include "urpc_framework_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define URPC_LOG_PRINT_PERIOD_MS 1000
#define URPC_LOG_PRINT_TIMES 10

bool urpc_log_drop(urpc_log_level_t level);
void urpc_log(urpc_log_level_t level, const char *function, int line, const char *format, ...);
bool urpc_log_limit(uint32_t *print_count, uint64_t *last_time);

#define URPC_LIB_LOG(level_, ...)  if (!urpc_log_drop(level_)) {                   \
        urpc_log(level_, __func__, __LINE__, ##__VA_ARGS__);                       \
    }

#define URPC_LIB_LIMIT_LOG(level_, ...)  if (!urpc_log_drop(level_)) {                                  \
        static uint32_t count_call = 0;                                                                 \
        static uint64_t last_time = 0;                                                                  \
        if (urpc_log_limit(&count_call, &last_time)) {  \
            urpc_log(level_, __func__, __LINE__, ##__VA_ARGS__);                                        \
        }                                                                                               \
    }                                                                                                   \

#define URPC_LIB_LOG_ERR(frm_, ...) URPC_LIB_LOG(URPC_LOG_LEVEL_ERR, frm_, ##__VA_ARGS__)
#define URPC_LIB_LOG_WARN(frm_, ...) URPC_LIB_LOG(URPC_LOG_LEVEL_WARN, frm_, ##__VA_ARGS__)
#define URPC_LIB_LOG_NOTICE(frm_, ...) URPC_LIB_LOG(URPC_LOG_LEVEL_NOTICE, frm_, ##__VA_ARGS__)
#define URPC_LIB_LOG_INFO(frm_, ...) URPC_LIB_LOG(URPC_LOG_LEVEL_INFO, frm_, ##__VA_ARGS__)
#define URPC_LIB_LOG_DEBUG(frm_, ...) URPC_LIB_LOG(URPC_LOG_LEVEL_DEBUG, frm_, ##__VA_ARGS__)

#define URPC_LIB_LIMIT_LOG_ERR(frm_, ...) URPC_LIB_LIMIT_LOG(URPC_LOG_LEVEL_ERR, frm_, ##__VA_ARGS__)
#define URPC_LIB_LIMIT_LOG_WARN(frm_, ...) URPC_LIB_LIMIT_LOG(URPC_LOG_LEVEL_WARN, frm_, ##__VA_ARGS__)
#define URPC_LIB_LIMIT_LOG_NOTICE(frm_, ...) URPC_LIB_LIMIT_LOG(URPC_LOG_LEVEL_NOTICE, frm_, ##__VA_ARGS__)
#define URPC_LIB_LIMIT_LOG_INFO(frm_, ...) URPC_LIB_LIMIT_LOG(URPC_LOG_LEVEL_INFO, frm_, ##__VA_ARGS__)
#define URPC_LIB_LIMIT_LOG_DEBUG(frm_, ...) URPC_LIB_LIMIT_LOG(URPC_LOG_LEVEL_DEBUG, frm_, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif // URPC_LIB_LOG_H
