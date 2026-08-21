/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 HiSilicon Technologies Co., Ltd. All rights reserved.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 *
 * Description: user space topology path_set calculation
 */

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "tpsa_log.h"
#include "uvs_topo_path.h"

#define UVS_INVALID_PORT (255)

/*
 * Private copy of the topology map. Only node_num nodes are allocated; the
 * urma_topo_map_t layout reserves MAX_NODE_NUM entries, which would be far
 * larger than any real deployment needs.
 */
static struct urma_topo_node *g_topo_nodes;
static uint32_t g_topo_node_num;
static pthread_rwlock_t g_topo_lock = PTHREAD_RWLOCK_INITIALIZER;

static uint64_t g_cmp_count;
static uint64_t g_diff_count;
static pthread_mutex_t g_stat_lock = PTHREAD_MUTEX_INITIALIZER;

int uvs_topo_path_update(const struct urma_topo_node *nodes, uint32_t node_num)
{
    struct urma_topo_node *copy = NULL;
    struct urma_topo_node *old = NULL;
    size_t bytes;

    if (nodes == NULL || node_num == 0 || node_num > MAX_NODE_NUM) {
        TPSA_LOG_ERR("Invalid topo params, node_num: %u.\n", node_num);
        return -EINVAL;
    }

    bytes = (size_t)node_num * sizeof(struct urma_topo_node);
    copy = (struct urma_topo_node *)malloc(bytes);
    if (copy == NULL) {
        TPSA_LOG_ERR("Failed to alloc local topo copy, bytes: %zu.\n", bytes);
        return -ENOMEM;
    }
    (void)memcpy(copy, nodes, bytes);

    (void)pthread_rwlock_wrlock(&g_topo_lock);
    old = g_topo_nodes;
    g_topo_nodes = copy;
    g_topo_node_num = node_num;
    (void)pthread_rwlock_unlock(&g_topo_lock);

    free(old);
    TPSA_LOG_INFO("Local topo map updated, node_num: %u, bytes: %zu.\n",
                  node_num, bytes);
    return 0;
}

void uvs_topo_path_clear(void)
{
    struct urma_topo_node *old = NULL;

    (void)pthread_rwlock_wrlock(&g_topo_lock);
    old = g_topo_nodes;
    g_topo_nodes = NULL;
    g_topo_node_num = 0;
    (void)pthread_rwlock_unlock(&g_topo_lock);

    free(old);
}

bool uvs_topo_path_ready(void)
{
    bool ready;

    (void)pthread_rwlock_rdlock(&g_topo_lock);
    ready = (g_topo_nodes != NULL && g_topo_node_num != 0);
    (void)pthread_rwlock_unlock(&g_topo_lock);
    return ready;
}

/* Same rule as the kernel is_eid_valid(): any non-zero byte makes it valid. */
static bool uvs_topo_eid_valid(const char *eid)
{
    int i;

    for (i = 0; i < EID_LEN; i++) {
        if (eid[i] != 0) {
            return true;
        }
    }
    return false;
}

/* Locate the node and device index owning agg_eid. Caller holds the rdlock. */
static struct urma_topo_node *uvs_topo_find_by_agg_eid(const uvs_eid_t *agg_eid,
                                                       uint32_t *device_id)
{
    uint32_t node_id;
    uint32_t dev_id;

    for (node_id = 0; node_id < g_topo_node_num; node_id++) {
        for (dev_id = 0; dev_id < DEV_NUM; dev_id++) {
            if (memcmp(agg_eid, g_topo_nodes[node_id].agg_devs[dev_id].agg_eid,
                       sizeof(uvs_eid_t)) == 0) {
                *device_id = dev_id;
                return &g_topo_nodes[node_id];
            }
        }
    }
    return NULL;
}

/*
 * iodie level: emit one path per connected iodie pair, using the primary eid.
 * The link matrix is read from the destination node, matching the kernel.
 */
