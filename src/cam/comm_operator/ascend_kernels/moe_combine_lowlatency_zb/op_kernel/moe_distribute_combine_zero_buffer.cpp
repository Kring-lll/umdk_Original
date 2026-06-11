/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: MoeDistributeCombineZeroBuffer function implementation file
 * Create: 2026-06-10
 * Note:
 * History: 2026-06-10 create MoeDistributeCombineZeroBuffer function implementation file
 */
#include "kernel_operator.h"
#include "moe_distribute_combine_zero_buffer_tiling.h"
#include "moe_distribute_combine_zero_buffer.h"

using namespace AscendC;
using namespace MoeDistributeCombineV2Impl;

namespace {
template <TemplateMC2TypeClass>
__aicore__ inline void ExecMoeDistributeCombineZeroBuffer(
    GM_ADDR expandX, GM_ADDR expertIds, GM_ADDR assistInfoForCombine, GM_ADDR epSendCount, GM_ADDR tpSendCount,
    GM_ADDR scales, GM_ADDR xActiveMask, GM_ADDR sharedExpertX, GM_ADDR elasticInfo, GM_ADDR oriX,
    GM_ADDR constExpertAlpha1, GM_ADDR constExpertAlpha2, GM_ADDR constExpertV, GM_ADDR XOut, GM_ADDR workspaceGM,
    GM_ADDR tilingGM, TPipe *pipePtr)
{
    GET_TILING_DATA_WITH_STRUCT(MoeDistributeCombineZeroBufferTilingData, tilingData, tilingGM);
    MoeDistributeCombineZeroBuffer<TemplateMC2TypeFunc> op;
    // PRINTF("[---------------- Initializing CombineV2 ------------------] \n");
    op.Init(expandX, expertIds, assistInfoForCombine, epSendCount, tpSendCount, scales, xActiveMask, sharedExpertX,
            elasticInfo, oriX, constExpertAlpha1, constExpertAlpha2, constExpertV, XOut, workspaceGM, pipePtr,
            &tilingData);
    // PRINTF("[---------------- Processing CombineV2 ------------------] \n");
    op.Process();
}
}  // namespace

extern "C" __global__ __aicore__ void moe_distribute_combine_zero_buffer(
    GM_ADDR expandX, GM_ADDR expertIds, GM_ADDR assistInfoForCombine, GM_ADDR epSendCount, GM_ADDR scales,
    GM_ADDR tpSendCount, GM_ADDR xActiveMask, GM_ADDR activationScale, GM_ADDR weightScale, GM_ADDR groupList,
    GM_ADDR expandScales, GM_ADDR sharedExpertX, GM_ADDR elasticInfo, GM_ADDR oriX, GM_ADDR constExpertAlpha1,
    GM_ADDR constExpertAlpha2, GM_ADDR constExpertV, GM_ADDR XOut, GM_ADDR workspaceGM, GM_ADDR tilingGM)

{
    // PRINTF("[---------------- Calling CombineV2 ------------------] \n");
    REGISTER_TILING_DEFAULT(MoeDistributeCombineZeroBufferTilingData);
    TPipe pipe;

#if (ORIG_DTYPE_EXPAND_X == DT_BF16 || ORIG_DTYPE_EXPAND_X == DT_FLOAT16)
    if (TILING_KEY_IS(10100)) {  // tp=2 IsInt8Quant=0
        ExecMoeDistributeCombineZeroBuffer<DTYPE_EXPAND_X, DTYPE_X, int32_t, true, false>(
            expandX, expertIds, assistInfoForCombine, epSendCount, tpSendCount, scales, xActiveMask, sharedExpertX,
            elasticInfo, oriX, constExpertAlpha1, constExpertAlpha2, constExpertV, XOut, workspaceGM, tilingGM, &pipe);
    }
    if (TILING_KEY_IS(10000)) {  // tp=1 IsInt8Quant=0
        ExecMoeDistributeCombineZeroBuffer<DTYPE_EXPAND_X, DTYPE_X, int32_t, false, false>(
            expandX, expertIds, assistInfoForCombine, epSendCount, tpSendCount, scales, xActiveMask, sharedExpertX,
            elasticInfo, oriX, constExpertAlpha1, constExpertAlpha2, constExpertV, XOut, workspaceGM, tilingGM, &pipe);
    }
    if (TILING_KEY_IS(10120)) {  // tp=2 IsInt8Quant=1
        ExecMoeDistributeCombineZeroBuffer<DTYPE_EXPAND_X, DTYPE_X, int32_t, true, true>(
            expandX, expertIds, assistInfoForCombine, epSendCount, tpSendCount, scales, xActiveMask, sharedExpertX,
            elasticInfo, oriX, constExpertAlpha1, constExpertAlpha2, constExpertV, XOut, workspaceGM, tilingGM, &pipe);
    }
    if (TILING_KEY_IS(10020)) {  // tp=1 IsInt8Quant=1
        ExecMoeDistributeCombineZeroBuffer<DTYPE_EXPAND_X, DTYPE_X, int32_t, false, true>(
            expandX, expertIds, assistInfoForCombine, epSendCount, tpSendCount, scales, xActiveMask, sharedExpertX,
            elasticInfo, oriX, constExpertAlpha1, constExpertAlpha2, constExpertV, XOut, workspaceGM, tilingGM, &pipe);
    }
#endif
}
