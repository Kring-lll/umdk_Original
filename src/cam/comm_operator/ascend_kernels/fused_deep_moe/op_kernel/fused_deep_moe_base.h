/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: Definition of communication group related structures
 * Create: 2025-07-19
 * Note:
 * History: 2025-07-19 Create a definition file for a distribution group related structure
 */
#ifndef FUSED_DEEP_MOE_BASE_H
#define FUSED_DEEP_MOE_BASE_H

#include "moe_distribute_base.h"

/* log level macros */
enum MoelogLevel {
    MOE_LOG_LEVEL_ERROR = 0,
    MOE_LOG_LEVEL_WARNING = 1,
    MOE_LOG_LEVEL_INFO = 2,
    MOE_LOG_LEVEL_DEBUG = 3,
    MOE_LOG_LEVEL_DETAIL = 4,
};

#define MOE_LOG_LEVEL MOE_LOG_LEVEL_DEBUG

#define MOE_LOG(level, fmt, ...)                                                                                       \
    do {                                                                                                               \
        if constexpr (level <= MOE_LOG_LEVEL) {                                                                        \
            AscendC::printf("[FusedDeepMoe][%s] " fmt "\n", __FUNCTION__, ##__VA_ARGS__);                              \
        }                                                                                                              \
    } while (0)

#define MOE_LOG_ERROR(fmt, ...)   MOE_LOG(MOE_LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)
#define MOE_LOG_WARNING(fmt, ...) MOE_LOG(MOE_LOG_LEVEL_WARNING, fmt, ##__VA_ARGS__)
#define MOE_LOG_INFO(fmt, ...)    MOE_LOG(MOE_LOG_LEVEL_INFO, fmt, ##__VA_ARGS__)
#define MOE_LOG_DEBUG(fmt, ...)   MOE_LOG(MOE_LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)
#define MOE_LOG_DETAIL(fmt, ...)  MOE_LOG(MOE_LOG_LEVEL_DETAIL, fmt, ##__VA_ARGS__)

#define TemplateMC2TypeClass typename ExpandXType, typename W1ScaleType, typename W2ScaleType, typename ExpandIdxType, bool IsNeedReduceScatter, uint32_t EXEC_FLAG
#define TemplateMC2TypeFunc ExpandXType, W1ScaleType, W2ScaleType, ExpandIdxType, IsNeedReduceScatter, EXEC_FLAG

#define TemplateDispatchTypeClass                                                                                  \
            typename XType, typename ExpandXOutType, bool StaticQuant, bool DynamicQuant, bool IsSmoothScaleExist, \
            bool IsNeedAllgater, uint32_t EXEC_FLAG
#define TemplateDispatchTypeFunc                                                  \
            XType, ExpandXOutType, StaticQuant, DynamicQuant, IsSmoothScaleExist, \
            IsNeedAllgater, EXEC_FLAG

#define SPIN_WAIT_CYCLES() \
    do { \
        int64_t st = AscendC::GetSystemCycle(); \
        while (AscendC::GetSystemCycle() - st < (25)) { ; } \
    } while (0)

#endif  // FUSED_DEEP_MOE_BASE_H
 