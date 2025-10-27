/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: Bonding provider WR utils header
 * Author: Ma Chuan
 * Create: 2025-02-21
 * Note:
 * History: 2025-02-21
 */
#ifndef WR_UTILS_H
#define WR_UTILS_H
#include "urma_types.h"
#ifdef __cplusplus
extern "C" {
#endif
urma_jfs_wr_t *deepcopy_jfs_wr(const urma_jfs_wr_t *wr);
urma_jfs_wr_t *deepcopy_jfs_wr_and_add_hdr_sge(const urma_jfs_wr_t *wr);
int delete_copied_jfs_wr(urma_jfs_wr_t *wr);
urma_jfr_wr_t *deepcopy_jfr_wr(const urma_jfr_wr_t *wr);
urma_jfr_wr_t *deepcopy_jfr_wr_and_add_hdr_sge(const urma_jfr_wr_t *wr);
int delete_copied_jfr_wr(urma_jfr_wr_t *wr);
#ifdef __cplusplus
}
#endif
#endif // __WR_UTILS_H__