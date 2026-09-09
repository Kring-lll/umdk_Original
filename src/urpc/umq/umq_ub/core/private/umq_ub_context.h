/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Validation of serialized URMA context extension lengths.
 */
#ifndef UMQ_UB_CONTEXT_H
#define UMQ_UB_CONTEXT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static inline bool umq_ub_user_info_valid(const void *blob, uint32_t length,
                                         uint32_t base_len, bool has_user_info)
{
    uint32_t user_info_len;

    if (blob == NULL || length < base_len) {
        return false;
    }
    if (!has_user_info) {
        return true;
    }
    /* URMA serializes a native uint32_t length followed by provider user data. */
    if (length - base_len < sizeof(user_info_len)) {
        return false;
    }
    (void)memcpy(&user_info_len, (const char *)blob + base_len, sizeof(user_info_len));
    return user_info_len <= length - base_len - sizeof(user_info_len);
}

#endif // UMQ_UB_CONTEXT_H
