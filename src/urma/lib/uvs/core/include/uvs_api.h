/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2025. All rights reserved.
 * Description: UVS API
 * Author: Zheng Hongqin
 * Create: 2023-10-11
 * Note:
 * History:
 */

#ifndef UVS_API_H
#define UVS_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * UVS set topo info which gets from MXE module.
 * @param[in] topo: topo info of one bonding device
 * @param[in] topo_num: number of bonding devices
 * Return: 0 on success, other value on error
 */
int uvs_set_topo_info(void *topo, uint32_t topo_num);

#ifdef __cplusplus
}
#endif

#endif