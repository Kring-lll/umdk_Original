/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: dispatch normal A2 host part
 * Create: 2026-01-15
 * Note:
 * History: 2026-01-15 create dispatch normal A2 host part
 */

#ifndef TILING_ARGS_H
#define TILING_ARGS_H
#include <cstdint>

namespace Moe {
constexpr uint64_t COMBINE_STATE_WIN_OFFSET = 3U * 1024UL * 1024UL;
constexpr uint64_t NOTIFY_DISPATCH_WIN_OFFSET = 204U * 1024UL * 1024UL;
}  // namespace Moe
#endif  // TILING_ARGS_H
