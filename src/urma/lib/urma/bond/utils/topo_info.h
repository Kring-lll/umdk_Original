/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: Helper structure and functions to handle topo info
 * Author: Ma Chuan
 * Create: 2025-06-04
 * Note:
 * History: 2025-06-04
 */
#ifndef TOPO_INFO_H
#define TOPO_INFO_H

#include <stdbool.h>
#include <stdint.h>
#include "urma_types.h"
#include "bondp_hash_table.h"

#define CNA_LEN (3)
#define EID_LEN (16)
#define PORT_NUM (9)
#define MAX_ALL_PORT_NUM (18)
#define MAX_NODE_NUM (16)
#define IODIE_NUM (2)
#define DEV_NUM (128)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bondp_topo_ue {
    uint32_t socket_id;
    uint32_t entity_id;
    char primary_eid[EID_LEN];
    char port_eid[PORT_NUM][EID_LEN];
} bondp_topo_ue_t;

/* Locate a specific port in topo_infos */
/* All params set to -1 means doesn't exist */
typedef struct bondp_topo_link {
    uint32_t peer_node;
    uint32_t peer_iodie;
    uint32_t peer_port;
} bondp_topo_link_t;

typedef struct bondp_topo_agg_dev {
    char agg_eid[EID_LEN];
    bondp_topo_ue_t ues[IODIE_NUM];
} bondp_topo_agg_dev_t;

typedef struct bondp_topo_node {
    uint32_t id;
    uint32_t is_current;
    bondp_topo_link_t links[IODIE_NUM][PORT_NUM];
    bondp_topo_agg_dev_t agg_devs[DEV_NUM];
} bondp_topo_node_t;

typedef struct eid_mapping_entry {
    hmap_node_t hmap_node;
    urma_eid_t  key_eid;
    urma_eid_t  bonding_eid;
} eid_mapping_entry_t;

typedef struct topo_map {
    bondp_topo_node_t topo_infos[MAX_NODE_NUM];
    uint32_t node_num;
} topo_map_t;

/* The following functions needs the caller to check the validity of the parameters */

topo_map_t *create_topo_map(bondp_topo_node_t *topo_infos, uint32_t node_num);

void delete_topo_map(topo_map_t *topo_map);

bondp_topo_agg_dev_t *get_topo_dev_info_by_agg_eid(topo_map_t *topo_map, urma_eid_t *agg_eid);

#ifdef __cplusplus
}
#endif
#endif // TOPO_INFO_H
