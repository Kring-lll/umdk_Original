/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: util id allocator.
 * Create: 2025-9-12
 */

#include <stdint.h>
#include <pthread.h>
#include <stdlib.h>
#include <errno.h>
#include "util_id_generator.h"

int util_id_allocator_init(util_id_allocator_t *id_allocator, uint32_t max_num, uint32_t start_id)
{
    if (id_allocator->available_ids != NULL) {
        return -1;
    }

    id_allocator->available_ids = (uint32_t *)calloc(max_num, sizeof(uint32_t));
    if (id_allocator->available_ids == NULL) {
        return -1;
    }

    id_allocator->num_available = 0;
    id_allocator->next_id = start_id;
    id_allocator->max_num = max_num;
    id_allocator->lock = util_mutex_lock_create(UTIL_MUTEX_ATTR_EXCLUSIVE);
    if (id_allocator->lock == NULL) {
        free(id_allocator->available_ids);
        id_allocator->available_ids = NULL;
        return -ENOMEM;
    }
    return 0;
}

void util_id_allocator_uninit(util_id_allocator_t *id_allocator)
{
    (void)util_mutex_lock_destroy(id_allocator->lock);
    id_allocator->lock = NULL;
    free(id_allocator->available_ids);
    id_allocator->available_ids = NULL;
}

uint32_t util_id_allocator_get(util_id_allocator_t *id_allocator)
{
    uint32_t id;
    (void)util_mutex_lock(id_allocator->lock);
    if (id_allocator->num_available > 0) {
        id = id_allocator->available_ids[--id_allocator->num_available];
    } else {
        id = id_allocator->next_id++;
        if (id >= id_allocator->max_num) {
            id_allocator->next_id--;
        }
    }
    (void)util_mutex_unlock(id_allocator->lock);

    return id;
}

void util_id_allocator_release(util_id_allocator_t *id_allocator, uint32_t util_id)
{
    if (id_allocator->available_ids == NULL) {
        return;
    }
    (void)util_mutex_lock(id_allocator->lock);
    if (id_allocator->num_available < id_allocator->max_num) {
        id_allocator->available_ids[id_allocator->num_available++] = util_id;
    }
    (void)util_mutex_unlock(id_allocator->lock);
    return;
}

int util_id_allocator_autoinc_init(util_id_allocator_autoinc_t *id_allocator, uint32_t max_num, uint32_t start_id)
{
    if (id_allocator->id_bitmap != NULL) {
        return -1;
    }

    if (max_num == 0 || start_id >= UINT32_MAX - max_num + 1) {
        return -1;
    }

    uint32_t bitmap_size = (max_num + 31) / 32;
    id_allocator->id_bitmap = (uint32_t *)calloc(bitmap_size, sizeof(uint32_t));
    if (id_allocator->id_bitmap == NULL) {
        return -ENOMEM;
    }

    id_allocator->max_num = max_num;
    id_allocator->start_id = start_id;
    id_allocator->cur_id = start_id;
    id_allocator->lock = util_mutex_lock_create(UTIL_MUTEX_ATTR_EXCLUSIVE);
    if (id_allocator->lock == NULL) {
        free(id_allocator->id_bitmap);
        id_allocator->id_bitmap = NULL;
        return -ENOMEM;
    }

    return 0;
}

void util_id_allocator_autoinc_uninit(util_id_allocator_autoinc_t *id_allocator)
{
    if (id_allocator->id_bitmap == NULL) {
        return;
    }

    (void)util_mutex_lock_destroy(id_allocator->lock);
    id_allocator->lock = NULL;
    free(id_allocator->id_bitmap);
    id_allocator->id_bitmap = NULL;
    id_allocator->max_num = 0;
    id_allocator->start_id = 0;
    id_allocator->cur_id = 0;
}

int util_id_allocator_autoinc_get(util_id_allocator_autoinc_t *id_allocator)
{
    int id = -1;
    uint32_t bitmap_size = (id_allocator->max_num + 31) / 32;
    uint32_t start = id_allocator->cur_id;
    uint32_t end = start + id_allocator->max_num;

    (void)util_mutex_lock(id_allocator->lock);

    for (uint32_t i = start; i < end; i++) {
        uint32_t real_id = i % id_allocator->max_num + id_allocator->start_id;
        uint32_t index = (real_id - id_allocator->start_id) / 32;
        uint32_t bit = (real_id - id_allocator->start_id) % 32;

        if (index >= bitmap_size) {
            continue;
        }

        if ((id_allocator->id_bitmap[index] & (1U << bit)) == 0) {
            id_allocator->id_bitmap[index] |= (1U << bit);
            id = real_id;
            id_allocator->cur_id = real_id + 1;
            break;
        }
    }

    (void)util_mutex_unlock(id_allocator->lock);

    return id;
}

void util_id_allocator_autoinc_release(util_id_allocator_autoinc_t *id_allocator, uint32_t id)
{
    if (id_allocator->id_bitmap == NULL) {
        return;
    }

    if (id < id_allocator->start_id || id >= id_allocator->start_id + id_allocator->max_num) {
        return;
    }

    uint32_t index = (id - id_allocator->start_id) / 32;
    uint32_t bit = (id - id_allocator->start_id) % 32;
    uint32_t bitmap_size = (id_allocator->max_num + 31) / 32;

    if (index >= bitmap_size) {
        return;
    }

    (void)util_mutex_lock(id_allocator->lock);
    id_allocator->id_bitmap[index] &= ~(1U << bit);
    (void)util_mutex_unlock(id_allocator->lock);
}