/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: Bond datapth ops header file
 * Author: Ma Chuan
 * Create: 2025-02-12
 * Note:
 * History: 2025-02-12   Create File
 */

#ifndef BONDP_DATAPATH_H
#define BONDP_DATAPATH_H

#include "urma_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct bondp_comp;
struct jfs_wr_entry;
struct bondp_lazy_create_info;

int bondp_resend_jfs_wr(struct bondp_comp *bdp_comp, struct jfs_wr_entry *wr_entry,
                        int send_idx, int target_idx);

/*
 * Create a not-yet-built cross-IODIE pjetty on the bond worker thread (heavy
 * ioctl + HW resource alloc, must never run in the poll thread spinlock),
 * register its id mapping, publish valid[idx]=true and resend the WRs
 * stranded on invalid paths to the new path. Implemented in bondp_datapath.c.
 */
int bondp_lazy_create_and_resend(struct bondp_comp *bdp_jetty, uint32_t pjetty_idx,
                                 const struct bondp_lazy_create_info *info);

urma_status_t bondp_post_jetty_send_wr(urma_jetty_t *jetty, urma_jfs_wr_t *wr, urma_jfs_wr_t **bad_wr);
urma_status_t bondp_post_jfs_wr(urma_jfs_t *jfs, urma_jfs_wr_t *wr, urma_jfs_wr_t **bad_wr);

urma_status_t bondp_post_jetty_recv_wr(urma_jetty_t *jetty, urma_jfr_wr_t *wr, urma_jfr_wr_t **bad_wr);
urma_status_t bondp_post_jfr_wr(urma_jfr_t *jfr, urma_jfr_wr_t *wr, urma_jfr_wr_t **bad_wr);

int bondp_poll_jfc(urma_jfc_t *jfc, int cr_cnt, urma_cr_t *cr);
int bondp_flush_jetty(urma_jetty_t *jetty, int cr_cnt, urma_cr_t *cr);

#ifdef __cplusplus
}
#endif
#endif // BONDP_DATAPATH_H
