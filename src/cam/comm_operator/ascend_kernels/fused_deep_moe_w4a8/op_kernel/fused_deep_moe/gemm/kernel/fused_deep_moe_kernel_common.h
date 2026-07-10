/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Common constants and macros shared by W4A8 and W8A8 GMM1 kernels
 */
#ifndef FUSED_DEEP_MOE_KERNEL_COMMON_H
#define FUSED_DEEP_MOE_KERNEL_COMMON_H

#include "kernel_operator.h"

// State offsets
constexpr uint32_t STATE_OFFSET = 512;
constexpr uint64_t WIN_STATE_OFFSET = 512 * 1024;
constexpr uint64_t STATE_WIN_OFFSET = 900 * 1024;
constexpr uint64_t GROUP_TOKEN_NUM_OFFSET = 932 * 1024;
constexpr uint64_t SOFT_SYNC_OFFSET = 964 * 1024;
constexpr uint64_t SHARE_QUANT_SOFT_SYNC_OFFSET = 988 * 1024;
constexpr uint32_t SELF_STATE_OFFSET = 256 * 1024;
constexpr uint32_t SUM_TMP_TENSOR_SIZE = 1024;
constexpr uint32_t UB_ALIGN = 32;
constexpr uint32_t TOKEN_EXTRA_SPACE = 512;
constexpr uint32_t INT32_COUNT_PER_BLOCK = 8;
constexpr int64_t LOOP_TMP_SIZE = 4096;
constexpr int32_t SUB_AIV_NUM = 2;
constexpr int32_t ODD_EVEN_BASE = 2;
constexpr int32_t BUFFER_NUM = 2;
constexpr int32_t GATHER_SECOND_NUM = 2;

// Macros
#define OPT_RANK_OFFSET 512
#define CEIL_UP(x) ((((x) + (UB_ALIGN) - 1) / (UB_ALIGN)) * (UB_ALIGN))
#define CEIL(x, y) ((((x) + (((y)) - 1)) / ((y))))
#define UB_BLOCK_SIZE (32)
#define TOKEN_FLAG_1 (0x55555555)
#define TOKEN_FLAG_2 (0x33333333)
#define V_TO_C_FLAG_1 (0x03030303)
#define V_TO_C_FLAG_2 (0x05050505)
#define CV_FLAG_INDEX 0
#define GROUP_ID_INDEX 1
#define PRE_COUNT_INDEX 2
#define SELF_COUNT_INDEX 3
#define TOTAL_COUNT_INDEX 4
#define GROUP_TOKEN_COUNT 3
#define GROUP_INFO_SIZE 32

#endif // FUSED_DEEP_MOE_KERNEL_COMMON_H
