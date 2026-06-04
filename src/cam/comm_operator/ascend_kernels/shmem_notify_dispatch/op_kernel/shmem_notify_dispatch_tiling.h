/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: ShmemNotifyDispatch tiling data structure
 * Create: 2026-06-09
 * Note:
 * History: 2026-06-09 create ShmemNotifyDispatch tiling data structure file
 */
 
#ifndef SHMEM_NOTIFY_DISPATCH_TILING_H
#define SHMEM_NOTIFY_DISPATCH_TILING_H

#include "kernel_tiling/kernel_tiling.h"

struct ShmemNotifyDispatchInfo {
    uint32_t rankSize;
    uint32_t rankId;
    uint32_t localRankSize;
    uint32_t localRankId;
    uint32_t sendCount;
    uint32_t topkNum;
    uint32_t aivNum;
    uint64_t totalUbSize;
};

struct ShmemNotifyDispatchTilingData {
    ShmemNotifyDispatchInfo notifyDispatchInfo;
    uint64_t shmemPtr;  // shmem symmetric point
};

#endif
