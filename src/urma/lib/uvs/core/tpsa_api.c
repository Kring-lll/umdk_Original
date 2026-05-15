/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2025. All rights reserved.
 * Description: tpsa api file
 * Author: Zheng Hongqin
 * Create: 2023-11-22
 * Note:
 * History:
 */

#include "tpsa_ioctl.h"
#include "uvs_api.h"
#include "uvs_cmd_tlv.h"
#include "uvs_private_api.h"
#include "uvs_ubagg_ioctl.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>

#define UVS_MAX_TOPO_NUM 64
#define UVS_MAIN_PRIMARY_EID_GROUP_INVALID_INDEX UINT32_MAX
#define UVS_MAIN_PRIMARY_EID_GROUP_BUCKET_FACTOR 2U
#define UVS_MAIN_PRIMARY_EID_GROUP_BUCKET_EXTRA 1U
/* 32-bit FNV-1a constants used only for the temporary node/entity/chip table. */
#define UVS_MAIN_PRIMARY_EID_HASH_OFFSET_BASIS 2166136261U
#define UVS_MAIN_PRIMARY_EID_HASH_PRIME 16777619U

static inline bool uvs_eid_is_valid(const uvs_eid_t *eid)
{
    return !(eid->in6.interface_id == 0 && eid->in6.subnet_prefix == 0);
}

typedef struct uvs_main_primary_eid_group {
    uint32_t node_idx;
    uint32_t entity_id;
    uint32_t chip_id;
    uvs_eid_t main_primary_eid;
} uvs_main_primary_eid_group_t;

typedef struct uvs_main_primary_eid_group_node {
    uvs_main_primary_eid_group_t group;
    uint32_t next;
} uvs_main_primary_eid_group_node_t;

typedef struct uvs_main_primary_eid_group_table {
    uint32_t *buckets;
    uvs_main_primary_eid_group_node_t *nodes;
    uint32_t bucket_num;
    uint32_t node_num;
    uint32_t node_max;
} uvs_main_primary_eid_group_table_t;

typedef struct uvs_main_primary_eid_ue_entry_ctx {
    uvs_main_primary_eid_ue_entry_t *entries;
    uint32_t *entry_num;
    uint32_t entry_max;
    const uvs_main_primary_eid_group_table_t *groups;
} uvs_main_primary_eid_ue_entry_ctx_t;

static inline bool uvs_topo_eid_is_valid(const char *eid)
{
    const char empty_eid[EID_LEN] = {0};

    return eid != NULL && memcmp(eid, empty_eid, EID_LEN) != 0;
}

int uvs_create_agg_dev(uvs_eid_t *agg_eid, const char *dev_name)
{
    int ret = 0;
    size_t dev_name_len;

    if (agg_eid == NULL || dev_name == NULL) {
        TPSA_LOG_ERR("Invalid parameter.\n");
        return -EINVAL;
    }

    dev_name_len = strnlen(dev_name, UVS_MAX_DEV_NAME_LEN);
    if (dev_name_len == 0 || dev_name_len >= UVS_MAX_DEV_NAME_LEN) {
        TPSA_LOG_ERR("Invalid parameter.\n");
        return -EINVAL;
    }

    ret = uvs_ubagg_ioctl_create_agg_dev(agg_eid, dev_name);
    if (ret != 0) {
        TPSA_LOG_ERR("failed to create agg dev in ubagg, ret = %d.\n", ret);
        return ret;
    }

    return ret;
}

int uvs_delete_agg_dev(uvs_eid_t *agg_eid)
{
    int ret = 0;

    if (agg_eid == NULL) {
        TPSA_LOG_ERR("Invalid parameter.\n");
        return -EINVAL;
    }

    ret = uvs_ubagg_ioctl_delete_agg_dev(agg_eid);
    if (ret != 0) {
        TPSA_LOG_ERR("failed to delete agg dev in ubagg, ret = %d.\n", ret);
        return ret;
    }

    return ret;
}

