/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: FusedDeepMoe tilingData definition file
 * Create: 2026-06-29
 * Note:
 * History: 2026-06-29 create FusedDeepMoe tilingData definition file
 */

#ifndef FUSED_DEEP_MOE_TILING_H
#define FUSED_DEEP_MOE_TILING_H

#include <cstdint>
#include "kernel_tiling/kernel_tiling.h"

#define ENABLE_REUSE_MEMORY

namespace Cam {
struct WorkSpaceOffset {
    int64_t shareX1TokenOffset;
    int64_t x1TokenOffset;
    int64_t shareX1ScaleOffset;
    int64_t x1ScaleOffset;
    int64_t shareSwigluOffset;
    int64_t swigluOffset;
    int64_t shareX2TokenOffset;
    int64_t x2TokenOffset;
    int64_t shareX2ScaleOffset;
    int64_t x2ScaleOffset;
    int64_t swapSpaceOffset;
    int64_t y2TokenOffset;
    int64_t groupListOffset;
    int64_t expandIdxOffset;
    int64_t epSendCountOffset;
    int64_t reservedOffset;
};

struct FusedDeepMoeInfo {
    uint32_t epRankSize;
    uint32_t epRankId;
    uint32_t moeExpertNum;
    uint32_t moeExpertNumPerRank;
    uint32_t quantMode;
    uint32_t globalBs;
    uint32_t bs;
    uint32_t k;
    uint32_t h;
    uint32_t aicNum;
    uint32_t aivNum;
    uint64_t totalUbSize;
    uint64_t totalWinSize;
    uint64_t gmm1HLen;
    uint64_t shareGmm1HLen;
    uint32_t isW4A8;
    bool isTensorList;
};

struct FusedDeepMoeTilingData {
    Mc2InitTiling mc2InitTiling;
    Mc2CcTiling mc2CcTiling;
    FusedDeepMoeInfo disGmmDeqSwigluQuantGmmDeqComInfo;
    WorkSpaceOffset workSpaceOffset;
};

constexpr uint32_t GM_ALIGN_BYTE = 512;
constexpr uint32_t CUSTOM_PRELOAD_STAGES = 1;
constexpr uint32_t CUSTOM_L1_STAGES = 2;
constexpr uint32_t CUSTOM_L0A_STAGES = 2;
constexpr uint32_t CUSTOM_L0B_STAGES = 2;
constexpr uint32_t CUSTOM_L0C_STAGES = 1;
constexpr bool CUSTOM_ENABLE_UNIT_FLAG = true;
constexpr bool CUSTOM_ENABLE_SHUFFLE_K = true;

constexpr uint32_t GMM1_L1M = 256;
constexpr uint32_t GMM1_L1N = 128;
constexpr uint32_t GMM1_L1K = 1024;  // W4A8 (INT4, 2 elements/byte)
constexpr uint32_t GMM1_L0K = 256;
constexpr uint32_t GMM1_L1K_W8A8 = 512;  // W8A8 (INT8)
constexpr uint32_t GMM1_L0K_W8A8 = 128;
constexpr uint32_t GMM1_EPIM = 64;
constexpr uint32_t GMM1_SWIZZLE_OFFSET = 3;
constexpr uint32_t GMM1_SWIZZLE_DIRECTION = 0;

constexpr uint32_t GMM2_L1A_STAGES = 4;
constexpr uint32_t GMM2_L1B_STAGES = 2;
constexpr uint32_t GMM2_L0A_STAGES = 4;
constexpr uint32_t GMM2_L0B_STAGES = 2;
constexpr uint32_t GMM2_L1M = 128;
constexpr uint32_t GMM2_L1N = 256;
constexpr uint32_t GMM2_L1K = 1024;  // W4A8
constexpr uint32_t GMM2_L0K = 256;
constexpr uint32_t GMM2_L1K_W8A8 = 512;  // W8A8
constexpr uint32_t GMM2_L0K_W8A8 = 128;
constexpr uint32_t GMM2_EPIM = 32;
constexpr uint32_t GMM2_SWIZZLE_OFFSET = 3;
constexpr uint32_t GMM2_SWIZZLE_DIRECTION = 0;

constexpr uint32_t WORKSPACE_STAGES = 4;

constexpr uint32_t EXEC_FLAG_DEEP_FUSE = (1U << 0);
constexpr uint32_t EXEC_FLAG_TENSOR_LIST = (1U << 1);
constexpr uint32_t EXEC_FLAG_X_ACTIVE_MASK = (1U << 2);
constexpr uint32_t EXEC_FLAG_SHARED_EXPERT = (1U << 3);
constexpr uint32_t EXEC_FLAG_SMOOTH_QUANT = (1U << 4);
} // namespace Cam
#endif  // FUSED_DEEP_MOE_TILING_H
