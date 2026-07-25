/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include <string.h>
#include "graph/types.h"
#include "aclnn/opdev/platform.h"
#include "aclnnInner_swiglu_clip_quant.h"
#include "aclnn_swiglu_clip_quant.h"

namespace {
static constexpr int32_t NNOPBASE_HCCL_SERVER_TYPE_AICPU = 0;
static constexpr int32_t NNOPBASE_HCCL_SERVER_TYPE_MTE = 1;
static constexpr int32_t NNOPBASE_HCCL_SERVER_TYPE_END = 2;
} // namespace
extern "C" void __attribute__((weak)) NnopbaseSetHcclServerType(void *executor, int32_t sType);

#ifdef __cplusplus
extern "C" {
#endif

aclnnStatus aclnnSwigluClipQuantGetWorkspaceSize(
    const aclTensor *x,
    const aclTensor *groupIndex,
    const aclTensor *groupAlpha,
    bool activateLeft,
    char *quantModeOptional,
    int64_t clampMode,
    const aclTensor *yOut,
    const aclTensor *scaleOut,
    uint64_t *workspaceSize,
    aclOpExecutor **executor)
{
    return aclnnInnerSwigluClipQuantGetWorkspaceSize(x, groupIndex, groupAlpha, activateLeft, quantModeOptional,
        clampMode, yOut, scaleOut, workspaceSize, executor);
}

aclnnStatus aclnnSwigluClipQuant(
    void *workspace,
    uint64_t workspaceSize,
    aclOpExecutor *executor,
    aclrtStream stream)
{
    if (NnopbaseSetHcclServerType) {
        NnopbaseSetHcclServerType(executor, NNOPBASE_HCCL_SERVER_TYPE_MTE);
    }
    return aclnnInnerSwigluClipQuant(workspace, workspaceSize, executor, stream);
}

#ifdef __cplusplus
}
#endif