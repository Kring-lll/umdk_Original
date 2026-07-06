/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: uvs ubagg netlink header file
 */
#ifndef UVS_UBAGG_NETLINK_H
#define UVS_UBAGG_NETLINK_H

#include <stdint.h>

#include "uvs_api.h"

/*
 * The following UBAGG generic netlink ABI values come from kernel
 * drivers/ub/urma/ubagg/ubagg_netlink.h. Keep them aligned with the kernel
 * definitions and do not depend on any provider-private UMDK header here.
 */
#define UVS_UBAGG_GENL_FAMILY_NAME    "UBAGG_GENL"
#define UVS_UBAGG_GENL_FAMILY_VERSION 1
#define UVS_UBAGG_NL_HDR_ARGS_ADDR    4
#define UVS_UBAGG_NL_CMD_GET_TOPO     1

typedef struct uvs_ubagg_nl_topo_info {
    struct {
        int node_idx;
    } in;
    struct {
        uint32_t node_num;
        struct urma_topo_node topo_info;
    } out;
} uvs_ubagg_nl_topo_info_t;

int uvs_ubagg_nl_get_topo_info(struct urma_topo_node **topo, uint32_t *topo_num);

#endif // UVS_UBAGG_NETLINK_H
