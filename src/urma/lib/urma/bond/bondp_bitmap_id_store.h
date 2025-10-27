/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: Bond dev bitmap id store header
 * Author: Ma Chuan
 * Create: 2025-02-20
 * Note:
 * History: 2025-02-20 Create File
 */
#ifndef BONDP_BITMAP_ID_STORE
#define BONDP_BITMAP_ID_STORE
#include "bondp_bitmap.h"
// A hash table that can alloc id with bitmap and store pointers
// Each id refer to one pointer element in the hash table
// The table alloc a unique id for each entry when it stores element
// The table only acts as index, doesn't handle lifetime of elements
typedef struct bondp_bitmap_id_store {
    void **elems;
} bondp_id_store_t;

int bondp_id_store_init(bondp_id_store_t *store, int size);
void bondp_id_store_uninit(bondp_id_store_t *store);
void bondp_id_store_save(bondp_id_store_t store, void *elem, uint32_t id);
void bondp_id_store_remove(bondp_id_store_t store, uint32_t id);
void *bondp_id_store_lookup(bondp_id_store_t store, uint32_t id);
#endif // BONDP_BITMAP_ID_STORE