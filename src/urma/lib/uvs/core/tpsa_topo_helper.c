/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: tpsa topo internal helpers
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tpsa_log.h"
#include "uvs_ubagg_ioctl.h"
#include "tpsa_topo_helper.h"

#define UVS_MAIN_PRIMARY_EID_INVALID_INDEX UINT32_MAX
#define UVS_MAIN_PRIMARY_EID_BUCKET_FACTOR 2U
#define UVS_MAIN_PRIMARY_EID_BUCKET_EXTRA 1U
#define UVS_MAIN_PRIMARY_EID_GROUP_BATCH_MAX 4096U
#define UVS_MAIN_PRIMARY_EID_HASH_OFFSET_BASIS 2166136261U
#define UVS_MAIN_PRIMARY_EID_HASH_PRIME 16777619U

typedef struct uvs_main_primary_eid_group {
    uint32_t node_idx;
    uint32_t entity_id;
    uint32_t chip_id;
    uint32_t prefix_head;
    uint32_t prefix_tail;
    uint32_t prefix_num;
    uvs_eid_t main_primary_eid;
} uvs_main_primary_eid_group_t;

typedef struct uvs_main_primary_eid_group_node {
    uvs_main_primary_eid_group_t group;
    uint32_t next;
} uvs_main_primary_eid_group_node_t;

typedef struct uvs_main_primary_eid_prefix_node {
    uvs_eid_t prefix;
    uint32_t next;
} uvs_main_primary_eid_prefix_node_t;

typedef struct uvs_main_primary_eid_group_table {
    uint32_t *buckets;
    uvs_main_primary_eid_group_node_t *nodes;
    uvs_main_primary_eid_prefix_node_t *prefix_nodes;
    uint32_t bucket_num;
    uint32_t node_num;
    uint32_t node_max;
    uint32_t prefix_num;
    uint32_t prefix_max;
} uvs_main_primary_eid_group_table_t;

typedef struct uvs_main_primary_eid_group_batch {
    struct uvs_main_primary_eid_group_entry *groups;
    uint32_t group_num;
    uvs_eid_t *prefixes;
    uint32_t prefix_num;
} uvs_main_primary_eid_group_batch_t;

struct uvs_main_primary_eid_group_entry {
    uvs_eid_t main_primary_eid;
    uint8_t prefix_len;
    uint8_t reserved[3];
    uint32_t prefix_offset;
    uint32_t prefix_num;
};

typedef struct uvs_main_primary_eid_topo_count {
    uint32_t group_num;
    uint32_t prefix_num;
} uvs_main_primary_eid_topo_count_t;

typedef enum uvs_main_primary_eid_scan_stage {
    UVS_MAIN_PRIMARY_EID_SCAN_COUNT,
    UVS_MAIN_PRIMARY_EID_SCAN_GROUP,
    UVS_MAIN_PRIMARY_EID_SCAN_PREFIX,
} uvs_main_primary_eid_scan_stage_t;

static inline bool uvs_topo_eid_is_valid(const char *eid)
{
    const char empty_eid[EID_LEN] = {0};

    return eid != NULL && memcmp(eid, empty_eid, EID_LEN) != 0;
}

static bool uvs_main_primary_eid_mul_overflow(size_t count, size_t item_size)
{
    return count != 0 && item_size > SIZE_MAX / count;
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
    return uvs_main_primary_eid_hash_u32(hash, chip_id);
}

static bool uvs_main_primary_eid_group_match(
    const uvs_main_primary_eid_group_t *group, uint32_t node_idx,
    uint32_t entity_id, uint32_t chip_id)
{
    return group->node_idx == node_idx &&
           group->entity_id == entity_id &&
           group->chip_id == chip_id;
}

