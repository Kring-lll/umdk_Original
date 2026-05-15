/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: tpsa topo internal helpers
 */

#include "tpsa_topo_helper.h"
#include "tpsa_log.h"
#include "uvs_ubagg_ioctl.h"
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define UVS_MAIN_PRIMARY_EID_GROUP_INVALID_INDEX UINT32_MAX
#define UVS_MAIN_PRIMARY_EID_GROUP_BUCKET_FACTOR 2U
#define UVS_MAIN_PRIMARY_EID_GROUP_BUCKET_EXTRA 1U
#define UVS_MAIN_PRIMARY_EID_HASH_OFFSET_BASIS 2166136261U
#define UVS_MAIN_PRIMARY_EID_HASH_PRIME 16777619U

typedef struct uvs_main_primary_eid_group {
    uint32_t node_idx;
    uint32_t entity_id;
    uint32_t chip_id;
    uint32_t prefix_offset;
    uint32_t prefix_num;
    uint32_t prefix_filled_num;
    uvs_eid_t main_primary_eid;
} uvs_main_primary_eid_group_t;

typedef struct uvs_main_primary_eid_group_node {
    uvs_main_primary_eid_group_t group;
    uint32_t next;
} uvs_main_primary_eid_group_node_t;

typedef struct uvs_main_primary_eid_group_table {
    uint32_t *buckets;
    uvs_main_primary_eid_group_node_t *nodes;
    uvs_eid_t *prefixes;
    uint32_t bucket_num;
    uint32_t node_num;
    uint32_t node_max;
    uint32_t prefix_num;
    uint32_t prefix_max;
} uvs_main_primary_eid_group_table_t;

typedef struct uvs_main_primary_eid_group_batch {
    uvs_main_primary_eid_group_entry_t *groups;
    uint32_t group_num;
    uvs_eid_t *prefixes;
    uint32_t prefix_num;
} uvs_main_primary_eid_group_batch_t;

static inline bool uvs_topo_eid_is_valid(const char *eid)
{
    const char empty_eid[EID_LEN] = {0};

    return eid != NULL && memcmp(eid, empty_eid, EID_LEN) != 0;
}

static bool uvs_main_primary_eid_mul_overflow(size_t count, size_t item_size)
{
    return count != 0 && item_size > SIZE_MAX / count;
}

static uint32_t uvs_main_primary_eid_hash_raw(uint32_t hash, const uint8_t *raw,
                                              size_t raw_len)
{
    uint32_t hash_value = hash;
    size_t byte_idx;

    for (byte_idx = 0; byte_idx < raw_len; byte_idx++) {
        hash_value ^= raw[byte_idx];
        hash_value *= UVS_MAIN_PRIMARY_EID_HASH_PRIME;
    }

    return hash_value;
}

static int uvs_main_primary_eid_group_batch_flush(
    uvs_main_primary_eid_group_batch_t *batch)
{
    int ret;

    if (batch->prefix_num == 0) {
        return 0;
    }
    if (batch->group_num == 0) {
        return -EINVAL;
    }

    ret = uvs_ubcore_ioctl_insert_main_primary_eid_group_batch(
        batch->groups, batch->group_num, batch->prefixes, batch->prefix_num);
    if (ret != 0) {
        return ret;
    }

    batch->group_num = 0;
    batch->prefix_num = 0;
    return 0;
}