int uvs_get_device_name_by_eid(uvs_eid_t *eid, char *buf, size_t len)
{
    int ret = 0;

    if (buf == NULL || len == 0 || len > UVS_MAX_DEV_NAME_LEN || eid == NULL) {
        TPSA_LOG_ERR("Invalid parameter.\n");
        return -EINVAL;
    }

    ret = uvs_ubagg_ioctl_get_dev_name_by_eid(eid, buf, len);
    if (ret != 0) {
        TPSA_LOG_ERR("failed to get dev name by eid in ubagg, ret = %d.\n", ret);
        return ret;
    }

    return 0;
}

static uint32_t uvs_main_primary_eid_batch_num(uint32_t left)
{
    return left > UVS_MAIN_PRIMARY_EID_BATCH_MAX ? UVS_MAIN_PRIMARY_EID_BATCH_MAX : left;
}

static uint32_t uvs_main_primary_eid_ue_port_mask(void)
{
    return (1U << PORT_NUM) - 1U;
}

static int uvs_main_primary_eid_ue_flat_entry_num(
    const uvs_main_primary_eid_ue_entry_t *entry, uint32_t *flat_entry_num)
{
    uint32_t port_idx;

    if (entry == NULL || flat_entry_num == NULL) {
        return -EINVAL;
    }

    if ((entry->port_bitmap & ~uvs_main_primary_eid_ue_port_mask()) != 0) {
        TPSA_LOG_ERR("Invalid main primary eid ue port bitmap.\n");
        return -EINVAL;
    }

    *flat_entry_num = 1;
    for (port_idx = 0; port_idx < PORT_NUM; port_idx++) {
        if ((entry->port_bitmap & (1U << port_idx)) != 0) {
            (*flat_entry_num)++;
        }
    }

    return 0;
}

static int uvs_main_primary_eid_ue_batch_num(
    const uvs_main_primary_eid_ue_entry_t *entries, uint32_t left,
    uint32_t *batch_num)
{
    uint32_t batch = 0;
    uint32_t flat_batch = 0;
    int ret;

    if (entries == NULL || batch_num == NULL || left == 0) {
        return -EINVAL;
    }

    while (batch < left && batch < UVS_MAIN_PRIMARY_EID_UE_BATCH_MAX) {
        uint32_t flat_entry_num = 0;

        ret = uvs_main_primary_eid_ue_flat_entry_num(&entries[batch],
                                                     &flat_entry_num);
        if (ret != 0) {
            return ret;
        }

        if (flat_entry_num > UVS_MAIN_PRIMARY_EID_BATCH_MAX) {
            TPSA_LOG_ERR("main primary eid ue entry is too large.\n");
            return -E2BIG;
        }

        if (batch != 0 &&
            flat_batch + flat_entry_num > UVS_MAIN_PRIMARY_EID_BATCH_MAX) {
            break;
        }

        flat_batch += flat_entry_num;
        batch++;
    }

    *batch_num = batch;
    return 0;
}

static bool uvs_main_primary_eid_mul_overflow(size_t count, size_t item_size)
{
    return count != 0 && item_size > SIZE_MAX / count;
}

int uvs_insert_main_primary_eid_entry(const uvs_main_primary_eid_entry_t *entry)
{
    int ret;

    if (entry == NULL) {
        TPSA_LOG_ERR("Invalid main primary eid entry.\n");
        return -EINVAL;
    }

    uvs_get_api_rdlock();
    ret = uvs_ubcore_ioctl_insert_main_primary_eid(entry);
    put_uvs_lock();
    return ret;
}

int uvs_delete_main_primary_eid_entry(const uvs_eid_t *prefix, uint8_t prefix_len)
{
    int ret;

    if (prefix == NULL) {
        TPSA_LOG_ERR("Invalid main primary eid prefix.\n");
        return -EINVAL;
    }

    uvs_get_api_rdlock();
    ret = uvs_ubcore_ioctl_delete_main_primary_eid(prefix, prefix_len);
    put_uvs_lock();
    return ret;
}

int uvs_lookup_main_primary_eid(const uvs_eid_t *eid, uvs_eid_t *main_primary_eid)
{
    int ret;

    if (eid == NULL || main_primary_eid == NULL) {
        TPSA_LOG_ERR("Invalid main primary eid lookup param.\n");
        return -EINVAL;
    }

    uvs_get_api_rdlock();
    ret = uvs_ubcore_ioctl_lookup_main_primary_eid(eid, main_primary_eid);
    put_uvs_lock();
    return ret;
}

