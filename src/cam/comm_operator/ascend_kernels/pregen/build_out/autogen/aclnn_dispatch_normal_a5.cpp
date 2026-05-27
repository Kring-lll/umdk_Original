/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: dispatch normal A5 interface part (multi-mesh layered support)
 * Create: 2026-05-27
 * Note:
 * History: 2026-05-27 create dispatch normal A5 interface part
 */

#include <string.h>
#include "graph/types.h"
#include "aclnnInner_dispatch_normal_a5.h"
#include "aclnn_dispatch_normal_a5.h"

namespace {
static constexpr int32_t NNOPBASE_HCCL_SERVER_TYPE_AICPU = 0;
static constexpr int32_t NNOPBASE_HCCL_SERVER_TYPE_MTE = 1;
static constexpr int32_t NNOPBASE_HCCL_SERVER_TYPE_END = 2;
} // namespace
extern "C" void __attribute__((weak)) NnopbaseSetHcclServerType(void *executor, int32_t sType);

#ifdef __cplusplus
extern "C" {
#endif

aclnnStatus aclnnDispatchNormalA5GetWorkspaceSize(
    const aclTensor *x,
    const aclTensor *expertIds,
    const aclTensor *scalesOptional,
    const aclTensor *xActiveMaskOptional,
    const aclTensor *expertScalesOptional,
    const aclTensor *tokenMeshIdxOptional,
    const aclTensor *tokenUniquePerMeshOptional,
    const aclTensor *epRankTokenCntOptional,
    const aclTensor *srcOffsetRankTokenIdxOptional,
    const aclTensor *dstOffsetRankTokenIdxOptional,
    char *groupEp,
    int64_t epWorldSize,
    int64_t epRankId,
    int64_t moeExpertNum,
    char *groupTpOptional,
    int64_t tpWorldSize,
    int64_t tpRankId,
    int64_t expertShardType,
    int64_t sharedExpertNum,
    int64_t sharedExpertRankNum,
    int64_t quantMode,
    int64_t globalBs,
    int64_t expertTokenNumsType,
    const aclTensor *recvXOut,
    const aclTensor *dynamicScalesOut,
    const aclTensor *expandIdxOut,
    const aclTensor *expertTokenNumsOut,
    const aclTensor *epRecvCountOut,
    const aclTensor *expandScalesOut,
    const aclTensor *dispatchWaitRecvCostStatsOutOptional,
    uint64_t *workspaceSize,
    aclOpExecutor **executor)
{
    return aclnnInnerDispatchNormalA5GetWorkspaceSize(
        x,
        expertIds,
        scalesOptional,
        xActiveMaskOptional,
        expertScalesOptional,
        tokenMeshIdxOptional,
        tokenUniquePerMeshOptional,
        epRankTokenCntOptional,
        srcOffsetRankTokenIdxOptional,
        dstOffsetRankTokenIdxOptional,
        groupEp,
        epWorldSize,
        epRankId,
        moeExpertNum,
        groupTpOptional,
        tpWorldSize,
        tpRankId,
        expertShardType,
        sharedExpertNum,
        sharedExpertRankNum,
        quantMode,
        globalBs,
        expertTokenNumsType,
        recvXOut,
        dynamicScalesOut,
        expandIdxOut,
        expertTokenNumsOut,
        epRecvCountOut,
        expandScalesOut,
        dispatchWaitRecvCostStatsOutOptional,
        workspaceSize,
        executor);
}

aclnnStatus aclnnDispatchNormalA5(
    void *workspace,
    uint64_t workspaceSize,
    aclOpExecutor *executor,
    aclrtStream stream)
{
    if (NnopbaseSetHcclServerType) {
        NnopbaseSetHcclServerType(executor, NNOPBASE_HCCL_SERVER_TYPE_AICPU);
    }
    return aclnnInnerDispatchNormalA5(
        workspace,
        workspaceSize,
        executor,
        stream);
}

#ifdef __cplusplus
}
#endif
