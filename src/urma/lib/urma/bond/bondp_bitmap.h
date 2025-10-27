/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: Bond Bitmap header
 * Author: Ma Chuan
 * Create: 2025-02-18
 * Note:
 * History: 2025-02-18   Create File
 */
#ifndef BONDP_BITMAP_H
#define BONDP_BITMAP_H
#include <stdbool.h>
#include <inttypes.h>
// Currently not multi-thread safe
typedef struct bondp_bitmap {
    unsigned long *bits;
    uint32_t size;
} bondp_bitmap_t;
// -- All apis of bondp_bitmap need `Caller` to check non-null bitmap pointer --
bondp_bitmap_t *bondp_bitmap_alloc(uint32_t bitmap_size);
void bondp_bitmap_free(bondp_bitmap_t *bitmap);
int bondp_bitmap_init(bondp_bitmap_t *bitmap, uint32_t size);
void bondp_bitmap_uninit(bondp_bitmap_t *bitmap);
int bondp_bitmap_alloc_idx(bondp_bitmap_t *bitmap, uint32_t *idx_out);
int bondp_bitmap_alloc_idx_from_offset(bondp_bitmap_t *bitmap, uint32_t offset, uint32_t *idx_out);
int bondp_bitmap_free_idx(bondp_bitmap_t *bitmap, uint32_t idx);
int bondp_bitmap_use_id(bondp_bitmap_t *bitmap, uint32_t id);
bool bondp_bitmap_is_set(bondp_bitmap_t *bitmap, uint32_t id);
#endif // BONDP_BITMAP_H