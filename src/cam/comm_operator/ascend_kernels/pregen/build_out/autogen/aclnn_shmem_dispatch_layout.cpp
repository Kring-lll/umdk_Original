/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: add shmem dispatch layout interface cpp file.
 * Create: 2026-06-09
 * Note:
 * History: 2026-06-09 add shmem dispatch layout interface cpp file.
 */
 
#include <string.h>
#include "graph/types.h"
#include "aclnn_shmem_dispatch_layout.h"
#include "aclnnInner_shmem_dispatch_layout.h"

enum NnopbaseHcclServerType {
    NNOPBASE_HCCL_SERVER_TYPE_AICPU = 0,
    NNOPBASE_HCCL_SERVER_TYPE_MTE,
    NNOPBASE_HCCL_SERVER_TYPE_END
};
extern "C" void __attribute__((weak)) NnopbaseSetHcclServerType(void *executor, NnopbaseHcclServerType sType);

#ifdef __cplusplus
extern "C" {
#endif

aclnnStatus aclnnShmemDispatchLayoutGetWorkspaceSize(const aclTensor *topkIdx, int64_t numTokens, int64_t numRanks,
                                                    int64_t numExperts, int64_t numTopk, int64_t localRankSize,
                                                    const aclTensor *numTokensPerRank, const aclTensor *numTokensPerExpert,
                                                    const aclTensor *isTokenInRank, const aclTensor *notifySendData,
                                                    const aclTensor *sendTokenIdxSmall, uint64_t *workspaceSize,
                                                    aclOpExecutor **executor)
{
    return aclnnInnerShmemDispatchLayoutGetWorkspaceSize(topkIdx, numTokens, numRanks, numExperts, numTopk, localRankSize,
                                                        numTokensPerRank, numTokensPerExpert, isTokenInRank, notifySendData,
                                                        sendTokenIdxSmall, workspaceSize, executor);
}

aclnnStatus aclnnShmemDispatchLayout(void *workspace, uint64_t workspaceSize, aclOpExecutor *executor, aclrtStream stream)
{
    if (NnopbaseSetHcclServerType) {
        NnopbaseSetHcclServerType(executor, NNOPBASE_HCCL_SERVER_TYPE_MTE);
    }
    return aclnnInnerShmemDispatchLayout(workspace, workspaceSize, executor, stream);
}

#ifdef __cplusplus
}
#endif
