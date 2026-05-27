/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: dispatch normal A5 kernel part operator entrance (multi-mesh layered support)
 * Create: 2026-05-27
 * Note:
 * History: 2026-05-27 create dispatch normal A5 kernel part operator entrance
 */

#include "kernel_operator.h"
#include "cam_moe_distribute_dispatch_a5_layered.h"
#include "dispatch_normal_a5_tiling.h"

#define TILING_KEY_FLOAT16 20
#define TILING_KEY_BFLOAT16 21
#define TILING_KEY_FLOAT 22
#define TILING_KEY_INT 23
#define TILING_KEY_A5_FLOAT16 220
#define TILING_KEY_A5_BFLOAT16 221
#define TILING_KEY_A5_FLOAT 222
#define TILING_KEY_A5_INT 223

#define KERNEL_USE_WORKSPACE (1 * 1024 * 1024)

extern "C" __global__ __aicore__ void dispatch_normal_a5(
    GM_ADDR x, GM_ADDR expertIds, GM_ADDR scales, GM_ADDR xActiveMask, GM_ADDR expertScales, GM_ADDR tokenMeshIdx,
    GM_ADDR tokenMeshCnt, GM_ADDR epRankTokenCnt, GM_ADDR srcOffsetRankTokenIdx, GM_ADDR dstOffsetRankTokenIdx,
    GM_ADDR recvX, GM_ADDR dynamicScalesOut, GM_ADDR expandIdxOut, GM_ADDR expertTokenNumsOut, GM_ADDR epRecvCountOut,
    GM_ADDR expandScalesOut, GM_ADDR dispatchWaitRecvCostStatsOut, GM_ADDR workspace, GM_ADDR tiling)
{
    REGISTER_TILING_DEFAULT(CamMoeDistributeDispatchA5TilingData);
    GET_TILING_DATA_WITH_STRUCT(CamMoeDistributeDispatchA5TilingData, tilingData, tiling);

    // hcomm will set magic later in init
    uint32_t magic = 1;
    GM_ADDR commArgs = nullptr;

    // fill in unused args
    uint32_t extraFlag = 0;
    GM_ADDR scale = nullptr;
    int root = 0;
    int op = 0;
    int cycleCount = 0;
    int64_t scaleCount = 0;
    GM_ADDR offset = nullptr;
    int blockNum = GetBlockNum();

    TPipe pipe;
    if (TILING_KEY_IS(5100001000)) {
        CamMoeDistributeDispatchA5Layered<bfloat16_t, bfloat16_t, false, false, false> opKernel;
        opKernel.Init(x, expertIds, scales, expertScales, tokenMeshIdx, tokenMeshCnt, epRankTokenCnt,
                      srcOffsetRankTokenIdx, dstOffsetRankTokenIdx, recvX, dynamicScalesOut, expandIdxOut,
                      expertTokenNumsOut, epRecvCountOut, expandScalesOut, workspace, &pipe, tiling);
        opKernel.Process();
    } else if (TILING_KEY_IS(5100000000)) {
        CamMoeDistributeDispatchA5Layered<bfloat16_t, bfloat16_t, false, false, false> opKernel;
        opKernel.Init(x, expertIds, scales, expertScales, tokenMeshIdx, tokenMeshCnt, epRankTokenCnt,
                      srcOffsetRankTokenIdx, dstOffsetRankTokenIdx, recvX, dynamicScalesOut, expandIdxOut,
                      expertTokenNumsOut, epRecvCountOut, expandScalesOut, workspace, &pipe, tiling);
        opKernel.Process();
    } else if (TILING_KEY_IS(5100002000)) {
        CamMoeDistributeDispatchA5Layered<bfloat16_t, bfloat16_t, false, false, false> opKernel;
        opKernel.Init(x, expertIds, scales, expertScales, tokenMeshIdx, tokenMeshCnt, epRankTokenCnt,
                      srcOffsetRankTokenIdx, dstOffsetRankTokenIdx, recvX, dynamicScalesOut, expandIdxOut,
                      expertTokenNumsOut, epRecvCountOut, expandScalesOut, workspace, &pipe, tiling);
        opKernel.Process();
    }
}
