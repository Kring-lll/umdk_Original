/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: add fused deep moe w4a8 interface cpp file.
 * Create: 2026-06-27
 * Note:
 * History: 2026-06-27 add fused deep moe w4a8 interface cpp file.
 */

#include <string.h>
#include <vector>
#include "graph/types.h"
#include "aclnn/opdev/platform.h"
#include "aclnn/opdev/make_op_executor.h"
#include "aclnnInner_fused_deep_moe.h"
#include "aclnn_fused_deep_moe.h"

namespace {
static constexpr int32_t NNOPBASE_HCCL_SERVER_TYPE_AICPU = 0;
static constexpr int32_t NNOPBASE_HCCL_SERVER_TYPE_MTE = 1;
} // namespace
extern "C" void __attribute__((weak)) NnopbaseSetHcclServerType(void *executor, int32_t sType);

#ifdef __cplusplus
extern "C" {
#endif

static aclTensorList *ConvertTensorListToInt4(const aclTensorList *input, aclOpExecutor *executor)
{
    if (input == nullptr || executor == nullptr) {
        return nullptr;
    }
    constexpr int64_t INT4_NUMS_IN_INT32 = 8;
    std::vector<aclTensor *> tensors;
    for (int i = 0; i < input->Size(); i++) {
        auto tensor = (*input)[i];
        auto viewShape = tensor->GetViewShape();
        viewShape[viewShape.GetDimNum() - 1] = viewShape[viewShape.GetDimNum() - 1] * INT4_NUMS_IN_INT32;
        auto inputTemp = executor->CreateView(tensor, viewShape, tensor->GetViewOffset());
        if (inputTemp == nullptr) {
            return nullptr;
        }
        inputTemp->SetDataType(DataType::DT_INT4);
        inputTemp->SetStorageFormat(Format::FORMAT_FRACTAL_NZ);
        inputTemp->SetViewFormat(Format::FORMAT_FRACTAL_NZ);
        tensors.push_back(inputTemp);
    }
    return executor->AllocTensorList(tensors.data(), tensors.size());
}

static aclTensor *ConvertTensorToInt4(const aclTensor *input, aclOpExecutor *executor)
{
    if (input == nullptr || executor == nullptr) {
        return nullptr;
    }
    constexpr int64_t INT4_NUMS_IN_INT32 = 8;
    auto viewShape = input->GetViewShape();
    viewShape[viewShape.GetDimNum() - 1] = viewShape[viewShape.GetDimNum() - 1] * INT4_NUMS_IN_INT32;
    auto inputTemp = executor->CreateView(input, viewShape, input->GetViewOffset());
    if (inputTemp == nullptr) {
        return nullptr;
    }
    inputTemp->SetDataType(DataType::DT_INT4);
    inputTemp->SetStorageFormat(Format::FORMAT_FRACTAL_NZ);
    inputTemp->SetViewFormat(Format::FORMAT_FRACTAL_NZ);
    return inputTemp;
}

aclnnStatus aclnnFusedDeepMoeGetWorkspaceSize(
    const aclTensor *x,
    const aclTensor *expertIds,
    const aclTensorList *gmm1Weight,
    const aclTensorList *gmm1WeightScale,
    const aclTensorList *gmm2Weight,
    const aclTensorList *gmm2WeightScale,
    const aclTensor *expertScales,
    const aclTensor *shareGmm1WeightOptional,
    const aclTensor *shareGmm1WeightScaleOptional,
    const aclTensor *shareGmm2WeightOptional,
    const aclTensor *shareGmm2WeightScaleOptional,
    const aclTensor *expertSmoothScalesOptional,
    const aclTensor *shareSmoothScalesOptional,
    const aclTensor *xActiveMaskOptional,
    const aclTensorList *gmm1BiasOptional,
    const aclTensorList *gmm2BiasOptional,
    const aclTensor *shareGmm1BiasOptional,
    const aclTensor *shareGmm2BiasOptional,
    char *groupEp,
    int64_t epRankSize,
    int64_t epRankId,
    int64_t moeExpertNum,
    int64_t quantMode,
    int64_t globalBs,
    const aclTensor *output,
    const aclTensor *shareOutput,
    const aclTensor *expertTokenNums,
    uint64_t *workspaceSize,
    aclOpExecutor **executor)
{
    if (*executor == nullptr) {
        auto uniqueExec = CREATE_EXECUTOR();
        uniqueExec.ReleaseTo(executor);
    }

    if (gmm1Weight != nullptr && gmm1Weight->Size() > 0 &&
        (*gmm1Weight)[0]->GetDataType() == DataType::DT_INT32) {
        gmm1Weight = ConvertTensorListToInt4(gmm1Weight, *executor);
    }
    if (gmm2Weight != nullptr && gmm2Weight->Size() > 0 &&
        (*gmm2Weight)[0]->GetDataType() == DataType::DT_INT32) {
        gmm2Weight = ConvertTensorListToInt4(gmm2Weight, *executor);
    }
    if (shareGmm1WeightOptional != nullptr &&
        shareGmm1WeightOptional->GetDataType() == DataType::DT_INT32) {
        shareGmm1WeightOptional = ConvertTensorToInt4(shareGmm1WeightOptional, *executor);
    }
    if (shareGmm2WeightOptional != nullptr &&
        shareGmm2WeightOptional->GetDataType() == DataType::DT_INT32) {
        shareGmm2WeightOptional = ConvertTensorToInt4(shareGmm2WeightOptional, *executor);
    }

    return aclnnInnerFusedDeepMoeGetWorkspaceSize(x, expertIds, gmm1Weight, gmm1WeightScale,
        gmm2Weight, gmm2WeightScale, expertScales,
        shareGmm1WeightOptional, shareGmm1WeightScaleOptional,
        shareGmm2WeightOptional, shareGmm2WeightScaleOptional,
        expertSmoothScalesOptional, shareSmoothScalesOptional, xActiveMaskOptional,
        gmm1BiasOptional, gmm2BiasOptional,
        shareGmm1BiasOptional, shareGmm2BiasOptional,
        groupEp, epRankSize, epRankId, moeExpertNum, quantMode, globalBs,
        output, shareOutput, expertTokenNums, workspaceSize, executor);
}

aclnnStatus aclnnFusedDeepMoe(
    void *workspace,
    uint64_t workspaceSize,
    aclOpExecutor *executor,
    aclrtStream stream)
{
    if (NnopbaseSetHcclServerType) {
        if (op::GetCurrentPlatformInfo().GetSocVersion() == op::SocVersion::ASCEND910B) {
            NnopbaseSetHcclServerType(executor, NNOPBASE_HCCL_SERVER_TYPE_AICPU);
        } else {
            NnopbaseSetHcclServerType(executor, NNOPBASE_HCCL_SERVER_TYPE_MTE);
        }
    }
    return aclnnInnerFusedDeepMoe(workspace, workspaceSize, executor, stream);
}

#ifdef __cplusplus
}
#endif