static int uvs_insert_main_primary_eid_entries_inner(const uvs_main_primary_eid_entry_t *entries,
                                                     uint32_t entry_num)
{
    uint32_t offset = 0;
    int ret;

    if (entry_num != 0 && entries == NULL) {
        TPSA_LOG_ERR("Invalid main primary eid entries.\n");
        return -EINVAL;
    }

    while (offset < entry_num) {
        uint32_t batch_num = uvs_main_primary_eid_batch_num(entry_num - offset);

        ret = uvs_ubcore_ioctl_insert_main_primary_eid_batch(&entries[offset], batch_num);
        if (ret != 0) {
            return ret;
        }
        offset += batch_num;
    }

    return 0;
}

int uvs_insert_main_primary_eid_entries(const uvs_main_primary_eid_entry_t *entries,
                                        uint32_t entry_num)
{
    int ret;

    uvs_get_api_rdlock();
    ret = uvs_insert_main_primary_eid_entries_inner(entries, entry_num);
    put_uvs_lock();
    return ret;
}

static int uvs_insert_main_primary_eid_ue_entries_inner(
    const uvs_main_primary_eid_ue_entry_t *entries, uint32_t entry_num)
{
    uint32_t offset = 0;
    int ret;

    if (entry_num != 0 && entries == NULL) {
        TPSA_LOG_ERR("Invalid main primary eid ue entries.\n");
        return -EINVAL;
    }

    while (offset < entry_num) {
        uint32_t batch_num = 0;

        ret = uvs_main_primary_eid_ue_batch_num(&entries[offset],
                                                entry_num - offset,
                                                &batch_num);
        if (ret != 0) {
            return ret;
        }

        ret = uvs_ubcore_ioctl_insert_main_primary_eid_ue_batch(&entries[offset], batch_num);
        if (ret != 0) {
            return ret;
        }
        offset += batch_num;
    }

    return 0;
}

int uvs_flush_main_primary_eid_entries(void)
{
    int ret;

    uvs_get_api_rdlock();
    ret = uvs_ubcore_ioctl_flush_main_primary_eid();
    put_uvs_lock();
    return ret;
}

static void uvs_count_main_primary_eid_ue(const struct urma_topo_ue *ue,
                                          uint32_t *ue_num, uint32_t *entry_num)
{
    uint32_t port_idx;

    if (!uvs_topo_eid_is_valid(ue->primary_eid)) {
        return;
    }

    (*ue_num)++;
    (*entry_num)++;
    for (port_idx = 0; port_idx < PORT_NUM; port_idx++) {
        if (uvs_topo_eid_is_valid(ue->port_eid[port_idx])) {
            (*entry_num)++;
        }
    }
}

static void uvs_count_main_primary_eid_agg_dev(const struct urma_topo_agg_dev *agg_dev,
                                               uint32_t *ue_num, uint32_t *entry_num)
{
    uint32_t iodie_idx;

    for (iodie_idx = 0; iodie_idx < IODIE_NUM; iodie_idx++) {
        uvs_count_main_primary_eid_ue(&agg_dev->ues[iodie_idx], ue_num, entry_num);
    }
}

static void uvs_count_main_primary_eid_topo_items(const struct urma_topo_node *topo,
                                                  uint32_t topo_num, uint32_t *ue_num,
                                                  uint32_t *entry_num)
{
    uint32_t node_idx;
    uint32_t dev_idx;

    *ue_num = 0;
    *entry_num = 0;
    for (node_idx = 0; node_idx < topo_num; node_idx++) {
        const struct urma_topo_node *node = &topo[node_idx];

        for (dev_idx = 0; dev_idx < DEV_NUM; dev_idx++) {
            uvs_count_main_primary_eid_agg_dev(&node->agg_devs[dev_idx], ue_num, entry_num);
        }
    }
}

static uint32_t uvs_main_primary_eid_hash_u32(uint32_t hash, uint32_t value)
{
    uint32_t hash_value = hash;
    uint32_t byte_idx;

    for (byte_idx = 0; byte_idx < sizeof(value); byte_idx++) {
        hash_value ^= (value >> (byte_idx * CHAR_BIT)) & UINT8_MAX;
        hash_value *= UVS_MAIN_PRIMARY_EID_HASH_PRIME;
    }

    return hash_value;
}

