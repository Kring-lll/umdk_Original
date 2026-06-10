/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: ShmemMoeCombineNormal tiling data header file
 * Create: 2026-06-10
 * Note:
 * History: 2026-06-10 create ShmemMoeCombineNormal tiling data header file
 */
#ifndef SHMEM_MOE_COMBINE_NORMAL_TILING_H
#define SHMEM_MOE_COMBINE_NORMAL_TILING_H

#include <cstdint>
#include "kernel_tiling/kernel_tiling.h"

struct ShmemMoeCombineNormalInfo {
    uint32_t epWorldSize;
    uint32_t tpWorldSize;
    uint32_t epRankId;
    uint32_t tpRankId;
    uint32_t expertShardType;
    uint32_t moeExpertNum;
    uint32_t moeExpertPerRankNum;
    uint32_t globalBs;
    uint32_t bs;
    uint32_t k;
    uint32_t h;
    uint32_t aivNum;
    uint64_t totalUbSize;
    uint64_t totalWinSize;
    float armAvgFactor;
    float epsilon;
    bool isEnableDiagnose;
};
struct ShmemMoeCombineNormalTilingData {
    ShmemMoeCombineNormalInfo moeCombineNormalInfo;
    uint64_t shmemPtr;  // shmem symmetric point
};

#endif  // SHMEM_MOE_COMBINE_NORMAL_TILING_H
