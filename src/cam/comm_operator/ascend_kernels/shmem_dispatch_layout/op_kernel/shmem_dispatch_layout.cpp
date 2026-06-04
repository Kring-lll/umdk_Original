/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Shmem dispatch layout function implementation file
 * Create: 2026-06-09
 * Note:
 * History: 2026-06-09 create shmem dispatch layout function file
 */

#include "kernel_operator.h"
#include "shmem_dispatch_layout_a2.h"
#include "shmem_dispatch_layout_tiling.h"
#include "shmem_dispatch_layout.h"

#define TILING_KEY_INT 23
#define TILING_KEY_A2_INT 123

extern "C" __global__ __aicore__ void shmem_dispatch_layout(GM_ADDR topkIdx, GM_ADDR numTokensPerRank,
                                                      GM_ADDR numTokensPerExpert, GM_ADDR isTokenInRank,
                                                      GM_ADDR notifySendData, GM_ADDR sendTokenIdxSmall,
                                                      GM_ADDR workspace, GM_ADDR tiling)
{
    REGISTER_TILING_DEFAULT(ShmemDispatchLayoutTilingData);
    GET_TILING_DATA_WITH_STRUCT(ShmemDispatchLayoutTilingData, tilingData, tiling);

    TPipe pipe;

    if (TILING_KEY_IS(TILING_KEY_INT)) {
        MoeShmemDispatchLayout::ShmemDispatchLayout<int32_t> op;
        op.Init(topkIdx, numTokensPerRank, numTokensPerExpert, isTokenInRank, notifySendData, sendTokenIdxSmall,
                workspace, &pipe, &tilingData);
        op.Process();
    } else if (TILING_KEY_IS(TILING_KEY_A2_INT)) {
        MoeShmemDispatchLayoutA2::ShmemDispatchLayoutA2<int32_t> op;
        op.Init(topkIdx, numTokensPerRank, numTokensPerExpert, isTokenInRank, notifySendData, sendTokenIdxSmall,
                workspace, &pipe, &tilingData);
        op.Process();
    }
}