static uint32_t uvs_main_primary_eid_group_hash(uint32_t node_idx,
                                                uint32_t entity_id,
                                                uint32_t chip_id)
{
    uint32_t hash = UVS_MAIN_PRIMARY_EID_HASH_OFFSET_BASIS;

    hash = uvs_main_primary_eid_hash_u32(hash, node_idx);
    hash = uvs_main_primary_eid_hash_u32(hash, entity_id);
    hash = uvs_main_primary_eid_hash_u32(hash, chip_id);
    return hash;
}

static bool uvs_main_primary_eid_group_match(const uvs_main_primary_eid_group_t *group,
                                             uint32_t node_idx, uint32_t entity_id,
                                             uint32_t chip_id)
{
    return group->node_idx == node_idx && group->entity_id == entity_id &&
           group->chip_id == chip_id;
}

static uint32_t uvs_main_primary_eid_group_bucket_num(uint32_t ue_num)
{
    if (ue_num > (UINT32_MAX - UVS_MAIN_PRIMARY_EID_GROUP_BUCKET_EXTRA) /
        UVS_MAIN_PRIMARY_EID_GROUP_BUCKET_FACTOR) {
        return 0;
    }

    return ue_num * UVS_MAIN_PRIMARY_EID_GROUP_BUCKET_FACTOR +
           UVS_MAIN_PRIMARY_EID_GROUP_BUCKET_EXTRA;
}

static int uvs_main_primary_eid_group_table_init(
    uvs_main_primary_eid_group_table_t *table, uint32_t ue_num)
{
    uint32_t bucket_num;
    uint32_t bucket_idx;
    size_t bucket_size;
    size_t node_size;

    if (table == NULL || ue_num == 0) {
        return -EINVAL;
    }

    bucket_num = uvs_main_primary_eid_group_bucket_num(ue_num);
    if (bucket_num == 0 ||
        uvs_main_primary_eid_mul_overflow((size_t)bucket_num, sizeof(*table->buckets)) ||
        uvs_main_primary_eid_mul_overflow((size_t)ue_num, sizeof(*table->nodes))) {
        TPSA_LOG_ERR("main primary eid group table size is too large.\n");
        return -EOVERFLOW;
    }
    bucket_size = (size_t)bucket_num * sizeof(*table->buckets);
    node_size = (size_t)ue_num * sizeof(*table->nodes);

    (void)memset(table, 0, sizeof(*table));
    table->buckets = malloc(bucket_size);
    if (table->buckets == NULL) {
        return -ENOMEM;
    }
    table->nodes = malloc(node_size);
    if (table->nodes == NULL) {
        free(table->buckets);
        table->buckets = NULL;
        return -ENOMEM;
    }
    (void)memset(table->nodes, 0, node_size);

    for (bucket_idx = 0; bucket_idx < bucket_num; bucket_idx++) {
        table->buckets[bucket_idx] = UVS_MAIN_PRIMARY_EID_GROUP_INVALID_INDEX;
    }
    table->bucket_num = bucket_num;
    table->node_max = ue_num;
    return 0;
}

static void uvs_main_primary_eid_group_table_uninit(
    uvs_main_primary_eid_group_table_t *table)
{
    if (table == NULL) {
        return;
    }

    free(table->nodes);
    free(table->buckets);
    (void)memset(table, 0, sizeof(*table));
}

static uvs_main_primary_eid_group_t *uvs_find_main_primary_eid_group(
    const uvs_main_primary_eid_group_table_t *table, uint32_t node_idx,
    uint32_t entity_id, uint32_t chip_id)
{
    uint32_t bucket_idx;
    uint32_t node_idx_in_bucket;

    if (table == NULL || table->bucket_num == 0 || table->buckets == NULL ||
        table->nodes == NULL) {
        return NULL;
    }

    bucket_idx = uvs_main_primary_eid_group_hash(node_idx, entity_id, chip_id) %
                 table->bucket_num;
    node_idx_in_bucket = table->buckets[bucket_idx];
    while (node_idx_in_bucket != UVS_MAIN_PRIMARY_EID_GROUP_INVALID_INDEX) {
        uvs_main_primary_eid_group_node_t *node = &table->nodes[node_idx_in_bucket];

        if (uvs_main_primary_eid_group_match(&node->group, node_idx, entity_id,
                                             chip_id)) {
            return &node->group;
        }
        node_idx_in_bucket = node->next;
    }

    return NULL;
}