static int uvs_main_primary_eid_group_batch_append(
    uvs_main_primary_eid_group_batch_t *batch,
    const uvs_main_primary_eid_group_t *group, const uvs_eid_t *prefixes,
    uint32_t *prefix_offset)
{
    uvs_main_primary_eid_group_entry_t *entry;
    uint32_t chunk_num;
    int ret;

    if (batch->group_num == UVS_MAIN_PRIMARY_EID_GROUP_BATCH_MAX ||
        batch->prefix_num == UVS_MAIN_PRIMARY_EID_BATCH_MAX) {
        ret = uvs_main_primary_eid_group_batch_flush(batch);
        if (ret != 0) {
            return ret;
        }
    }

    chunk_num = group->prefix_num - *prefix_offset;
    if (chunk_num > UVS_MAIN_PRIMARY_EID_BATCH_MAX - batch->prefix_num) {
        chunk_num = UVS_MAIN_PRIMARY_EID_BATCH_MAX - batch->prefix_num;
    }
    if (chunk_num == 0) {
        return -EINVAL;
    }

    entry = &batch->groups[batch->group_num];
    entry->main_primary_eid = group->main_primary_eid;
    entry->prefix_len = UVS_MAIN_PRIMARY_EID_PREFIX_LEN;
    entry->prefix_offset = batch->prefix_num;
    entry->prefix_num = chunk_num;
    (void)memset(entry->reserved, 0, sizeof(entry->reserved));
    (void)memcpy(&batch->prefixes[batch->prefix_num],
                 &prefixes[*prefix_offset], (size_t)chunk_num * sizeof(*prefixes));

    batch->group_num++;
    batch->prefix_num += chunk_num;
    *prefix_offset += chunk_num;
    return 0;
}

static int uvs_insert_main_primary_eid_group_batch_inner(
    const uvs_main_primary_eid_group_table_t *table)
{
    uvs_main_primary_eid_group_batch_t batch = {0};
    uint32_t node_idx;
    int ret = 0;

    if (table == NULL || (table->node_num != 0 && table->prefixes == NULL)) {
        TPSA_LOG_ERR("Invalid main primary eid group table.\n");
        return -EINVAL;
    }
    if (table->node_num == 0) {
        return 0;
    }

    batch.groups = malloc((size_t)UVS_MAIN_PRIMARY_EID_GROUP_BATCH_MAX *
                          sizeof(*batch.groups));
    if (batch.groups == NULL) {
        return -ENOMEM;
    }

    batch.prefixes = malloc((size_t)UVS_MAIN_PRIMARY_EID_BATCH_MAX *
                            sizeof(*batch.prefixes));
    if (batch.prefixes == NULL) {
        free(batch.groups);
        return -ENOMEM;
    }

    for (node_idx = 0; node_idx < table->node_num && ret == 0; node_idx++) {
        const uvs_main_primary_eid_group_t *group = &table->nodes[node_idx].group;
        const uvs_eid_t *prefixes = &table->prefixes[group->prefix_offset];
        uint32_t prefix_offset = 0;

        while (prefix_offset < group->prefix_num) {
            ret = uvs_main_primary_eid_group_batch_append(&batch, group,
                                                          prefixes, &prefix_offset);
            if (ret != 0) {
                break;
            }
        }
    }

    if (ret == 0) {
        ret = uvs_main_primary_eid_group_batch_flush(&batch);
    }

    free(batch.prefixes);
    free(batch.groups);
    return ret;
}

static uint32_t uvs_main_primary_eid_endpoint_prefix_num(
    const struct urma_topo_ue *endpoint_group)
{
    uint32_t prefix_num = 0;
    uint32_t port_idx;

    if (!uvs_topo_eid_is_valid(endpoint_group->primary_eid)) {
        return 0;
    }

    prefix_num++;
    for (port_idx = 0; port_idx < PORT_NUM; port_idx++) {
        if (uvs_topo_eid_is_valid(endpoint_group->port_eid[port_idx])) {
            prefix_num++;
        }
    }

    return prefix_num;
}