static int uvs_topo_path_set_primary(const struct urma_topo_node *dst_node,
                                     const struct urma_topo_agg_dev *src_agg,
                                     const struct urma_topo_agg_dev *dst_agg,
                                     uvs_path_set_t *path_set)
{
    uint32_t path_idx = 0;
    uint32_t local_iodie;
    uint32_t remote_iodie;

    for (local_iodie = 0; local_iodie < IODIE_NUM; local_iodie++) {
        for (remote_iodie = 0; remote_iodie < IODIE_NUM; remote_iodie++) {
            bool has_port_link = false;
            uint32_t local_port;
            uvs_path_t *path = NULL;

            if (path_idx >= UVS_MAX_ROUTES) {
                TPSA_LOG_WARN("Path set is full, path_count: %u.\n", path_idx);
                path_set->path_count = path_idx;
                return 0;
            }

            for (local_port = 0; local_port < PORT_NUM && !has_port_link; local_port++) {
                uint32_t local_idx = local_iodie * PORT_NUM + local_port;
                uint32_t remote_port;

                for (remote_port = 0; remote_port < PORT_NUM && !has_port_link; remote_port++) {
                    uint32_t remote_idx = remote_iodie * PORT_NUM + remote_port;

                    if (dst_node->links[local_idx][remote_idx]) {
                        has_port_link = true;
                    }
                }
            }

            if (!has_port_link) {
                continue;
            }

            if (!uvs_topo_eid_valid(src_agg->ues[local_iodie].primary_eid) ||
                !uvs_topo_eid_valid(dst_agg->ues[remote_iodie].primary_eid)) {
                continue;
            }

            path = &path_set->paths[path_idx];
            path->src_port.chip_id = (uint8_t)src_agg->ues[local_iodie].chip_id;
            path->src_port.die_id = (uint8_t)src_agg->ues[local_iodie].die_id;
            path->src_port.port_idx = UVS_INVALID_PORT;
            path->src_port.reserved = 0;

            path->dst_port.chip_id = (uint8_t)dst_agg->ues[remote_iodie].chip_id;
            path->dst_port.die_id = (uint8_t)dst_agg->ues[remote_iodie].die_id;
            path->dst_port.port_idx = UVS_INVALID_PORT;
            path->dst_port.reserved = 0;

            (void)memcpy(&path->src_eid, src_agg->ues[local_iodie].primary_eid,
                         sizeof(uvs_eid_t));
            (void)memcpy(&path->dst_eid, dst_agg->ues[remote_iodie].primary_eid,
                         sizeof(uvs_eid_t));
            path_idx++;
        }
    }

    path_set->path_count = path_idx;
    if (path_idx == 0) {
        TPSA_LOG_ERR("Failed to get any valid primary path.\n");
        return -EINVAL;
    }
    return 0;
}

/*
 * port level: emit one path per connected port pair, using the port eid.
 * Only the source port eid is checked for validity, matching the kernel.
 */
static int uvs_topo_path_set_port(const struct urma_topo_node *dst_node,
                                  const struct urma_topo_agg_dev *src_agg,
                                  const struct urma_topo_agg_dev *dst_agg,
                                  uvs_path_set_t *path_set)
{
    uint32_t path_idx = 0;
    uint32_t local_idx;

    for (local_idx = 0; local_idx < IODIE_NUM * PORT_NUM; local_idx++) {
        uint32_t local_iodie = local_idx / PORT_NUM;
        uint32_t local_port = local_idx % PORT_NUM;
        uint32_t remote_idx;

        if (path_idx >= UVS_MAX_ROUTES) {
            TPSA_LOG_WARN("Path set is full, path_count: %u.\n", path_idx);
            path_set->path_count = path_idx;
            return 0;
        }

        if (!uvs_topo_eid_valid(src_agg->ues[local_iodie].port_eid[local_port])) {
            continue;
        }

        for (remote_idx = 0; remote_idx < IODIE_NUM * PORT_NUM; remote_idx++) {
            uint32_t remote_iodie;
            uint32_t remote_port;
            uvs_path_t *path = NULL;

            if (!dst_node->links[local_idx][remote_idx]) {
                continue;
            }

            if (path_idx >= UVS_MAX_ROUTES) {
                TPSA_LOG_WARN("Path set is full, path_count: %u.\n", path_idx);
                path_set->path_count = path_idx;
                return 0;
            }

            remote_iodie = remote_idx / PORT_NUM;
            remote_port = remote_idx % PORT_NUM;

            path = &path_set->paths[path_idx];
            path->src_port.chip_id = (uint8_t)src_agg->ues[local_iodie].chip_id;
            path->src_port.die_id = (uint8_t)src_agg->ues[local_iodie].die_id;
            path->src_port.port_idx = (uint8_t)local_port;
            path->src_port.reserved = 0;

            path->dst_port.chip_id = (uint8_t)dst_agg->ues[remote_iodie].chip_id;
            path->dst_port.die_id = (uint8_t)dst_agg->ues[remote_iodie].die_id;
            path->dst_port.port_idx = (uint8_t)remote_port;
            path->dst_port.reserved = 0;

            (void)memcpy(&path->src_eid,
                         src_agg->ues[local_iodie].port_eid[local_port],
                         sizeof(uvs_eid_t));
            (void)memcpy(&path->dst_eid,
                         dst_agg->ues[remote_iodie].port_eid[remote_port],
                         sizeof(uvs_eid_t));
            path_idx++;
        }
    }

    path_set->path_count = path_idx;
    if (path_idx == 0) {
        TPSA_LOG_ERR("Failed to get any valid port path.\n");
        return -EINVAL;
    }
    return 0;
}

