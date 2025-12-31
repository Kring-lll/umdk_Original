/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2025. All rights reserved.
 * Description: tpsa api file
 * Author: Zheng Hongqin
 * Create: 2023-11-22
 * Note:
 * History:
 */

#include <sys/syscall.h>
#include <errno.h>
#include "uvs_api.h"
#include "uvs_cmd_tlv.h"
#include "tpsa_ioctl.h"
#include "uvs_private_api.h"
#include "uvs_ubagg_ioctl.h"

#define UVS_MAX_TOPO_NUM 16

#define EID_FMT          "%2.2x%2.2x:%2.2x%2.2x:%2.2x%2.2x:%2.2x%2.2x:%2.2x%2.2x:%2.2x%2.2x:%2.2x%2.2x:%2.2x%2.2x"
#define EID_RAW_ARGS(eid)                                                                                              \
    eid[0], eid[1], eid[2], eid[3], eid[4], eid[5], eid[6], eid[7], eid[8], eid[9], eid[10], eid[11], eid[12],         \
        eid[13], eid[14], eid[15]
#define EID_ARGS(eid)             EID_RAW_ARGS((eid).raw)
#define EID_LEN (16)
#define MAX_PORT_NUM (9)
#define MAX_ALL_PORT_NUM (18)
#define MAX_NODE_NUM (16)
#define MAX_IODIE_NUM (2)
#define MAX_DEV_NUM (1)

struct io_die_info {
    int socket_id;
    char primary_eid[EID_LEN];
    char port_eid[MAX_PORT_NUM][EID_LEN];
};

/* Locate a specific port in topo_infos */
/* All params set to -1 means doesn't exist */
struct topo_map_idx {
    int topo_info_idx;
    int plane_idx;
    int port_idx;
};

struct topo_dev_info {
    char bonding_eid[EID_LEN];
    struct io_die_info io_die_info[MAX_IODIE_NUM];
};

struct topo_info {
    int id;
    bool is_cur_node;
    struct topo_map_idx links[MAX_IODIE_NUM][MAX_PORT_NUM];
    struct topo_dev_info dev[MAX_DEV_NUM];
};

typedef struct topo_info topo_info_t;

/* 兼容LCNE原有下发格式 */
typedef struct old_tool_iodie_info {
    char primary_eid[EID_LEN];
    char port_eid[MAX_PORT_NUM][EID_LEN];
    char peer_port_eid[MAX_PORT_NUM][EID_LEN];
    int socket_id;
} old_tool_iodie_info_t;

typedef struct old_tool_topo_info {
    char bonding_eid[EID_LEN];
    old_tool_iodie_info_t io_die_info[MAX_IODIE_NUM];
    bool is_cur_node;
} old_tool_topo_info_t;

typedef struct old_tool_topo_map {
    old_tool_topo_info_t topo_infos[MAX_NODE_NUM];
    uint32_t node_num;
} old_tool_topo_map_t;

static inline bool is_uvs_eid_equal(uvs_eid_t *eid1, uvs_eid_t *eid2)
{
    return !memcmp(eid1, eid2, sizeof(uvs_eid_t));
}

static bool is_eid_valid(const char *eid)
{
    int i;

    for (i = 0; i < EID_LEN; i++) {
        if (eid[i] != 0) {
            return true;
        }
    }
    return false;
}

static struct topo_map_idx get_topo_map_idx_by_port_eid(old_tool_topo_info_t *topo_infos, uint32_t node_num, uvs_eid_t *port_eid)
{
    struct topo_map_idx idx = {
        .topo_info_idx = -1,
        .plane_idx = -1,
        .port_idx = -1,
    };
    if (is_eid_valid((char *)port_eid) == false) {
        return idx;
    }
    for (int i = 0; i < node_num; ++i) {
        old_tool_topo_info_t *topo_info = &topo_infos[i];
        for (int k = 0; k < MAX_IODIE_NUM; ++k) {
            old_tool_iodie_info_t *die_info = &topo_info->io_die_info[k];
            for (int l = 0; l < MAX_PORT_NUM; ++l) {
                if (is_uvs_eid_equal((uvs_eid_t *)die_info->port_eid[l], port_eid)) {
                    idx.topo_info_idx = i;
                    idx.plane_idx = k;
                    idx.port_idx = l;
                    return idx;
                }
            }
        }
    }
    return idx;
}

