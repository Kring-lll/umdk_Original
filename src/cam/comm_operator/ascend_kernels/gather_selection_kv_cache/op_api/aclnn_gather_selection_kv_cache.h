/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef ACLNN_GATHER_SELECTION_KV_CACHE_H_
#define ACLNN_GATHER_SELECTION_KV_CACHE_H_

#include "aclnn/acl_meta.h"

#ifdef __cplusplus
extern "C" {
#endif

/* funtion: aclnnGatherSelectionKvCacheGetWorkspaceSize
 * parameters :
 * selectionKRopeRef : required
 * selectionKvCacheRef : required
 * selectionKvBlockTableRef : required
 * selectionKvBlockStatusRef : required
 * selectionTopkIndices : required
 * fullKRope : required
 * fullKvCache : required
 * fullKvBlockTable : required
 * fullKvActualSeq : required
 * fullQActualSeq : required
 * selectionTopkBlockSize : optional
 * selectionKRopeRef : required
 * selectionKvCacheRef : required
 * selectionKvBlockTableRef : required
 * selectionKvBlockStatusRef : required
 * selectionKvActualSeqOut : required
 * workspaceSize : size of workspace(output).
 * executor : executor context(output).
 */
__attribute__((visibility("default"))) aclnnStatus aclnnGatherSelectionKvCacheGetWorkspaceSize(
    aclTensor *selectionKRopeRef,
    aclTensor *selectionKvCacheRef,
    aclTensor *selectionKvBlockTableRef,
    aclTensor *selectionKvBlockStatusRef,
    const aclTensor *selectionTopkIndices,
    const aclTensor *fullKRope,
    const aclTensor *fullKvCache,
    const aclTensor *fullKvBlockTable,
    const aclTensor *fullKvActualSeq,
    const aclTensor *fullQActualSeq,
    int64_t selectionTopkBlockSize,
    const aclTensor *selectionKvActualSeqOut,
    uint64_t *workspaceSize,
    aclOpExecutor **executor);

/* funtion: aclnnGatherSelectionKvCache
 * parameters :
 * workspace : workspace memory addr(input).
 * workspaceSize : size of workspace(input).
 * executor : executor context(input).
 * stream : acl stream.
 */
__attribute__((visibility("default"))) aclnnStatus aclnnGatherSelectionKvCache(
    void *workspace,
    uint64_t workspaceSize,
    aclOpExecutor *executor,
    aclrtStream stream);

#ifdef __cplusplus
}
#endif

#endif