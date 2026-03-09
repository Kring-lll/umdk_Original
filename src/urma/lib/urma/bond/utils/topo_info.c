/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: Helper structure and functions to handle topo info. Implementation.
 * Author: Ma Chuan
 * Create: 2025-06-04
 * Note:
 * History: 2025-06-04
 */
#include <stdlib.h>
#include <string.h>
#include "ub_hash.h"
#include "urma_log.h"
#include "urma_types.h"
#include "topo_info.h"

#define DIRECT_DEV_HASH_BASIS (9819876)

static inline bool is_topo_map_idx_equal(bondp_topo_link_t *map_idx, bondp_topo_link_t* target_map_idx)
{
    return !memcmp(map_idx, target_map_idx, sizeof(bondp_topo_link_t));
}

static inline bool is_empty_eid(urma_eid_t *eid)
{
    return eid->in6.interface_id == 0 && eid->in6.subnet_prefix == 0;
}

static inline bool is_eid_equal(urma_eid_t *eid1, urma_eid_t *eid2)
{
    return !memcmp(eid1, eid2, sizeof(urma_eid_t));
}

topo_map_t *create_topo_map(bondp_topo_node_t *topo_infos, uint32_t node_num)
{
    if (topo_infos == NULL || node_num == 0 || node_num > MAX_NODE_NUM) {
        URMA_LOG_ERR("Invalid topo info to create topo map\n");
        return NULL;
    }
    topo_map_t *topo_map = calloc(1, sizeof(topo_map_t));
    if (topo_map == NULL) {
        URMA_LOG_ERR("Failed to alloc topo_map\n");
        return NULL;
    }
    (void)memcpy(topo_map->topo_infos, topo_infos, sizeof(bondp_topo_node_t) * node_num);
    topo_map->node_num = node_num;

    uint32_t cur_node_idx = UINT32_MAX;
    for (uint32_t i = 0; i < node_num; ++i) {
        if (topo_map->topo_infos[i].is_current) {
            cur_node_idx = i;
            break;
        }
    }
    if (cur_node_idx == UINT32_MAX) {
        URMA_LOG_ERR("topo info doesn't have cur_node\n");
        free(topo_map);
        return NULL;
    }

    return topo_map;
}

void delete_topo_map(topo_map_t *topo_map)
{
    if (topo_map != NULL) {
        free(topo_map);
    }
}

bondp_topo_agg_dev_t *get_topo_dev_info_by_agg_eid(topo_map_t *topo_map, urma_eid_t *agg_eid)
{
    if (topo_map == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < MAX_NODE_NUM; ++i) {
        for (uint32_t j = 0; j < DEV_NUM; ++j) {
            if (is_eid_equal((urma_eid_t *)topo_map->topo_infos[i].agg_devs[j].agg_eid, agg_eid)) {
                return &topo_map->topo_infos[i].agg_devs[j];
            }
        }
    }
    return NULL;
}
