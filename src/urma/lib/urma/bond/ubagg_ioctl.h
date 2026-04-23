/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: ubagg ioctl and user ctl cmd and struct header
 * Author: Ma Chuan
 * Create: 2025-06-10
 * Note:
 * History:
 */
#ifndef UBAGG_IOCTL_H
#define UBAGG_IOCTL_H

#include <stdint.h>
#include "topo_info.h"
#include "urma_ubagg.h"
#include "urma_types.h"

#define UBAGG_MAX_DEV_NAME_LEN    (64)

struct ubagg_set_topo_info {
    struct {
        void *topo;
        uint32_t topo_num;
    } in;
};

typedef enum ubagg_userctl_opcode {
    GET_SLAVE_DEVICE = 1,
    GET_TOPO_INFO    = 2,
    SEND_SWITCHBACK_REQ = 9,
} ubagg_userctl_opcode_t;

typedef struct ubagg_switchback_req {
    struct {
        uint32_t vjetty_id;
        uint32_t local_idx;
        uint32_t target_idx;
        uint8_t ctrl_type;
        uint8_t req_seq;
        uint16_t reserved;
        uint32_t payload;
    } in;
} ubagg_switchback_req_t;

typedef struct ubagg_switchback_msg {
    struct {
        uint32_t recv_local_id;
        uint8_t ctrl_type;
        uint8_t req_seq;
        uint16_t reserved;
        uint32_t payload;
    } in;
} ubagg_switchback_msg_t;

typedef struct bondp_physical_device {
	char dev_name[UBAGG_MAX_DEV_NAME_LEN];
	uint32_t chip_id;
	uint32_t primary_eid_idx;
	uint32_t port_eid_idx[PORT_NUM];
} bondp_physical_device_t;

typedef struct bondp_userctl_physical_device_out {
	int physical_dev_num;
	bondp_physical_device_t physical_devs[IODIE_NUM];
} bondp_userctl_physical_device_out_t;

struct ubagg_topo_info_out {
    bondp_topo_node_t topo_info[MAX_NODE_NUM];
    uint32_t node_num;
};
#endif // UBAGG_IOCTL_H