int uvs_topo_local_get_path_set(const uvs_eid_t *src_bonding_eid,
                                const uvs_eid_t *dst_bonding_eid,
                                enum uvs_tp_type tp_type, bool iodie_level,
                                uvs_path_set_t *path_set)
{
    struct urma_topo_node *src_node = NULL;
    struct urma_topo_node *dst_node = NULL;
    struct urma_topo_agg_dev *src_agg = NULL;
    struct urma_topo_agg_dev *dst_agg = NULL;
    uint32_t src_dev_id = 0;
    uint32_t dst_dev_id = 0;
    int ret;

    if (src_bonding_eid == NULL || dst_bonding_eid == NULL || path_set == NULL) {
        TPSA_LOG_ERR("Invalid parameter.\n");
        return -EINVAL;
    }

    (void)pthread_rwlock_rdlock(&g_topo_lock);

    if (g_topo_nodes == NULL || g_topo_node_num == 0) {
        (void)pthread_rwlock_unlock(&g_topo_lock);
        TPSA_LOG_ERR("Local topo map does not exist.\n");
        return -EINVAL;
    }

    (void)memset(path_set, 0, sizeof(uvs_path_set_t));

    src_node = uvs_topo_find_by_agg_eid(src_bonding_eid, &src_dev_id);
    if (src_node == NULL) {
        (void)pthread_rwlock_unlock(&g_topo_lock);
        TPSA_LOG_ERR("Failed to get src topo node.\n");
        return -EINVAL;
    }

    dst_node = uvs_topo_find_by_agg_eid(dst_bonding_eid, &dst_dev_id);
    if (dst_node == NULL) {
        (void)pthread_rwlock_unlock(&g_topo_lock);
        TPSA_LOG_ERR("Failed to get dst topo node.\n");
        return -EINVAL;
    }

    if (src_node->type != dst_node->type) {
        (void)pthread_rwlock_unlock(&g_topo_lock);
        TPSA_LOG_ERR("src topo type is not equal with dst topo type.\n");
        return -EINVAL;
    }

    path_set->topo_type = (enum uvs_topo_type_t)src_node->type;
    path_set->chip_count = IODIE_NUM;
    path_set->die_count = IODIE_NUM_PER_CHIP;
    path_set->src_node.node_id = src_node->node_id;
    path_set->src_node.super_node_id = src_node->super_node_id;
    path_set->dst_node.node_id = dst_node->node_id;
    path_set->dst_node.super_node_id = dst_node->super_node_id;

    src_agg = &src_node->agg_devs[src_dev_id];
    dst_agg = &dst_node->agg_devs[dst_dev_id];

    /* iodie level RTP: the path array stays empty. */
    if (iodie_level && tp_type == UVS_RTP) {
        path_set->path_count = 0;
        (void)pthread_rwlock_unlock(&g_topo_lock);
        return 0;
    }

    if (iodie_level && tp_type == UVS_CTP) {
        ret = uvs_topo_path_set_primary(dst_node, src_agg, dst_agg, path_set);
    } else {
        ret = uvs_topo_path_set_port(dst_node, src_agg, dst_agg, path_set);
    }

    (void)pthread_rwlock_unlock(&g_topo_lock);
    return ret;
}

bool uvs_path_set_equal(const uvs_path_set_t *a, const uvs_path_set_t *b)
{
    uint32_t i;

    if (a == NULL || b == NULL) {
        return false;
    }

    if (a->topo_type != b->topo_type || a->chip_count != b->chip_count ||
        a->die_count != b->die_count || a->path_count != b->path_count) {
        return false;
    }

    if (memcmp(&a->src_node, &b->src_node, sizeof(uvs_node_id_t)) != 0 ||
        memcmp(&a->dst_node, &b->dst_node, sizeof(uvs_node_id_t)) != 0) {
        return false;
    }

    if (a->path_count > UVS_MAX_ROUTES) {
        return false;
    }

    for (i = 0; i < a->path_count; i++) {
        if (a->paths[i].src_port.value != b->paths[i].src_port.value ||
            a->paths[i].dst_port.value != b->paths[i].dst_port.value) {
            return false;
        }
        if (memcmp(&a->paths[i].src_eid, &b->paths[i].src_eid, sizeof(uvs_eid_t)) != 0 ||
            memcmp(&a->paths[i].dst_eid, &b->paths[i].dst_eid, sizeof(uvs_eid_t)) != 0) {
            return false;
        }
    }

    return true;
}

bool uvs_topo_path_shadow_enabled(void)
{
    static int cached = -1;
    const char *env = NULL;

    if (cached >= 0) {
        return cached != 0;
    }

    env = getenv("UVS_TOPO_PATH_SHADOW");
    if (env != NULL && (env[0] == '0') && env[1] == '\0') {
        cached = 0;
    } else {
        cached = 1;
    }
    return cached != 0;
}

void uvs_topo_path_count_cmp(bool matched)
{
    (void)pthread_mutex_lock(&g_stat_lock);
    g_cmp_count++;
    if (!matched) {
        g_diff_count++;
    }
    (void)pthread_mutex_unlock(&g_stat_lock);
}

uint64_t uvs_topo_path_diff_count(void)
{
    uint64_t v;

    (void)pthread_mutex_lock(&g_stat_lock);
    v = g_diff_count;
    (void)pthread_mutex_unlock(&g_stat_lock);
    return v;
}

uint64_t uvs_topo_path_cmp_count(void)
{
    uint64_t v;

    (void)pthread_mutex_lock(&g_stat_lock);
    v = g_cmp_count;
    (void)pthread_mutex_unlock(&g_stat_lock);
    return v;
}