static int uvs_add_main_primary_eid_group(uvs_main_primary_eid_group_table_t *table,
                                          uint32_t node_idx,
                                          const struct urma_topo_ue *ue)
{
    uvs_main_primary_eid_group_t *group;
    uvs_main_primary_eid_group_node_t *node;
    uint32_t bucket_idx;

    group = uvs_find_main_primary_eid_group(table, node_idx, ue->entity_id,
                                            ue->chip_id);
    if (group != NULL) {
        if (memcmp(group->main_primary_eid.raw, ue->primary_eid, EID_LEN) > 0) {
            (void)memcpy(group->main_primary_eid.raw, ue->primary_eid, EID_LEN);
        }
        return 0;
    }

    if (table == NULL || table->node_num >= table->node_max) {
        return -ENOSPC;
    }

    bucket_idx = uvs_main_primary_eid_group_hash(node_idx, ue->entity_id,
                                                 ue->chip_id) %
                 table->bucket_num;
    node = &table->nodes[table->node_num];
    node->group.node_idx = node_idx;
    node->group.entity_id = ue->entity_id;
    node->group.chip_id = ue->chip_id;
    (void)memcpy(node->group.main_primary_eid.raw, ue->primary_eid, EID_LEN);
    node->next = table->buckets[bucket_idx];
    table->buckets[bucket_idx] = table->node_num;
    table->node_num++;
    return 0;
}

static int uvs_collect_main_primary_eid_groups_from_agg_dev(
    uvs_main_primary_eid_group_table_t *groups, uint32_t node_idx,
    const struct urma_topo_agg_dev *agg_dev);

static int uvs_collect_main_primary_eid_groups(const struct urma_topo_node *topo,
                                               uint32_t topo_num,
                                               uvs_main_primary_eid_group_table_t *groups)
{
    uint32_t node_idx;
    uint32_t dev_idx;
    int ret;

    for (node_idx = 0; node_idx < topo_num; node_idx++) {
        const struct urma_topo_node *node = &topo[node_idx];

        for (dev_idx = 0; dev_idx < DEV_NUM; dev_idx++) {
            ret = uvs_collect_main_primary_eid_groups_from_agg_dev(
                groups, node_idx, &node->agg_devs[dev_idx]);
            if (ret != 0) {
                return ret;
            }
        }
    }

    return 0;
}

static int uvs_alloc_main_primary_eid_ue_buffers(uint32_t ue_num,
                                                 uvs_main_primary_eid_ue_entry_t **entries)
{
    size_t entry_size;

    if (ue_num == 0) {
        return -EINVAL;
    }

    if (uvs_main_primary_eid_mul_overflow((size_t)ue_num, sizeof(**entries))) {
        TPSA_LOG_ERR("main primary eid ue entry count is too large.\n");
        return -EOVERFLOW;
    }
    entry_size = (size_t)ue_num * sizeof(**entries);

    *entries = malloc(entry_size);
    if (*entries == NULL) {
        return -ENOMEM;
    }
    (void)memset(*entries, 0, entry_size);

    return 0;
}

static int uvs_add_main_primary_eid_ue_entry(uvs_main_primary_eid_ue_entry_ctx_t *ctx,
                                             const struct urma_topo_ue *ue,
                                             const uvs_eid_t *main_primary_eid)
{
    uvs_main_primary_eid_ue_entry_t *entry;
    uint32_t port_idx;

    if (!uvs_topo_eid_is_valid(ue->primary_eid)) {
        return 0;
    }

    if (*ctx->entry_num >= ctx->entry_max) {
        return -ENOSPC;
    }

    entry = &ctx->entries[*ctx->entry_num];
    entry->main_primary_eid = *main_primary_eid;
    (void)memcpy(entry->primary_eid.raw, ue->primary_eid, EID_LEN);
    entry->prefix_len = UVS_MAIN_PRIMARY_EID_PREFIX_LEN;

    for (port_idx = 0; port_idx < PORT_NUM; port_idx++) {
        if (!uvs_topo_eid_is_valid(ue->port_eid[port_idx])) {
            continue;
        }

        (void)memcpy(entry->port_eid[port_idx].raw, ue->port_eid[port_idx],
                     EID_LEN);
        entry->port_bitmap |= (uint16_t)(1U << port_idx);
    }

    (*ctx->entry_num)++;
    return 0;
}

