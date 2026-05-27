/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: util id allocator.
 * Create: 2025-9-12
 */

#ifndef UTIL_ID_GENERATOR_H
#define UTIL_ID_GENERATOR_H

#include "util_lock.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct util_id_allocator {
    uint32_t *available_ids;
    uint32_t num_available;
    uint32_t next_id;
    uint32_t max_num;
    util_external_mutex_lock *lock;
} util_id_allocator_t;

typedef struct util_id_allocator_autoinc {
    uint32_t max_num;
    uint32_t start_id;
    uint32_t cur_id;
    uint32_t *id_bitmap;
    util_external_mutex_lock *lock;
} util_id_allocator_autoinc_t;

int util_id_allocator_init(util_id_allocator_t *id_allocator, uint32_t max_num, uint32_t start_id);
void util_id_allocator_uninit(util_id_allocator_t *id_allocator);
uint32_t util_id_allocator_get(util_id_allocator_t *id_allocator);
void util_id_allocator_release(util_id_allocator_t *id_allocator, uint32_t util_id);

int util_id_allocator_autoinc_init(util_id_allocator_autoinc_t *id_allocator, uint32_t max_num, uint32_t start_id);
void util_id_allocator_autoinc_uninit(util_id_allocator_autoinc_t *id_allocator);
int util_id_allocator_autoinc_get(util_id_allocator_autoinc_t *id_allocator);
void util_id_allocator_autoinc_release(util_id_allocator_autoinc_t *id_allocator, uint32_t id);

#ifdef __cplusplus
}
#endif

#endif