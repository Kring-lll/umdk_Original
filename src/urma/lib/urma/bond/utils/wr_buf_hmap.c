/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: Bonding provider WR buffer hmap implementation. WR buffer depends on this hmap abstraction.
 * Author: Ma Chuan
 * Create: 2025-02-21
 * Note:
 * History: 2025-02-21
 */
#include "ub_hmap.h"
#include "ub_util.h"
#include "bondp_hash_table.h"
#include "wr_buf_hmap.h"

int wr_buf_hmap_create(wr_buf_hmap_t *map, size_t size)
{
    return ub_hmap_init(map, (uint32_t)size);
}

void wr_buf_hmap_destroy(wr_buf_hmap_t *map)
{
    ub_hmap_destroy(map);
}

wr_buf_node_t *wr_buf_hmap_get(wr_buf_hmap_t *map, uint32_t key)
{
    hmap_node_t *node = NULL;
    node = ub_hmap_first_with_hash(map, key);
    if (node == NULL) {
        return NULL;
    }
    return CONTAINER_OF_FIELD(node, wr_buf_node_t, hh);
}

int wr_buf_hmap_insert(wr_buf_hmap_t *map, uint32_t key, void *wr, wr_buf_extra_value_t *value)
{
    wr_buf_node_t *node;

    // check if the key exists
    // fail if the key already exists
    node = wr_buf_hmap_get(map, key);
    if (node != NULL) {
        return -1;
    }
    node = (wr_buf_node_t *)malloc(sizeof(wr_buf_node_t));
    node->key = key;
    node->wr = wr;
    if (value != NULL) {
        node->value = *value;
    }
    // insert new node into the map
    ub_hmap_insert(map, &node->hh, key);
    return 0;
}

int wr_buf_hmap_remove(wr_buf_hmap_t *map, uint32_t key)
{
    wr_buf_node_t *node;

    node = wr_buf_hmap_get(map, key);
    if (node == NULL) {
        return -1;
    }
    ub_hmap_remove(map, &node->hh);
    free(node);
    return 0;
}

wr_buf_node_t *wr_buf_hmap_move_out(wr_buf_hmap_t *map, uint32_t key)
{
    wr_buf_node_t *node;

    node = wr_buf_hmap_get(map, key);
    if (node == NULL) {
        return NULL;
    }
    ub_hmap_remove(map, &node->hh);
    return node;
}

void wr_buf_hmap_traverse_and_remove(wr_buf_hmap_t *map, int (*func)(void *))
{
    wr_buf_node_t *current;
    wr_buf_node_t *next;

    // iterate the map
    HMAP_FOR_EACH_SAFE(current, next, hh, map) {
        if (func(current->wr) != WR_BUF_HMAP_FUNC_SUCCESS) {
            break;
        }
        ub_hmap_remove(map, &current->hh);
        free(current);
    }
}

void wr_buf_hmap_traverse_and_remove_with_args(wr_buf_hmap_t *map,
    int (*func)(wr_buf_node_t *node, void *args), void *args)
{
    wr_buf_node_t *current;
    wr_buf_node_t *next;
    int ret = 0;

    // iterate the map
    HMAP_FOR_EACH_SAFE(current, next, hh, map) {
        ret = func(current, args);
        if (ret == WR_BUF_HMAP_FUNC_CONTINUE) {
            continue;
        } else if (ret != WR_BUF_HMAP_FUNC_SUCCESS) {
            break;
        }
        ub_hmap_remove(map, &current->hh);
        free(current);
    }
}

uint32_t wr_buf_hmap_count(wr_buf_hmap_t *map)
{
    return map->count;
}