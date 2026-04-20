/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2025. All rights reserved.
 * Description: uoip — JFS (send queue) interface
 */

#ifndef UOIP_U_JFS_H
#define UOIP_U_JFS_H

#include "urma_provider.h"

urma_jfs_t *uoip_u_create_jfs(urma_context_t *ctx, urma_jfs_cfg_t *cfg);
urma_status_t uoip_u_delete_jfs(urma_jfs_t *jfs);

urma_status_t uoip_u_post_jfs_wr(urma_jfs_t *jfs, urma_jfs_wr_t *wr,
                                   urma_jfs_wr_t **bad_wr);

#endif /* UOIP_U_JFS_H */