static void uvs_count_main_primary_eid_topo_items(const struct urma_topo_node *topo,
                                                  uint32_t topo_num,
                                                  uint32_t *endpoint_group_num,
                                                  uint32_t *entry_num)
{
    uint32_t node_idx;
    uint32_t dev_idx;
    uint32_t iodie_idx;

    *endpoint_group_num = 0;
    *entry_num = 0;
    for (node_idx = 0; node_idx < topo_num; node_idx++) {
        const struct urma_topo_node *node = &topo[node_idx];

        for (dev_idx = 0; dev_idx < DEV_NUM; dev_idx++) {
            const struct urma_topo_agg_dev *agg_dev = &node->agg_devs[dev_idx];

            for (iodie_idx = 0; iodie_idx < IODIE_NUM; iodie_idx++) {
                uint32_t prefix_num =
                    uvs_main_primary_eid_endpoint_prefix_num(&agg_dev->ues[iodie_idx]);

                if (prefix_num == 0) {
                    continue;
                }
                (*endpoint_group_num)++;
                (*entry_num) += prefix_num;
            }
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

static uint32_t uvs_main_primary_eid_group_hash_by_eid(const uvs_eid_t *main_primary_eid)
{
    return uvs_main_primary_eid_hash_raw(UVS_MAIN_PRIMARY_EID_HASH_OFFSET_BASIS,
                                         main_primary_eid->raw, EID_LEN);
}

static bool uvs_main_primary_eid_group_match(const uvs_main_primary_eid_group_t *group,
                                             uint32_t node_idx, uint32_t entity_id,
                                             uint32_t chip_id)
{
    return group->node_idx == node_idx && group->entity_id == entity_id &&
           group->chip_id == chip_id;
}

static bool uvs_main_primary_eid_group_match_by_eid(
    const uvs_main_primary_eid_group_t *group, const uvs_eid_t *main_primary_eid)
{
    return memcmp(group->main_primary_eid.raw, main_primary_eid->raw, EID_LEN) == 0;
}

static uint32_t uvs_main_primary_eid_group_bucket_num(uint32_t group_num)
{
    if (group_num >
        (UINT32_MAX - UVS_MAIN_PRIMARY_EID_GROUP_BUCKET_EXTRA) /
        UVS_MAIN_PRIMARY_EID_GROUP_BUCKET_FACTOR) {
        return 0;
    }

    return group_num * UVS_MAIN_PRIMARY_EID_GROUP_BUCKET_FACTOR +
           UVS_MAIN_PRIMARY_EID_GROUP_BUCKET_EXTRA;
}

static int uvs_main_primary_eid_group_table_init(
    uvs_main_primary_eid_group_table_t *table, uint32_t group_num,
    uint32_t prefix_num)
{
    uint32_t bucket_num;
    uint32_t bucket_idx;
    size_t bucket_size;
    size_t node_size;
    size_t prefix_size = 0;

    if (table == NULL || group_num == 0) {
        return -EINVAL;
    }

    bucket_num = uvs_main_primary_eid_group_bucket_num(group_num);
    if (bucket_num == 0 ||
        uvs_main_primary_eid_mul_overflow((size_t)bucket_num, sizeof(*table->buckets)) ||
        uvs_main_primary_eid_mul_overflow((size_t)group_num, sizeof(*table->nodes)) ||
        (prefix_num != 0 &&
         uvs_main_primary_eid_mul_overflow((size_t)prefix_num, sizeof(*table->prefixes)))) {
        TPSA_LOG_ERR("main primary eid group table size is too large.\n");
        return -EOVERFLOW;
    }

    bucket_size = (size_t)bucket_num * sizeof(*table->buckets);
    node_size = (size_t)group_num * sizeof(*table->nodes);
    if (prefix_num != 0) {
        prefix_size = (size_t)prefix_num * sizeof(*table->prefixes);
    }

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

    if (prefix_num != 0) {
        table->prefixes = malloc(prefix_size);
        if (table->prefixes == NULL) {
            free(table->nodes);
            free(table->buckets);
            table->nodes = NULL;
            table->buckets = NULL;
            return -ENOMEM;
        }
        (void)memset(table->prefixes, 0, prefix_size);
    }

    for (bucket_idx = 0; bucket_idx < bucket_num; bucket_idx++) {
        table->buckets[bucket_idx] = UVS_MAIN_PRIMARY_EID_GROUP_INVALID_INDEX;
    }
    table->bucket_num = bucket_num;
    table->node_max = group_num;
    table->prefix_max = prefix_num;
    return 0;
}

static void uvs_main_primary_eid_group_table_uninit(
    uvs_main_primary_eid_group_table_t *table)
{
    if (table == NULL) {
        return;
    }

    free(table->prefixes);
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

static uvs_main_primary_eid_group_t *uvs_find_main_primary_eid_map_group(
    const uvs_main_primary_eid_group_table_t *table, const uvs_eid_t *main_primary_eid)
{
    uint32_t bucket_idx;
    uint32_t node_idx_in_bucket;

    if (table == NULL || main_primary_eid == NULL || table->bucket_num == 0 ||
        table->buckets == NULL || table->nodes == NULL) {
        return NULL;
    }

    bucket_idx = uvs_main_primary_eid_group_hash_by_eid(main_primary_eid) %
                 table->bucket_num;
    node_idx_in_bucket = table->buckets[bucket_idx];
    while (node_idx_in_bucket != UVS_MAIN_PRIMARY_EID_GROUP_INVALID_INDEX) {
        uvs_main_primary_eid_group_node_t *node = &table->nodes[node_idx_in_bucket];

        if (uvs_main_primary_eid_group_match_by_eid(&node->group,
                                                    main_primary_eid)) {
            return &node->group;
        }
        node_idx_in_bucket = node->next;
    }

    return NULL;
}

static int uvs_add_main_primary_eid_topo_group(uvs_main_primary_eid_group_table_t *table,
                                               uint32_t node_idx,
                                               const struct urma_topo_ue *endpoint_group)
{
    uvs_main_primary_eid_group_t *group;
    uvs_main_primary_eid_group_node_t *node;
    uint32_t bucket_idx;

    group = uvs_find_main_primary_eid_group(table, node_idx,
                                            endpoint_group->entity_id,
                                            endpoint_group->chip_id);
    if (group != NULL) {
        if (memcmp(group->main_primary_eid.raw, endpoint_group->primary_eid,
                   EID_LEN) > 0) {
            (void)memcpy(group->main_primary_eid.raw,
                         endpoint_group->primary_eid, EID_LEN);
        }
        return 0;
    }

    if (table == NULL || table->node_num >= table->node_max) {
        return -ENOSPC;
    }

    bucket_idx = uvs_main_primary_eid_group_hash(node_idx,
                                                 endpoint_group->entity_id,
                                                 endpoint_group->chip_id) %
                 table->bucket_num;
    node = &table->nodes[table->node_num];
    node->group.node_idx = node_idx;
    node->group.entity_id = endpoint_group->entity_id;
    node->group.chip_id = endpoint_group->chip_id;
    (void)memcpy(node->group.main_primary_eid.raw,
                 endpoint_group->primary_eid, EID_LEN);
    node->next = table->buckets[bucket_idx];
    table->buckets[bucket_idx] = table->node_num;
    table->node_num++;
    return 0;
}

static int uvs_add_main_primary_eid_map_group(uvs_main_primary_eid_group_table_t *table,
                                              const uvs_eid_t *main_primary_eid,
                                              uvs_main_primary_eid_group_t **group_out)
{
    uvs_main_primary_eid_group_t *group;
    uvs_main_primary_eid_group_node_t *node;
    uint32_t bucket_idx;

    if (group_out == NULL || table == NULL || main_primary_eid == NULL) {
        return -EINVAL;
    }

    group = uvs_find_main_primary_eid_map_group(table, main_primary_eid);
    if (group != NULL) {
        *group_out = group;
        return 0;
    }

    if (table->node_num >= table->node_max || table->prefixes == NULL) {
        return -ENOSPC;
    }

    bucket_idx = uvs_main_primary_eid_group_hash_by_eid(main_primary_eid) %
                 table->bucket_num;
    node = &table->nodes[table->node_num];
    (void)memset(node, 0, sizeof(*node));
    (void)memcpy(node->group.main_primary_eid.raw, main_primary_eid->raw, EID_LEN);
    node->group.prefix_offset = 0;
    node->group.prefix_num = 0;
    node->group.prefix_filled_num = 0;
    node->next = table->buckets[bucket_idx];
    table->buckets[bucket_idx] = table->node_num;
    table->node_num++;
    *group_out = &node->group;
    return 0;
}

static int uvs_count_main_primary_eid_prefixes(uvs_main_primary_eid_group_table_t *table,
                                               uvs_main_primary_eid_group_t *group,
                                               uint32_t prefix_num)
{
    if (table == NULL || group == NULL) {
        return -EINVAL;
    }

    if (prefix_num > UINT32_MAX - group->prefix_num ||
        prefix_num > UINT32_MAX - table->prefix_num) {
        return -EOVERFLOW;
    }

    if (prefix_num > table->prefix_max - table->prefix_num) {
        return -ENOSPC;
    }

    group->prefix_num += prefix_num;
    table->prefix_num += prefix_num;
    return 0;
}

static int uvs_collect_main_primary_eid_topo_groups(
    const struct urma_topo_node *topo, uint32_t topo_num,
    uvs_main_primary_eid_group_table_t *groups)
{
    uint32_t node_idx;
    uint32_t dev_idx;
    uint32_t iodie_idx;
    int ret;

    for (node_idx = 0; node_idx < topo_num; node_idx++) {
        const struct urma_topo_node *node = &topo[node_idx];

        for (dev_idx = 0; dev_idx < DEV_NUM; dev_idx++) {
            const struct urma_topo_agg_dev *agg_dev = &node->agg_devs[dev_idx];

            for (iodie_idx = 0; iodie_idx < IODIE_NUM; iodie_idx++) {
                const struct urma_topo_ue *endpoint_group = &agg_dev->ues[iodie_idx];

                if (!uvs_topo_eid_is_valid(endpoint_group->primary_eid)) {
                    continue;
                }

                ret = uvs_add_main_primary_eid_topo_group(groups, node_idx,
                                                          endpoint_group);
                if (ret != 0) {
                    return ret;
                }
            }
        }
    }

    return 0;
}

static int uvs_count_main_primary_eid_map_groups(
    const struct urma_topo_node *topo, uint32_t topo_num,
    const uvs_main_primary_eid_group_table_t *topo_groups,
    uvs_main_primary_eid_group_table_t *groups)
{
    uint32_t node_idx;
    uint32_t dev_idx;
    uint32_t iodie_idx;
    int ret;

    for (node_idx = 0; node_idx < topo_num; node_idx++) {
        const struct urma_topo_node *node = &topo[node_idx];

        for (dev_idx = 0; dev_idx < DEV_NUM; dev_idx++) {
            const struct urma_topo_agg_dev *agg_dev = &node->agg_devs[dev_idx];

            for (iodie_idx = 0; iodie_idx < IODIE_NUM; iodie_idx++) {
                const struct urma_topo_ue *endpoint_group = &agg_dev->ues[iodie_idx];
                uvs_main_primary_eid_group_t *topo_group;
                uvs_main_primary_eid_group_t *map_group;
                uint32_t prefix_num =
                    uvs_main_primary_eid_endpoint_prefix_num(endpoint_group);

                if (prefix_num == 0) {
                    continue;
                }

                topo_group = uvs_find_main_primary_eid_group(topo_groups, node_idx,
                                                             endpoint_group->entity_id,
                                                             endpoint_group->chip_id);
                if (topo_group == NULL) {
                    return -ENOENT;
                }

                ret = uvs_add_main_primary_eid_map_group(
                    groups, &topo_group->main_primary_eid, &map_group);
                if (ret != 0) {
                    return ret;
                }

                ret = uvs_count_main_primary_eid_prefixes(groups, map_group, prefix_num);
                if (ret != 0) {
                    return ret;
                }
            }
        }
    }

    return 0;
}

static int uvs_assign_main_primary_eid_map_group_offsets(
    uvs_main_primary_eid_group_table_t *groups)
{
    uint32_t node_idx;
    uint32_t prefix_offset = 0;

    for (node_idx = 0; node_idx < groups->node_num; node_idx++) {
        uvs_main_primary_eid_group_t *group = &groups->nodes[node_idx].group;

        if (prefix_offset > UINT32_MAX - group->prefix_num) {
            return -EOVERFLOW;
        }

        group->prefix_offset = prefix_offset;
        group->prefix_filled_num = 0;
        prefix_offset += group->prefix_num;
    }

    if (prefix_offset != groups->prefix_num) {
        return -EFAULT;
    }

    return 0;
}

static int uvs_fill_main_primary_eid_prefix(uvs_main_primary_eid_group_table_t *table,
                                            uvs_main_primary_eid_group_t *group,
                                            const char *eid)
{
    uint32_t prefix_idx;

    if (table == NULL || group == NULL || eid == NULL || table->prefixes == NULL) {
        return -EINVAL;
    }
    if (group->prefix_offset >= table->prefix_max ||
        group->prefix_filled_num >= group->prefix_num ||
        group->prefix_filled_num >= table->prefix_max - group->prefix_offset) {
        return -ENOSPC;
    }

    prefix_idx = group->prefix_offset + group->prefix_filled_num;
    (void)memcpy(table->prefixes[prefix_idx].raw, eid, EID_LEN);
    group->prefix_filled_num++;
    return 0;
}

static int uvs_fill_main_primary_eid_prefixes_from_endpoint_group(
    uvs_main_primary_eid_group_table_t *groups,
    const struct urma_topo_ue *endpoint_group, const uvs_eid_t *main_primary_eid)
{
    uvs_main_primary_eid_group_t *group;
    uint32_t port_idx;
    int ret;

    group = uvs_find_main_primary_eid_map_group(groups, main_primary_eid);
    if (group == NULL) {
        return -ENOENT;
    }

    ret = uvs_fill_main_primary_eid_prefix(groups, group, endpoint_group->primary_eid);
    if (ret != 0) {
        return ret;
    }

    for (port_idx = 0; port_idx < PORT_NUM; port_idx++) {
        if (!uvs_topo_eid_is_valid(endpoint_group->port_eid[port_idx])) {
            continue;
        }

        ret = uvs_fill_main_primary_eid_prefix(groups, group,
                                               endpoint_group->port_eid[port_idx]);
        if (ret != 0) {
            return ret;
        }
    }

    return 0;
}

static int uvs_fill_main_primary_eid_map_groups(
    const struct urma_topo_node *topo, uint32_t topo_num,
    const uvs_main_primary_eid_group_table_t *topo_groups,
    uvs_main_primary_eid_group_table_t *groups)
{
    uint32_t node_idx;
    uint32_t dev_idx;
    uint32_t iodie_idx;
    int ret;

    for (node_idx = 0; node_idx < topo_num; node_idx++) {
        const struct urma_topo_node *node = &topo[node_idx];

        for (dev_idx = 0; dev_idx < DEV_NUM; dev_idx++) {
            const struct urma_topo_agg_dev *agg_dev = &node->agg_devs[dev_idx];

            for (iodie_idx = 0; iodie_idx < IODIE_NUM; iodie_idx++) {
                const struct urma_topo_ue *endpoint_group = &agg_dev->ues[iodie_idx];
                uvs_main_primary_eid_group_t *group;

                if (!uvs_topo_eid_is_valid(endpoint_group->primary_eid)) {
                    continue;
                }

                group = uvs_find_main_primary_eid_group(topo_groups, node_idx,
                                                        endpoint_group->entity_id,
                                                        endpoint_group->chip_id);
                if (group == NULL) {
                    return -ENOENT;
                }

                ret = uvs_fill_main_primary_eid_prefixes_from_endpoint_group(
                    groups, endpoint_group, &group->main_primary_eid);
                if (ret != 0) {
                    return ret;
                }
            }
        }
    }

    return 0;
}

static int uvs_validate_main_primary_eid_map_groups(
    const uvs_main_primary_eid_group_table_t *groups)
{
    uint32_t node_idx;

    for (node_idx = 0; node_idx < groups->node_num; node_idx++) {
        const uvs_main_primary_eid_group_t *group = &groups->nodes[node_idx].group;

        if (group->prefix_filled_num != group->prefix_num) {
            return -EFAULT;
        }
    }

    return 0;
}

static void uvs_uninit_main_primary_eid_group_tables(
    uvs_main_primary_eid_group_table_t *topo_groups,
    uvs_main_primary_eid_group_table_t *groups)
{
    uvs_main_primary_eid_group_table_uninit(groups);
    uvs_main_primary_eid_group_table_uninit(topo_groups);
}

static int uvs_init_main_primary_eid_group_tables(
    const struct urma_topo_node *topo, uint32_t topo_num,
    uvs_main_primary_eid_group_table_t *topo_groups,
    uvs_main_primary_eid_group_table_t *groups, uint32_t *entry_num)
{
    uint32_t endpoint_group_num = 0;
    int ret;

    (void)memset(topo_groups, 0, sizeof(*topo_groups));
    (void)memset(groups, 0, sizeof(*groups));
    *entry_num = 0;

    uvs_count_main_primary_eid_topo_items(topo, topo_num,
                                          &endpoint_group_num, entry_num);
    if (endpoint_group_num == 0) {
        return 0;
    }

    ret = uvs_main_primary_eid_group_table_init(topo_groups,
                                                endpoint_group_num, 0);
    if (ret != 0) {
        return ret;
    }

    ret = uvs_main_primary_eid_group_table_init(groups, endpoint_group_num,
                                                *entry_num);
    if (ret != 0) {
        uvs_main_primary_eid_group_table_uninit(topo_groups);
        return ret;
    }

    return 0;
}

static int uvs_populate_main_primary_eid_group_tables(
    const struct urma_topo_node *topo, uint32_t topo_num,
    uvs_main_primary_eid_group_table_t *topo_groups,
    uvs_main_primary_eid_group_table_t *groups, uint32_t entry_num)
{
    int ret;

    /* Count first so every main-primary group owns one contiguous prefix slice. */
    ret = uvs_collect_main_primary_eid_topo_groups(topo, topo_num, topo_groups);
    if (ret != 0) {
        return ret;
    }

    ret = uvs_count_main_primary_eid_map_groups(topo, topo_num, topo_groups,
                                                groups);
    if (ret != 0) {
        return ret;
    }

    if (groups->prefix_num != entry_num) {
        return -EFAULT;
    }

    ret = uvs_assign_main_primary_eid_map_group_offsets(groups);
    if (ret != 0) {
        return ret;
    }

    ret = uvs_fill_main_primary_eid_map_groups(topo, topo_num, topo_groups,
                                               groups);
    if (ret != 0) {
        return ret;
    }

    return uvs_validate_main_primary_eid_map_groups(groups);
}

static int uvs_build_main_primary_eid_groups_from_topo(
    const struct urma_topo_node *topo, uint32_t topo_num,
    uvs_main_primary_eid_group_table_t *topo_groups,
    uvs_main_primary_eid_group_table_t *groups)
{
    uint32_t entry_num;
    int ret;

    ret = uvs_init_main_primary_eid_group_tables(topo, topo_num, topo_groups,
                                                 groups, &entry_num);
    if (ret != 0) {
        return ret;
    }

    if (entry_num == 0) {
        return 0;
    }

    ret = uvs_populate_main_primary_eid_group_tables(topo, topo_num,
                                                     topo_groups, groups,
                                                     entry_num);
    if (ret != 0) {
        uvs_uninit_main_primary_eid_group_tables(topo_groups, groups);
    }

    return ret;
}

int uvs_update_main_primary_eid_table_by_topo(const struct urma_topo_node *topo,
                                              uint32_t topo_num)
{
    uvs_main_primary_eid_group_table_t topo_groups;
    uvs_main_primary_eid_group_table_t groups;
    int ret;

    ret = uvs_build_main_primary_eid_groups_from_topo(topo, topo_num,
                                                      &topo_groups, &groups);
    if (ret != 0) {
        return ret;
    }

    ret = uvs_insert_main_primary_eid_group_batch_inner(&groups);
    if (ret != 0) {
        TPSA_LOG_ERR("failed to update main primary eid group entries, ret = %d.\n", ret);
    } else {
        TPSA_LOG_INFO("successfully updated main primary eid entries by group batch, "
                      "group_num = %u, prefix_num = %u.\n",
                      groups.node_num, groups.prefix_num);
    }

    uvs_main_primary_eid_group_table_uninit(&groups);
    uvs_main_primary_eid_group_table_uninit(&topo_groups);
    return ret;
}
