/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: notify dispatch kernel A5 part operator entrance (multi-mesh layered support)
 * Create: 2026-05-26
 * Note:
 * History: 2026-05-26 create notify dispatch A5 kernel part operator entrance
 */

#include "kernel_operator.h"
#include "notify_dispatch_a5.h"
#include "notify_dispatch_tiling_a5.h"

#define TILING_KEY_FLOAT16 20
#define TILING_KEY_BFLOAT16 21
#define TILING_KEY_FLOAT 22
#define TILING_KEY_INT 23
#define TILING_KEY_A5_FLOAT16 220
#define TILING_KEY_A5_BFLOAT16 221
#define TILING_KEY_A5_FLOAT 222
#define TILING_KEY_A5_INT 223

#define KERNEL_USE_WORKSPACE (1 * 1024 * 1024)

extern "C" __global__ __aicore__ void notify_dispatch_a5(GM_ADDR sendData, GM_ADDR tokenPerExpertData, GM_ADDR tmpData,
                                                         GM_ADDR sendDataOffset, GM_ADDR recvData,
                                                         GM_ADDR tokenMeshIdx, GM_ADDR tokensUniquePerMesh,
                                                         GM_ADDR epRankTokenCnt, GM_ADDR localEpTokenCnt,
                                                         GM_ADDR srcOffsetRankTokenIdx, GM_ADDR dstOffsetRankTokenIdx,
                                                         GM_ADDR offsetInner, GM_ADDR countOuter, GM_ADDR expandIdx,
                                                         GM_ADDR totalRecvTokens, GM_ADDR workspace, GM_ADDR tiling)
{
    REGISTER_TILING_DEFAULT(NotifyDispatchA5TilingData);
    GET_TILING_DATA_WITH_STRUCT(NotifyDispatchA5TilingData, tilingData, tiling);

    int localRank = tilingData.notifyDispatchInfoA5.localRankId;
    int localRankSize = tilingData.notifyDispatchInfoA5.localRankSize;
    int rank = tilingData.notifyDispatchInfoA5.rankId;
    int rankSize = tilingData.notifyDispatchInfoA5.rankSize;
    int64_t len = tilingData.notifyDispatchInfoA5.sendCount;
    int64_t numTokens = tilingData.notifyDispatchInfoA5.numTokens;
    int64_t topkNum = tilingData.notifyDispatchInfoA5.topkNum;
    int64_t numExperts = tilingData.notifyDispatchInfoA5.numExperts;

    GM_ADDR sendDataInput = sendData;
    GM_ADDR tokenPerExpertDataInput = tokenPerExpertData;
    GM_ADDR tmpDataInput = tmpData;

    GM_ADDR sendDataOffsetOutput = sendDataOffset;
    GM_ADDR recvDataOutput = recvData;
    GM_ADDR tokenMeshIdxOutput = tokenMeshIdx;
    GM_ADDR tokensUniquePerMeshOutput = tokensUniquePerMesh;
    GM_ADDR epRankTokenCntOutput = epRankTokenCnt;
    GM_ADDR localEpTokenCntOutput = localEpTokenCnt;
    GM_ADDR srcOffsetRankTokenIdxOutput = srcOffsetRankTokenIdx;
    GM_ADDR dstOffsetRankTokenIdxOutput = dstOffsetRankTokenIdx;
    GM_ADDR offsetInnerOutput = offsetInner;
    GM_ADDR countOuterOutput = countOuter;
    GM_ADDR expandIdxOutput = expandIdx;
    GM_ADDR totalRecvTokensOutput = totalRecvTokens;

    // fill in unused args
    uint32_t extraFlag = 0;
    GM_ADDR scale = nullptr;
    int root = 0;
    int op = 0;
    int cycleCount = 0;
    int64_t scaleCount = 0;
    GM_ADDR offset = nullptr;
    int blockNum = GetBlockNum();

    if (TILING_KEY_IS(TILING_KEY_A5_INT)) {
        NotifyDispatchA5<int> opKernel(rank, rankSize, extraFlag);
        opKernel.Init(sendDataInput, tokenPerExpertDataInput, tmpDataInput, sendDataOffsetOutput,
            recvDataOutput, len, numTokens, topkNum, numExperts, op, root, cycleCount, scale, scaleCount, offset,
            localRank, localRankSize, tokenMeshIdxOutput, tokensUniquePerMeshOutput, epRankTokenCntOutput,
            localEpTokenCntOutput, srcOffsetRankTokenIdxOutput, dstOffsetRankTokenIdxOutput, offsetInnerOutput,
            countOuterOutput, expandIdxOutput, totalRecvTokensOutput, workspace, tiling);
        opKernel.Process();
    }
}