static int trans_old_topo(topo_info_t *new_top_info, old_tool_topo_info_t *old_topo_info, uint32_t topo_num)
{
    TPSA_LOG_ERR("Translating old topology map to new topology map, and node_num is %u\n", topo_num);

    for (int i = 0; i < (int)topo_num; ++i) {
        topo_info_t *topo_info = &new_top_info[i];
        old_tool_topo_info_t *old_info = &old_topo_info[i];
        topo_info->id = i;
        topo_info->is_cur_node = old_info->is_cur_node;
        // 每个node只取一个dev
        struct topo_dev_info *dev_info = &topo_info->dev[0];
        memcpy(dev_info->bonding_eid, old_info->bonding_eid, EID_LEN);
        for (int j = 0; j < MAX_IODIE_NUM; ++j) {
            struct io_die_info *die_info = &dev_info->io_die_info[j];
            die_info->socket_id = old_info->io_die_info[j].socket_id;
            memcpy(die_info->primary_eid, old_info->io_die_info[j].primary_eid, EID_LEN);
            for (int k = 0; k < MAX_PORT_NUM; ++k) {
                memcpy(die_info->port_eid[k], old_info->io_die_info[j].port_eid[k], EID_LEN);
                TPSA_LOG_ERR("****port_eid:"EID_FMT" with peer_port eid: "EID_FMT"\n",
                    EID_ARGS(*(uvs_eid_t *)old_info->io_die_info[j].port_eid[k]),
                    EID_ARGS(*(uvs_eid_t *)old_info->io_die_info[j].peer_port_eid[k]));
                struct topo_map_idx idx = get_topo_map_idx_by_port_eid(old_topo_info, topo_num, (uvs_eid_t *)old_info->io_die_info[j].peer_port_eid[k]);
                topo_info->links[j][k].topo_info_idx = idx.topo_info_idx;
                topo_info->links[j][k].plane_idx = idx.plane_idx;
                topo_info->links[j][k].port_idx = idx.port_idx;
            }
        }
    }

    return 0;
}

int uvs_set_topo_info_inner(void *topo, uint32_t topo_num)
{
    int ret;

    if (!topo || topo_num > UVS_MAX_TOPO_NUM || topo_num == 0) {
        TPSA_LOG_ERR("topo is NULL or topo_num is invalid.\n");
        return -EINVAL;
    }

    /* 兼容LCNE旧有格式 */
    topo_info_t new_topo_infos[MAX_NODE_NUM];
    memset(new_topo_infos, 0, sizeof(topo_info_t) * MAX_NODE_NUM);
    trans_old_topo(new_topo_infos, (old_tool_topo_info_t *)topo, topo_num);
    ret = uvs_ubagg_ioctl_set_topo((void *)new_topo_infos, topo_num);
    if (ret != 0) {
        TPSA_LOG_ERR("failed to set topo info in ubagg.\n");
        return ret;
    } else {
        TPSA_LOG_INFO("success to set topo info in ubagg\n");
    }

    TPSA_LOG_INFO("User space args size:%lu.\n", sizeof(topo_info_t) * topo_num);
    ret = uvs_ubcore_ioctl_set_topo((void *)new_topo_infos, topo_num);
    if (ret != 0) {
        TPSA_LOG_ERR("failed to set topo info in ubcore.\n");
    } else {
        TPSA_LOG_INFO("success to set topo info in ubcore\n");
    }

    return ret;
}

int uvs_set_topo_info(void *topo, uint32_t topo_num)
{
    int ret = 0;
    uvs_get_api_rdlock();
    ret = uvs_set_topo_info_inner(topo, topo_num);
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
        TPSA_LOG_ERR("Failed to get route list, ret: %d.\n", ret);
    }
    return ret;
}
