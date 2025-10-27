/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: Bond dev bitmap id store implementation
 * Author: Ma Chuan
 * Create: 2025-02-20
 * Note:
 * History: 2025-02-20 Create File
 */
#include <stdlib.h>
#include <stdio.h>
#include "urma_log.h"
#include "bondp_bitmap_id_store.h"

int bondp_id_store_init(bondp_id_store_t *store, int size)
{
    if (store == NULL) {
        return -1;
    }
    // The range of idx is [1, 1+size), we need an extra space to store pointers.
    store->elems = calloc(size + 1, sizeof(void *));
    if (store->elems == NULL) {
        return -1;
    }
    return 0;
}

void bondp_id_store_uninit(bondp_id_store_t *store)
{
    if (store == NULL) {
        return;
    }
    free(store->elems);
    store->elems = NULL;
}

void bondp_id_store_save(bondp_id_store_t store, void *elem, uint32_t id)
{
    store.elems[id] = elem;
}

void bondp_id_store_remove(bondp_id_store_t store, uint32_t id)
{
    store.elems[id] = NULL;
}

void *bondp_id_store_lookup(bondp_id_store_t store, uint32_t id)
{
    return store.elems[id];
}