static int uvs_collect_main_primary_eid_ue_entries_from_ue(
    uvs_main_primary_eid_ue_entry_ctx_t *ctx, uint32_t node_idx,
    const struct urma_topo_ue *ue)
{
    uvs_main_primary_eid_group_t *group;

    if (!uvs_topo_eid_is_valid(ue->primary_eid)) {
        return 0;
    }

    group = uvs_find_main_primary_eid_group(ctx->groups, node_idx, ue->entity_id,
                                            ue->chip_id);
    if (group == NULL) {
        return -ENOENT;
    }

    return uvs_add_main_primary_eid_ue_entry(ctx, ue, &group->main_primary_eid);
}

static int uvs_collect_main_primary_eid_ue_entries_from_agg_dev(
    uvs_main_primary_eid_ue_entry_ctx_t *ctx, uint32_t node_idx,
    const struct urma_topo_agg_dev *agg_dev)
{
    uint32_t iodie_idx;
    int ret;

    for (iodie_idx = 0; iodie_idx < IODIE_NUM; iodie_idx++) {
        ret = uvs_collect_main_primary_eid_ue_entries_from_ue(
            ctx, node_idx, &agg_dev->ues[iodie_idx]);
        if (ret != 0) {
            return ret;
        }
    }

    return 0;
}

static int uvs_collect_main_primary_eid_ue_entries(const struct urma_topo_node *topo,
                                                   uint32_t topo_num,
                                                   uvs_main_primary_eid_ue_entry_ctx_t *ctx)
{
    uint32_t node_idx;
    uint32_t dev_idx;
    int ret;

    for (node_idx = 0; node_idx < topo_num; node_idx++) {
        const struct urma_topo_node *node = &topo[node_idx];

        for (dev_idx = 0; dev_idx < DEV_NUM; dev_idx++) {
            ret = uvs_collect_main_primary_eid_ue_entries_from_agg_dev(
                ctx, node_idx, &node->agg_devs[dev_idx]);
            if (ret != 0) {
                return ret;
            }
        }
    }

    return 0;
}

static int uvs_collect_main_primary_eid_group_from_ue(uvs_main_primary_eid_group_table_t *groups,
                                                      uint32_t node_idx,
                                                      const struct urma_topo_ue *ue)
{
    if (!uvs_topo_eid_is_valid(ue->primary_eid)) {
        return 0;
    }

    return uvs_add_main_primary_eid_group(groups, node_idx, ue);
}

static int uvs_collect_main_primary_eid_groups_from_agg_dev(uvs_main_primary_eid_group_table_t *groups,
                                                            uint32_t node_idx,
                                                            const struct urma_topo_agg_dev *agg_dev)
{
    uint32_t iodie_idx;
    int ret;

    for (iodie_idx = 0; iodie_idx < IODIE_NUM; iodie_idx++) {
        ret = uvs_collect_main_primary_eid_group_from_ue(groups, node_idx,
                                                         &agg_dev->ues[iodie_idx]);
        if (ret != 0) {
            return ret;
        }
    }

    return 0;
}

