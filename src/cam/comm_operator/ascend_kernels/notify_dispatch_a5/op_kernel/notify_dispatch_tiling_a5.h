/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: notify dispatch kernel A5 part (multi-mesh layered support)
 * Create: 2026-05-26
 * Note:
 * History: 2026-05-26 create notify dispatch A5 kernel part
 */

#ifndef NOTIFY_DISPATCH_TILING_A5_H
#define NOTIFY_DISPATCH_TILING_A5_H

#include <cstdint>
#include "kernel_tiling/kernel_tiling.h"

namespace Cam {
struct NotifyDispatchInfoA5 {
    uint32_t rankSize;
    uint32_t rankId;
    uint32_t localRankSize;
    uint32_t localRankId;
    uint32_t sendCount;
    uint32_t numTokens;
    uint32_t topkNum;
    uint32_t numExperts;
    uint32_t aivNum;
    uint64_t totalUbSize;
};

struct NotifyDispatchA5TilingData {
    Mc2InitTiling mc2InitTiling;
    Mc2CcTiling mc2CcTiling1;
    NotifyDispatchInfoA5 notifyDispatchInfoA5;
};
} // namespace Cam
#endif
