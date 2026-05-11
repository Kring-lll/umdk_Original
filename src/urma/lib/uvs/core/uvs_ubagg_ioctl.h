/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: uvs ubagg ioctl header file
 * Author: Jiajun Liu
 * Create: 2025-06-05
 * Note:
 * History:
 */
#ifndef UVS_UBAGG_IOCTL_H
#define UVS_UBAGG_IOCTL_H
#include "uvs_api.h"
#include "uvs_types.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>

#define UVS_MAX_DEV_NAME_LEN 64
#define UVS_MAIN_PRIMARY_EID_UE_BATCH_MAX 4096

typedef struct uvs_main_primary_eid_ue_entry {
    uvs_eid_t main_primary_eid;
    uvs_eid_t primary_eid;
    uvs_eid_t port_eid[PORT_NUM];
    uint16_t port_bitmap;
    uint8_t prefix_len;
    uint8_t reserved;
} uvs_main_primary_eid_ue_entry_t;

typedef enum uvs_ubagg_cmd {
    UVS_UBAGG_CMD_ADD_DEV = 1,
    UVS_UBAGG_CMD_RMV_DEV,
    UVS_UBAGG_CMD_SET_TOPO_INFO,
    UVS_UBAGG_CMD_CREATE_DEV,
    UVS_UBAGG_CMD_DELETE_DEV,
    UVS_UBAGG_CMD_GET_DEV_NAME,
    UVS_UBAGG_CMD_DELETE_TOPO_INFO,
} uvs_ubagg_cmd_t;

struct uvs_ubagg_cmd_hdr {
    uint32_t command;
    uint32_t args_len;
    uint64_t args_addr;
};

#define UVS_UBAGG_CMD_MAGIC 'B'
#define UVS_UBAGG_CMD       _IOWR(UVS_UBAGG_CMD_MAGIC, 1, struct uvs_ubagg_cmd_hdr)

struct uvs_ubagg_create_dev_arg {
    struct {
        uvs_eid_t agg_eid;
        char dev_name[UVS_MAX_DEV_NAME_LEN];
    } in;
};

struct uvs_ubagg_delete_dev_arg {
    struct {
        uvs_eid_t agg_eid;
    } in;
};

struct uvs_ubagg_get_dev_name_arg {
    struct {
        uvs_eid_t eid;
    } in;
    struct {
        char dev_name[UVS_MAX_DEV_NAME_LEN];
    } out;
};

struct uvs_ubagg_set_topo_info {
    struct {
        void *topo;
        uint32_t topo_num;
    } in;
};

struct uvs_ubagg_delete_topo_info {
    struct {
        uint32_t reserved;
    } in;
};

int uvs_ubagg_ioctl_create_agg_dev(uvs_eid_t *agg_eid, const char *dev_name);
int uvs_ubagg_ioctl_delete_agg_dev(uvs_eid_t *agg_eid);
int uvs_ubagg_ioctl_get_dev_name_by_eid(uvs_eid_t *eid, char *buf, size_t len);
int uvs_ubagg_ioctl_set_topo(void *topo_info, int topo_num);
int uvs_ubagg_ioctl_delete_topo(void);
int uvs_ubcore_ioctl_get_topo(void *topo_map);
int uvs_ubcore_ioctl_set_topo(void *topo_info, int topo_num);
int uvs_ubcore_ioctl_delete_topo(void);
int uvs_ubcore_ioctl_insert_main_primary_eid(const uvs_main_primary_eid_entry_t *entry);
int uvs_ubcore_ioctl_del_main_primary_eid(const uvs_eid_t *prefix, uint8_t prefix_len);
int uvs_ubcore_ioctl_lookup_main_primary_eid(const uvs_eid_t *eid,
                                             uvs_eid_t *main_primary_eid);
int uvs_ubcore_ioctl_flush_main_primary_eid(void);
int uvs_ubcore_ioctl_insert_main_primary_eid_batch(const uvs_main_primary_eid_entry_t *entries,
                                                   uint32_t entry_num);
int uvs_ubcore_ioctl_insert_main_primary_eid_ue_batch(
    const uvs_main_primary_eid_ue_entry_t *entries, uint32_t entry_num);
int uvs_ubcore_ioctl_bench_main_primary_eid(const uvs_eid_t *eids, uint32_t eid_num,
                                            uint32_t loop,
                                            uvs_main_primary_eid_bench_result_t *result);
int uvs_ubcore_ioctl_get_route_list(const uvs_route_t *route, uvs_route_list_t *route_list);
int uvs_ubcore_ioctl_get_path_set(const uvs_eid_t *src_bondind_eid,
                                  const uvs_eid_t *dst_bonding_eid,
                                  enum uvs_tp_type tp_type, bool multi_path,
                                  uvs_path_set_t *uvs_path_set);

#endif // UVS_UBAGG_IOCTL_H