static int uvs_build_main_primary_eid_ue_entries_from_topo(
    const struct urma_topo_node *topo, uint32_t topo_num,
    uvs_main_primary_eid_ue_entry_t **entries_out, uint32_t *entry_num_out,
    uint32_t *flat_entry_num_out)
{
    uvs_main_primary_eid_ue_entry_t *entries = NULL;
    uvs_main_primary_eid_group_table_t groups;
    uvs_main_primary_eid_ue_entry_ctx_t entry_ctx;
    uint32_t ue_num = 0;
    uint32_t flat_entry_num = 0;
    uint32_t entry_num = 0;
    int ret;

    *entries_out = NULL;
    *entry_num_out = 0;
    *flat_entry_num_out = 0;

    uvs_count_main_primary_eid_topo_items(topo, topo_num, &ue_num, &flat_entry_num);
    if (ue_num == 0) {
        return 0;
    }

    (void)memset(&groups, 0, sizeof(groups));
    ret = uvs_main_primary_eid_group_table_init(&groups, ue_num);
    if (ret != 0) {
        return ret;
    }

    ret = uvs_alloc_main_primary_eid_ue_buffers(ue_num, &entries);
    if (ret != 0) {
        uvs_main_primary_eid_group_table_uninit(&groups);
        return ret;
    }

    ret = uvs_collect_main_primary_eid_groups(topo, topo_num, &groups);
    if (ret != 0) {
        free(entries);
        uvs_main_primary_eid_group_table_uninit(&groups);
        return ret;
    }

    entry_ctx.entries = entries;
    entry_ctx.entry_num = &entry_num;
    entry_ctx.entry_max = ue_num;
    entry_ctx.groups = &groups;
    ret = uvs_collect_main_primary_eid_ue_entries(topo, topo_num, &entry_ctx);
    if (ret != 0) {
        free(entries);
        uvs_main_primary_eid_group_table_uninit(&groups);
        return ret;
    }

    *entries_out = entries;
    *entry_num_out = entry_num;
    *flat_entry_num_out = flat_entry_num;
    uvs_main_primary_eid_group_table_uninit(&groups);
    return 0;
}

static int uvs_update_main_primary_eid_table_by_topo(const struct urma_topo_node *topo,
                                                     uint32_t topo_num)
{
    uvs_main_primary_eid_ue_entry_t *entries = NULL;
    uint32_t entry_num = 0;
    uint32_t flat_entry_num = 0;
    int ret;

    ret = uvs_build_main_primary_eid_ue_entries_from_topo(topo, topo_num, &entries,
                                                          &entry_num,
                                                          &flat_entry_num);
    if (ret != 0) {
        return ret;
    }

    ret = uvs_insert_main_primary_eid_ue_entries_inner(entries, entry_num);
    if (ret != 0) {
        TPSA_LOG_ERR("failed to update main primary eid ue entries, ret = %d.\n", ret);
    } else {
        TPSA_LOG_INFO("successfully updated main primary eid entries by ue batch, "
                      "ue_entry_num = %u, flat_entry_num = %u.\n",
                      entry_num, flat_entry_num);
    }

    free(entries);
    return ret;
}

static int uvs_set_topo_info_inner(struct urma_topo_node *topo, uint32_t topo_num)
{
    int ret;

    if (!topo || topo_num > UVS_MAX_TOPO_NUM || topo_num == 0) {
        TPSA_LOG_ERR("topo is NULL or topo_num is invalid.\n");
        return -EINVAL;
    }

    ret = uvs_ubagg_ioctl_set_topo(topo, (int)topo_num);
    if (ret != 0) {
        TPSA_LOG_ERR("failed to set topo info in ubagg, ret = %d.\n", ret);
        return ret;
    } else {
        TPSA_LOG_INFO("successfully setted topo info in ubagg\n");
    }

    ret = uvs_update_main_primary_eid_table_by_topo(topo, topo_num);
    if (ret != 0) {
        TPSA_LOG_ERR("failed to update main primary eid table by topo, ret = %d.\n", ret);
        return ret;
    }

    ret = uvs_ubcore_ioctl_set_topo(topo, (int)topo_num);
    if (ret != 0) {
        TPSA_LOG_ERR("failed to set topo info in ubcore, ret = %d.\n", ret);
        return ret;
    } else {
        TPSA_LOG_INFO("successfully setted topo info in ubcore\n");
    }

    return 0;
}

static int uvs_get_topo_info_inner(void *topo)
{
    int ret;

    if (!topo) {
        TPSA_LOG_ERR("topo is NULL.\n");
        return -EINVAL;
    }

    ret = uvs_ubcore_ioctl_get_topo(topo);
    if (ret != 0) {
        TPSA_LOG_ERR("failed to get topo info in ubcore.\n");
        return ret;
    } else {
        TPSA_LOG_INFO("successfully got topo info in ubcore\n");
    }

    return ret;
}

int uvs_set_topo_info(void *topo_buf, uint32_t node_size, uint32_t node_num)
{
    uint32_t size = sizeof(struct urma_topo_node);
    int ret;

    if (size != node_size) {
        TPSA_LOG_ERR("node size not match, urma=%u, ubse=%u\n", size, node_size);
        return -EINVAL;
    }

    uvs_get_api_rdlock();
    ret = uvs_set_topo_info_inner((struct urma_topo_node *)topo_buf, node_num);
    put_uvs_lock();
    return ret;
}

