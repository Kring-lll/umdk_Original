/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: notify dispatch A5 interface part (multi-mesh layered support)
 * Create: 2026-05-26
 * Note:
 * History: 2026-05-26 create notify dispatch A5 interface part
 */

#include <string.h>
#include "graph/types.h"
#include "aclnnInner_notify_dispatch_a5.h"
#include "aclnn_notify_dispatch_a5.h"

namespace {
static constexpr int32_t NNOPBASE_HCCL_SERVER_TYPE_AICPU = 0;
static constexpr int32_t NNOPBASE_HCCL_SERVER_TYPE_MTE = 1;
static constexpr int32_t NNOPBASE_HCCL_SERVER_TYPE_END = 2;
} // namespace
extern "C" void __attribute__((weak)) NnopbaseSetHcclServerType(void *executor, int32_t sType);

#ifdef __cplusplus
extern "C" {
#endif

aclnnStatus aclnnNotifyDispatchA5GetWorkspaceSize(
    const aclTensor *sendData,
    const aclTensor *tokenPerExpertData,
    const aclTensor *tmpData,
    int64_t sendCount,
    int64_t numTokens,
    int64_t topkNum,
    int64_t numExperts,
    char *commGroup,
    int64_t rankSize,
    int64_t rankId,
    int64_t localRankSize,
    int64_t localRankId,
    const aclTensor *sendDataOffsetOut,
    const aclTensor *recvDataOut,
    const aclTensor *tokenMeshIdxOut,
    const aclTensor *tokenUniquePerMeshOut,
    const aclTensor *epRankTokenCntOut,
    const aclTensor *localEpTokenCntOut,
    const aclTensor *srcOffsetRankTokenIdxOut,
    const aclTensor *dstOffsetRankTokenIdxOut,
    const aclTensor *offsetInnerOut,
    const aclTensor *countOuterOut,
    const aclTensor *expandIdxOut,
    const aclTensor *totalRecvTokensOut,
    uint64_t *workspaceSize,
    aclOpExecutor **executor)
{
    return aclnnInnerNotifyDispatchA5GetWorkspaceSize(
        sendData,
        tokenPerExpertData,
        tmpData,
        sendCount,
        numTokens,
        topkNum,
        numExperts,
        commGroup,
        rankSize,
        rankId,
        localRankSize,
        localRankId,
        sendDataOffsetOut,
        recvDataOut,
        tokenMeshIdxOut,
        tokenUniquePerMeshOut,
        epRankTokenCntOut,
        localEpTokenCntOut,
        srcOffsetRankTokenIdxOut,
        dstOffsetRankTokenIdxOut,
        offsetInnerOut,
        countOuterOut,
        expandIdxOut,
        totalRecvTokensOut,
        workspaceSize,
        executor);
}

aclnnStatus aclnnNotifyDispatchA5(
    void *workspace,
    uint64_t workspaceSize,
    aclOpExecutor *executor,
    aclrtStream stream)
{
    if (NnopbaseSetHcclServerType) {
        NnopbaseSetHcclServerType(executor, NNOPBASE_HCCL_SERVER_TYPE_AICPU);
    }
    return aclnnInnerNotifyDispatchA5(
        workspace,
        workspaceSize,
        executor,
        stream);
}


#ifdef __cplusplus
}
#endif
