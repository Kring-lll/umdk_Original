/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: tools for handling ubagg devices by command line
 * Author: Dongxu Li
 * Create: 2025-01-14
 * Note:
 * History:
 */
#ifndef UBAGG_CLI_H
#define UBAGG_CLI_H
#include <stdint.h>
#include "urma_ubagg.h"
#include "urma_types.h"

#define UBAGG_MAX_DEV_NAME_LEN    (64)
#define UBAGG_EID_SIZE            (sizeof(urma_eid_t))

typedef enum ubagg_cmd {
    UBAGG_ADD_DEV = 1,
    UBAGG_RMV_DEV,
} ubagg_cmd_t;

struct ubagg_cmd_hdr {
    uint32_t command;
    uint32_t args_len;
    uint64_t args_addr;
};

#define UBAGG_CMD_MAGIC 'B'
#define UBAGG_CMD _IOWR(UBAGG_CMD_MAGIC, 1, struct ubagg_cmd_hdr)

struct ubagg_add_dev {
    struct {
        int slave_dev_num;
        char master_dev_name[UBAGG_MAX_DEV_NAME_LEN];
        char slave_dev_name[URMA_UBAGG_DEV_MAX_NUM][UBAGG_MAX_DEV_NAME_LEN];
        urma_eid_t eid;
        struct urma_device_cap dev_cap;
    } in;
};

struct ubagg_rmv_dev {
    struct {
        char master_dev_name[UBAGG_MAX_DEV_NAME_LEN];
    } in;
};
#endif // UBAGG_CLI_H