static int uvs_delete_topo_info_inner(void)
{
    int first_ret = 0;
    int ret;

    ret = uvs_ubagg_ioctl_delete_topo();
    if (ret != 0) {
        TPSA_LOG_ERR("failed to delete topo info in ubagg, ret = %d.\n", ret);
        first_ret = ret;
    } else {
        TPSA_LOG_INFO("successfully deleted topo info in ubagg\n");
    }

    ret = uvs_ubcore_ioctl_delete_topo();
    if (ret != 0) {
        TPSA_LOG_ERR("failed to delete topo info in ubcore, ret = %d.\n", ret);
        if (first_ret == 0) {
            first_ret = ret;
        }
    } else {
        TPSA_LOG_INFO("successfully deleted topo info in ubcore\n");
    }

    return first_ret;
}

int uvs_delete_topo_info(void)
{
    int ret;

    uvs_get_api_rdlock();
    ret = uvs_delete_topo_info_inner();
    put_uvs_lock();
    return ret;
}

int uvs_get_topo_info(void *topo)
{
    int ret = 0;
    uvs_get_api_rdlock();
    ret = uvs_get_topo_info_inner(topo);
    put_uvs_lock();
    return ret;
}

int uvs_get_route_list(const uvs_route_t *route, uvs_route_list_t *route_list)
{
    int ret = 0;
    if (route == NULL || route_list == NULL) {
        TPSA_LOG_ERR("Invalid parameter.\n");
        return -EINVAL;
    }
    ret = uvs_ubcore_ioctl_get_route_list(route, route_list);
    if (ret != 0) {
        TPSA_LOG_ERR("Failed to get route list, ret = %d.\n", ret);
    }
    return ret;
}

static void uvs_filter_path_set(uvs_path_set_t *uvs_path_set, bool multi_path)
{
    uvs_path_set_t filtered = *uvs_path_set;
    uint32_t i, j;

    filtered.path_count = 0;
    for (i = 0, j = 0; i < uvs_path_set->path_count; i++) {
        uvs_path_t *path_src = &uvs_path_set->paths[i];
        uvs_path_t *path_dst = &filtered.paths[j];

        if (multi_path && path_src->src_port.port_idx != 255) {
            continue;
        }

        if (!multi_path && path_src->src_port.port_idx == 255) {
            continue;
        }

        *path_dst = *path_src;
        j++;
    }
    filtered.path_count = j;
    *uvs_path_set = filtered;
}

int uvs_get_path_set(const uvs_eid_t *src_bondind_eid,
                     const uvs_eid_t *dst_bonding_eid,
                     enum uvs_tp_type tp_type, bool multi_path,
                     uvs_path_set_t *uvs_path_set)
{
    int ret = 0;

    if (src_bondind_eid == NULL || dst_bonding_eid == NULL) {
        TPSA_LOG_ERR("Invalid parameter.\n");
        return -EINVAL;
    }

    if (tp_type > UVS_UTP || tp_type < UVS_RTP) {
        TPSA_LOG_ERR("Invalid tp_type.\n");
        return -EINVAL;
    }

    if (!uvs_eid_is_valid(src_bondind_eid) ||
        !uvs_eid_is_valid(dst_bonding_eid) ||
        uvs_path_set == NULL) {
        TPSA_LOG_ERR("Invalid parameter.\n");
        return -EINVAL;
    }
    ret = uvs_ubcore_ioctl_get_path_set(src_bondind_eid,
                                        dst_bonding_eid, tp_type, multi_path, uvs_path_set);
    if (ret != 0) {
        TPSA_LOG_ERR("Failed to get path set, ret = %d.\n", ret);
        return ret;
    }
    // Temporary fix for the issue where kernel FULLMESH_1D returns all eids
    if (uvs_path_set->topo_type == UVS_TOPO_TYPE_FULLMESH_1D) {
        uvs_filter_path_set(uvs_path_set, multi_path);
        TPSA_LOG_DEBUG("Filtered path set for FULLMESH_1D\n");
    }
    return ret;
}
