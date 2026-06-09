
/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: add shmem notify dispatch interface header file.
 * Create: 2026-06-09
 * Note:
 * History: 2026-06-09 add shmem notify dispatch interface header file.
 */
 
#ifndef ACLNN_SHMEM_NOTIFY_DISPATCH_H_
#define ACLNN_SHMEM_NOTIFY_DISPATCH_H_

#include "aclnn/acl_meta.h"

#ifdef __cplusplus
extern "C" {
#endif

/* function: aclnnShmemNotifyDispatchGetWorkspaceSize
 * parameters:
 *  tokenPerExpertData     - input
 *  sendCount              - input
 *  rankSize               - input
 *  rankId                 - input
 *  localRankSize          - input
 *  localRankId            - input
 *  topkNum                - input
 *  shmemPtr               - input
 *  recvData               - input
 *  totalRecvTokens        - input
 *  maxBs                  - input
 *  recvTokensPerExpert    - input
 *  putOffset              - input
 *  workspaceSize          - output, size of workspace
 *  executor               - output, executor context
 */
__attribute__((visibility("default"))) aclnnStatus aclnnShmemNotifyDispatchGetWorkspaceSize(
    const aclTensor *tokenPerExpertData, int64_t sendCount, int64_t rankSize, int64_t rankId, int64_t localRankSize,
    int64_t localRankId, int64_t topkNum, uint64_t shmemPtr, const aclTensor *recvData,
    const aclTensor *totalRecvTokens, const aclTensor *maxBs, const aclTensor *recvTokensPerExpert,
    const aclTensor *putOffset, uint64_t *workspaceSize, aclOpExecutor **executor);

/* function: aclnnShmemNotifyDispatch
 * parameters:
 *  workspace     - input, workspace memory address
 *  workspaceSize - input, size of workspace
 *  executor      - input, executor context
 *  stream        - input, acl stream
 */
__attribute__((visibility("default"))) aclnnStatus aclnnShmemNotifyDispatch(void *workspace, uint64_t workspaceSize,
                                                                            aclOpExecutor *executor,
                                                                            aclrtStream stream);

#ifdef __cplusplus
}
#endif

#endif