static uint32_t uvs_main_primary_eid_bucket_num(uint32_t group_num)
{
    if (group_num > (UINT32_MAX - UVS_MAIN_PRIMARY_EID_BUCKET_EXTRA) /
        UVS_MAIN_PRIMARY_EID_BUCKET_FACTOR) {
        return 0;
    }

    return group_num * UVS_MAIN_PRIMARY_EID_BUCKET_FACTOR +
           UVS_MAIN_PRIMARY_EID_BUCKET_EXTRA;
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

static int uvs_count_main_primary_eid_topo_item(
    uvs_main_primary_eid_topo_count_t *count,
    const struct urma_topo_ue *endpoint_group)
{
    uint32_t prefix_num =
        uvs_main_primary_eid_endpoint_prefix_num(endpoint_group);
    if (prefix_num == 0) {
        return 0;
    }
    if (count->group_num == UINT32_MAX ||
        prefix_num > UINT32_MAX - count->prefix_num) {
        return -EOVERFLOW;
    }

    count->group_num++;
    count->prefix_num += prefix_num;
    return 0;
}

static int uvs_main_primary_eid_group_table_init(
    uvs_main_primary_eid_group_table_t *table, uint32_t group_num,
    uint32_t prefix_num)
{
    uint32_t bucket_num;
    uint32_t bucket_idx;
    size_t bucket_size;
    size_t node_size;
    size_t prefix_size;

    if (table == NULL || group_num == 0) {
        return -EINVAL;
    }

    bucket_num = uvs_main_primary_eid_bucket_num(group_num);
    if (bucket_num == 0 ||
        uvs_main_primary_eid_mul_overflow((size_t)bucket_num, sizeof(*table->buckets)) ||
        uvs_main_primary_eid_mul_overflow((size_t)group_num, sizeof(*table->nodes)) ||
        uvs_main_primary_eid_mul_overflow((size_t)prefix_num,
                                          sizeof(*table->prefix_nodes))) {
        TPSA_LOG_ERR("main primary eid group table size is too large.\n");
        return -EOVERFLOW;
    }

    bucket_size = (size_t)bucket_num * sizeof(*table->buckets);
    node_size = (size_t)group_num * sizeof(*table->nodes);
    prefix_size = (size_t)prefix_num * sizeof(*table->prefix_nodes);

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

    table->prefix_nodes = malloc(prefix_size);
    if (table->prefix_nodes == NULL) {
        free(table->nodes);
        free(table->buckets);
        table->nodes = NULL;
        table->buckets = NULL;
        return -ENOMEM;
    }
    (void)memset(table->prefix_nodes, 0, prefix_size);

    for (bucket_idx = 0; bucket_idx < bucket_num; bucket_idx++) {
        table->buckets[bucket_idx] = UVS_MAIN_PRIMARY_EID_INVALID_INDEX;
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

    free(table->prefix_nodes);
    free(table->nodes);
    free(table->buckets);
    (void)memset(table, 0, sizeof(*table));
}

static uvs_main_primary_eid_group_t *uvs_find_main_primary_eid_group(
    const uvs_main_primary_eid_group_table_t *table, uint32_t node_idx,
    uint32_t entity_id, uint32_t chip_id)
{
    uint32_t bucket_idx;
    uint32_t idx;

    if (table == NULL || table->bucket_num == 0 ||
        table->buckets == NULL || table->nodes == NULL) {
        return NULL;
    }

    bucket_idx = uvs_main_primary_eid_group_hash(node_idx, entity_id, chip_id) %
                 table->bucket_num;
    idx = table->buckets[bucket_idx];
    while (idx != UVS_MAIN_PRIMARY_EID_INVALID_INDEX) {
        uvs_main_primary_eid_group_node_t *node = &table->nodes[idx];

        if (uvs_main_primary_eid_group_match(&node->group, node_idx,
                                             entity_id, chip_id)) {
            return &node->group;
        }
        idx = node->next;
    }

    return NULL;
}

static int uvs_add_main_primary_eid_group(uvs_main_primary_eid_group_table_t *table,
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
    node->group.prefix_head = UVS_MAIN_PRIMARY_EID_INVALID_INDEX;
    node->group.prefix_tail = UVS_MAIN_PRIMARY_EID_INVALID_INDEX;
    (void)memcpy(node->group.main_primary_eid.raw,
                 endpoint_group->primary_eid, EID_LEN);
    node->next = table->buckets[bucket_idx];
    table->buckets[bucket_idx] = table->node_num;
    table->node_num++;
    return 0;
}

static int uvs_collect_main_primary_eid_group(
    uvs_main_primary_eid_group_table_t *table, uint32_t node_idx,
    const struct urma_topo_ue *endpoint_group)
{
    if (!uvs_topo_eid_is_valid(endpoint_group->primary_eid)) {
        return 0;
    }

    return uvs_add_main_primary_eid_group(table, node_idx, endpoint_group);
}

static int uvs_append_main_primary_eid_prefix(uvs_main_primary_eid_group_table_t *table,
                                              uvs_main_primary_eid_group_t *group,
                                              const char *eid)
{
    uint32_t prefix_idx;

    if (table == NULL || group == NULL || eid == NULL ||
        table->prefix_nodes == NULL) {
        return -EINVAL;
    }
    if (table->prefix_num >= table->prefix_max ||
        group->prefix_num == UINT32_MAX) {
        return -ENOSPC;
    }

    prefix_idx = table->prefix_num++;
    (void)memcpy(table->prefix_nodes[prefix_idx].prefix.raw, eid, EID_LEN);
    table->prefix_nodes[prefix_idx].next = UVS_MAIN_PRIMARY_EID_INVALID_INDEX;

    if (group->prefix_num == 0) {
        group->prefix_head = prefix_idx;
    } else {
        table->prefix_nodes[group->prefix_tail].next = prefix_idx;
    }
    group->prefix_tail = prefix_idx;
    group->prefix_num++;
    return 0;
}

static int uvs_append_main_primary_eid_endpoint_prefixes(
    uvs_main_primary_eid_group_table_t *table,
    uvs_main_primary_eid_group_t *group,
    const struct urma_topo_ue *endpoint_group)
{
    uint32_t port_idx;
    int ret;

    ret = uvs_append_main_primary_eid_prefix(table, group,
                                             endpoint_group->primary_eid);
    if (ret != 0) {
        return ret;
    }

    for (port_idx = 0; port_idx < PORT_NUM; port_idx++) {
        if (!uvs_topo_eid_is_valid(endpoint_group->port_eid[port_idx])) {
            continue;
        }

        ret = uvs_append_main_primary_eid_prefix(table, group,
                                                 endpoint_group->port_eid[port_idx]);
        if (ret != 0) {
            return ret;
        }
    }

    return 0;
}

static int uvs_collect_main_primary_eid_prefixes(
    uvs_main_primary_eid_group_table_t *table, uint32_t node_idx,
    const struct urma_topo_ue *endpoint_group)
{
    uvs_main_primary_eid_group_t *group;

    if (!uvs_topo_eid_is_valid(endpoint_group->primary_eid)) {
        return 0;
    }

    group = uvs_find_main_primary_eid_group(table, node_idx,
                                            endpoint_group->entity_id,
                                            endpoint_group->chip_id);
    if (group == NULL) {
        return -ENOENT;
    }

    return uvs_append_main_primary_eid_endpoint_prefixes(
        table, group, endpoint_group);
}

static int uvs_scan_main_primary_eid_endpoint(
    uvs_main_primary_eid_scan_stage_t stage, uint32_t node_idx,
    const struct urma_topo_ue *endpoint_group,
    uvs_main_primary_eid_group_table_t *table,
    uvs_main_primary_eid_topo_count_t *count)
{
    switch (stage) {
        case UVS_MAIN_PRIMARY_EID_SCAN_COUNT:
            return uvs_count_main_primary_eid_topo_item(count, endpoint_group);
        case UVS_MAIN_PRIMARY_EID_SCAN_GROUP:
            return uvs_collect_main_primary_eid_group(table, node_idx, endpoint_group);
        case UVS_MAIN_PRIMARY_EID_SCAN_PREFIX:
            return uvs_collect_main_primary_eid_prefixes(table, node_idx, endpoint_group);
        default:
            return -EINVAL;
    }
}

static int uvs_scan_main_primary_eid_agg_dev(
    const struct urma_topo_agg_dev *agg_dev, uint32_t node_idx,
    uvs_main_primary_eid_scan_stage_t stage,
    uvs_main_primary_eid_group_table_t *table,
    uvs_main_primary_eid_topo_count_t *count)
{
    uint32_t iodie_idx;

    for (iodie_idx = 0; iodie_idx < IODIE_NUM; iodie_idx++) {
        const struct urma_topo_ue *endpoint_group = &agg_dev->ues[iodie_idx];
        int ret = uvs_scan_main_primary_eid_endpoint(stage, node_idx,
                                                     endpoint_group, table, count);
        if (ret != 0) {
            return ret;
        }
    }

    return 0;
}

static int uvs_scan_main_primary_eid_node(
    const struct urma_topo_node *node, uint32_t node_idx,
    uvs_main_primary_eid_scan_stage_t stage,
    uvs_main_primary_eid_group_table_t *table,
    uvs_main_primary_eid_topo_count_t *count)
{
    uint32_t dev_idx;

    for (dev_idx = 0; dev_idx < DEV_NUM; dev_idx++) {
        int ret = uvs_scan_main_primary_eid_agg_dev(&node->agg_devs[dev_idx],
                                                    node_idx, stage, table, count);
        if (ret != 0) {
            return ret;
        }
    }

    return 0;
}

static int uvs_scan_main_primary_eid_topo(
    const struct urma_topo_node *topo, uint32_t topo_num,
    uvs_main_primary_eid_scan_stage_t stage,
    uvs_main_primary_eid_group_table_t *table,
    uvs_main_primary_eid_topo_count_t *count)
{
    uint32_t node_idx;

    if ((stage == UVS_MAIN_PRIMARY_EID_SCAN_COUNT && count == NULL) ||
        (stage != UVS_MAIN_PRIMARY_EID_SCAN_COUNT && table == NULL)) {
        return -EINVAL;
    }

    for (node_idx = 0; node_idx < topo_num; node_idx++) {
        int ret = uvs_scan_main_primary_eid_node(&topo[node_idx], node_idx,
                                                 stage, table, count);
        if (ret != 0) {
            return ret;
        }
    }

    return 0;
}

static int uvs_build_main_primary_eid_groups_from_topo(
    const struct urma_topo_node *topo, uint32_t topo_num,
    uvs_main_primary_eid_group_table_t *table)
{
    uvs_main_primary_eid_topo_count_t count = {0};
    int ret;

    (void)memset(table, 0, sizeof(*table));
    ret = uvs_scan_main_primary_eid_topo(topo, topo_num,
                                         UVS_MAIN_PRIMARY_EID_SCAN_COUNT,
                                         NULL, &count);
    if (ret != 0 || count.group_num == 0) {
        return ret;
    }

    ret = uvs_main_primary_eid_group_table_init(table, count.group_num,
                                                count.prefix_num);
    if (ret != 0) {
        return ret;
    }

    ret = uvs_scan_main_primary_eid_topo(topo, topo_num,
                                         UVS_MAIN_PRIMARY_EID_SCAN_GROUP,
                                         table, NULL);
    if (ret == 0) {
        ret = uvs_scan_main_primary_eid_topo(topo, topo_num,
                                             UVS_MAIN_PRIMARY_EID_SCAN_PREFIX,
                                             table, NULL);
    }
    if (ret == 0 && table->prefix_num != count.prefix_num) {
        ret = -EFAULT;
    }
    if (ret != 0) {
        uvs_main_primary_eid_group_table_uninit(table);
    }

    return ret;
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
    const uvs_main_primary_eid_group_t *group,
    const uvs_main_primary_eid_prefix_node_t *prefix_nodes,
    uint32_t *prefix_idx)
{
    struct uvs_main_primary_eid_group_entry *entry;
    uint32_t chunk_num;
    int ret;

    if (batch->group_num == UVS_MAIN_PRIMARY_EID_GROUP_BATCH_MAX ||
        batch->prefix_num == UVS_MAIN_PRIMARY_EID_BATCH_MAX) {
        ret = uvs_main_primary_eid_group_batch_flush(batch);
        if (ret != 0) {
            return ret;
        }
    }
    if (*prefix_idx == UVS_MAIN_PRIMARY_EID_INVALID_INDEX) {
        return -EINVAL;
    }

    entry = &batch->groups[batch->group_num];
    entry->main_primary_eid = group->main_primary_eid;
    entry->prefix_len = UVS_MAIN_PRIMARY_EID_PREFIX_LEN;
    entry->prefix_offset = batch->prefix_num;
    (void)memset(entry->reserved, 0, sizeof(entry->reserved));

    for (chunk_num = 0;
         *prefix_idx != UVS_MAIN_PRIMARY_EID_INVALID_INDEX &&
         batch->prefix_num < UVS_MAIN_PRIMARY_EID_BATCH_MAX;
         chunk_num++) {
        batch->prefixes[batch->prefix_num++] = prefix_nodes[*prefix_idx].prefix;
        *prefix_idx = prefix_nodes[*prefix_idx].next;
    }
    if (chunk_num == 0) {
        return -EINVAL;
    }

    entry->prefix_num = chunk_num;
    batch->group_num++;
    return 0;
}

static int uvs_insert_main_primary_eid_group_batch_inner(
    const uvs_main_primary_eid_group_table_t *table)
{
    uvs_main_primary_eid_group_batch_t batch = {0};
    uint32_t group_idx;
    int ret = 0;

    if (table == NULL || (table->node_num != 0 && table->prefix_nodes == NULL)) {
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

    for (group_idx = 0; group_idx < table->node_num && ret == 0; group_idx++) {
        const uvs_main_primary_eid_group_t *group = &table->nodes[group_idx].group;
        uint32_t prefix_idx = group->prefix_head;

        while (prefix_idx != UVS_MAIN_PRIMARY_EID_INVALID_INDEX) {
            ret = uvs_main_primary_eid_group_batch_append(&batch, group,
                                                          table->prefix_nodes,
                                                          &prefix_idx);
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

int uvs_update_main_primary_eid_table_by_topo(const struct urma_topo_node *topo,
                                              uint32_t topo_num)
{
    uvs_main_primary_eid_group_table_t table;
    int ret;

    ret = uvs_build_main_primary_eid_groups_from_topo(topo, topo_num, &table);
    if (ret != 0) {
        return ret;
    }

    ret = uvs_insert_main_primary_eid_group_batch_inner(&table);
    if (ret != 0) {
        TPSA_LOG_ERR("failed to update main primary eid group entries, ret = %d.\n", ret);
    } else {
        TPSA_LOG_INFO("successfully updated main primary eid entries by group batch, "
                      "group_num = %u, prefix_num = %u.\n",
                      table.node_num, table.prefix_num);
    }

    uvs_main_primary_eid_group_table_uninit(&table);
    return ret;
}
