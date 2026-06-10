/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: zero-buffer operator tiling error log header file
 * Create: 2026-06-10
 * Note:
 * History: 2026-06-10 create zero-buffer operator tiling error log header file
 */
#ifndef OPS_BUILT_IN_OP_TILING_ERROR_LOG_H_
#define OPS_BUILT_IN_OP_TILING_ERROR_LOG_H_

#include <string>
#include "toolchain/slog.h"

#define OP_LOGI(opname, ...)
#define OP_LOGW(opname, ...)             \
    do {                                 \
        printf("[WARN][%s] ", (opname)); \
        printf(__VA_ARGS__);             \
        printf("\n");                    \
    } while (0)

#define OP_LOGE_WITHOUT_REPORT(opname, ...) \
    do {                                    \
        printf("[ERRORx][%s] ", (opname));  \
        printf(__VA_ARGS__);                \
        printf("\n");                       \
    } while (0)

#define OP_LOGE(opname, ...)              \
    do {                                  \
        printf("[ERROR][%s] ", (opname)); \
        printf(__VA_ARGS__);              \
        printf("\n");                     \
    } while (0)

#define OP_LOGD(opname, ...)

namespace optiling {

#define VECTOR_INNER_ERR_REPORT_TILIING(op_name, err_msg, ...)   \
    do {                                                         \
        OP_LOGE_WITHOUT_REPORT(op_name, err_msg, ##__VA_ARGS__); \
    } while (0)

#define OP_TILING_CHECK(cond, log_func, expr) \
    do {                                      \
        if (cond) {                           \
            log_func;                         \
            expr;                             \
        }                                     \
    } while (0)
}  // namespace optiling

#endif  // OPS_BUILT_IN_OP_TILING_ERROR_LOG_H_
