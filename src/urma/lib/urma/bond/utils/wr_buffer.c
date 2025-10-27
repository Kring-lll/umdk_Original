/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: Bonding provider WR buffer implementation
 * Author: Ma Chuan
 * Create: 2025-02-21
 * Note:
 * History: 2025-02-21
 */
#include <stdlib.h>
#include "urma_log.h"
#include "wr_utils.h"
#include "wr_buffer.h"

static wr_buf_t *wr_buf_new(size_t hash_map_size)
{
    wr_buf_t *buf = (wr_buf_t *)calloc(1, sizeof(wr_buf_t));
    if (buf == NULL) {
        return NULL;
    }
    if (wr_buf_hmap_create(&buf->map, hash_map_size)) {
        free(buf);
        return NULL;
    }
    return buf;
}

static void wr_buf_delete(wr_buf_t *buf, wr_buf_hmap_delete_wr_func_t delete_func)
{
    wr_buf_hmap_traverse_and_remove(&buf->map, delete_func);
    wr_buf_hmap_destroy(&buf->map);
    free(buf);
}

static int wr_buf_add_wr(wr_buf_t *buf, uint32_t id, void *wr, wr_buf_extra_value_t *value)
{
    return wr_buf_hmap_insert(&buf->map, id, wr, value);
}

static inline wr_buf_node_t *wr_buf_get_node(wr_buf_t *buf, uint32_t id)
{
    return wr_buf_hmap_get(&buf->map, id);
}

static void *wr_buf_get_wr(wr_buf_t *buf, uint32_t id)
{
    wr_buf_node_t *s = NULL;
    s = wr_buf_hmap_get(&buf->map, id);
    if (s == NULL) {
        return NULL;
    }
    return s->wr;
}

static int wr_buf_get_user_ctx(wr_buf_t *buf, uint32_t id, uint64_t *user_ctx)
{
    wr_buf_node_t *s = NULL;
    s = wr_buf_hmap_get(&buf->map, id);
    if (s == NULL) {
        return -1;
    }
    *user_ctx = s->value.user_ctx;
    return 0;
}

static int wr_buf_remove_wr(wr_buf_t *buf, uint32_t id, wr_buf_hmap_delete_wr_func_t delete_func)
{
    wr_buf_node_t *s = NULL;
    s = wr_buf_hmap_move_out(&buf->map, id);
    if (s == NULL) {
        URMA_LOG_WARN("id %u doesn't exist\n", id);
        return -1;
    }
    delete_func(s->wr);
    free(s);
    return 0;
}

static void *wr_buf_move_out_node(wr_buf_t *buf, uint32_t id)
{
    wr_buf_node_t *s = NULL;
    s = wr_buf_hmap_move_out(&buf->map, id);
    if (s == NULL) {
        URMA_LOG_WARN("id %u doesn't exist\n", id);
        return NULL;
    }
    return s;
}

bool wr_buf_contain(wr_buf_t *buf, uint32_t id)
{
    return wr_buf_hmap_get(&buf->map, id) != NULL;
}

int wr_buf_count(wr_buf_t *buf)
{
    return wr_buf_hmap_count(&buf->map);
}

void wr_buf_traverse_and_remove(wr_buf_t *buf, wr_buf_hmap_visit_node_func_t func, void *args)
{
    wr_buf_hmap_traverse_and_remove_with_args(&buf->map, func, args);
}

jfs_wr_buf_t *jfs_wr_buf_new(size_t size)
{
    return wr_buf_new(size);
}

void jfs_wr_buf_delete(jfs_wr_buf_t *buf)
{
    wr_buf_delete(buf, (wr_buf_hmap_delete_wr_func_t)delete_copied_jfs_wr);
}

int jfs_wr_buf_add_wr(jfs_wr_buf_t *buf, uint32_t id, urma_jfs_wr_t *wr, wr_buf_extra_value_t *value)
{
    return wr_buf_add_wr(buf, id, wr, value);
}

urma_jfs_wr_t *jfs_wr_buf_get_wr(jfs_wr_buf_t *buf, uint32_t id)
{
    return wr_buf_get_wr(buf, id);
}

int jfs_wr_buf_get_user_ctx(jfs_wr_buf_t *buf, uint32_t id, uint64_t *user_ctx)
{
    return wr_buf_get_user_ctx(buf, id, user_ctx);
}

wr_buf_node_t *jfs_wr_buf_get_node(jfs_wr_buf_t *buf, uint32_t id)
{
    return wr_buf_get_node(buf, id);
}

int jfs_wr_buf_remove_wr(jfs_wr_buf_t *buf, uint32_t id)
{
    return wr_buf_remove_wr(buf, id, (wr_buf_hmap_delete_wr_func_t)delete_copied_jfs_wr);
}

urma_jfs_wr_t *jfs_wr_buf_move_out_wr(jfs_wr_buf_t *buf, uint32_t id)
{
    wr_buf_node_t *s = wr_buf_move_out_node(buf, id);
    urma_jfs_wr_t *wr;
    if (s == NULL) {
        return NULL;
    }
    wr = s->jfs_wr;
    free(s);
    return wr;
}

jfr_wr_buf_t *jfr_wr_buf_new(size_t size)
{
    return wr_buf_new(size);
}

void jfr_wr_buf_delete(jfr_wr_buf_t *buf)
{
    wr_buf_delete(buf, (wr_buf_hmap_delete_wr_func_t)delete_copied_jfr_wr);
}

int jfr_wr_buf_add_wr(jfr_wr_buf_t *buf, uint32_t id, urma_jfr_wr_t *wr, wr_buf_extra_value_t *value)
{
    return wr_buf_add_wr(buf, id, wr, value);
}

urma_jfr_wr_t *jfr_wr_buf_get_wr(jfr_wr_buf_t *buf, uint32_t id)
{
    return wr_buf_get_wr(buf, id);
}

int jfr_wr_buf_get_user_ctx(jfr_wr_buf_t *buf, uint32_t id, uint64_t *user_ctx)
{
    return wr_buf_get_user_ctx(buf, id, user_ctx);
}

int jfr_wr_buf_remove_wr(jfr_wr_buf_t *buf, uint32_t id)
{
    return wr_buf_remove_wr(buf, id, (wr_buf_hmap_delete_wr_func_t)delete_copied_jfr_wr);
}

urma_jfr_wr_t *jfr_wr_buf_move_out_wr(jfr_wr_buf_t *buf, uint32_t id)
{
    wr_buf_node_t *s = wr_buf_move_out_node(buf, id);
    urma_jfr_wr_t *wr;
    if (s == NULL) {
        return NULL;
    }
    wr = s->jfr_wr;
    free(s);
    return wr;
}