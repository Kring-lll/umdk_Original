/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: add fused deep moe interface cpp file.
 * Create: 2025-07-21
 * Note:
 * History: 2025-07-21 add fused deep moe interface cpp file.
 */

#include <string.h>
#include "graph/types.h"
#include "aclnn/opdev/platform.h"
#include "aclnnInner_fused_deep_moe_fwk.h"
#include "aclnn_fused_deep_moe_fwk.h"

namespace {
static constexpr int32_t NNOPBASE_HCCL_SERVER_TYPE_AICPU = 0;
static constexpr int32_t NNOPBASE_HCCL_SERVER_TYPE_MTE = 1;
static constexpr int32_t NNOPBASE_HCCL_SERVER_TYPE_END = 2;
} // namespace
extern "C" void __attribute__((weak)) NnopbaseSetHcclServerType(void *executor, int32_t sType);

#ifdef __cplusplus
extern "C" {
#endif

aclnnStatus aclnnFusedDeepMoeFwkGetWorkspaceSize(
    const aclTensor *x,
    const aclTensor *expertIds,
    const aclTensorList *gmm1Weight,
    const aclTensorList *gmm1WeightScale,
    const aclTensorList *gmm2Weight,
    const aclTensorList *gmm2WeightScale,
    const aclTensor *expertSmoothScalesOptional,
    const aclTensor *expertScalesOptional,
    char *groupEp,
    int64_t epRankSize,
    int64_t epRankId,
    int64_t moeExpertNum,
    int64_t shareExpertNum,
    int64_t shareExpertRankNum,
    int64_t quantMode,
    int64_t globalBs,
    const aclTensor *output,
    const aclTensor *expertTokenNums,
    uint64_t *workspaceSize,
    aclOpExecutor **executor)
{
    return aclnnInnerFusedDeepMoeFwkGetWorkspaceSize(
        x,
        expertIds,
        gmm1Weight,
        gmm1WeightScale,
        gmm2Weight,
        gmm2WeightScale,
        expertSmoothScalesOptional,
        expertScalesOptional,
        groupEp,
        epRankSize,
        epRankId,
        moeExpertNum,
        shareExpertNum,
        shareExpertRankNum,
        quantMode,
        globalBs,
        output,
        expertTokenNums,
        workspaceSize,
        executor);
}

aclnnStatus aclnnFusedDeepMoeFwk(
    void *workspace,
    uint64_t workspaceSize,
    aclOpExecutor *executor,
    aclrtStream stream)
{
    if (NnopbaseSetHcclServerType) {
        if (op::GetCurrentPlatformInfo().GetSocVersion() == op::SocVersion::ASCEND910B) {
            NnopbaseSetHcclServerType(executor, NNOPBASE_HCCL_SERVER_TYPE_MTE);
        } else {
            NnopbaseSetHcclServerType(executor, NNOPBASE_HCCL_SERVER_TYPE_MTE);
        }
    }
    return aclnnInnerFusedDeepMoeFwk(workspace, workspaceSize, executor, stream);
}

#ifdef __cplusplus
}
#endif