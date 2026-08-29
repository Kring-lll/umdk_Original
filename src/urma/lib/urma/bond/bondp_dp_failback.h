/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bond provider failback helpers.
 */

#ifndef BONDP_DP_FAILBACK_H
#define BONDP_DP_FAILBACK_H

#include "bondp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

bool bondp_fb_need_switch_path(bondp_comp_t *bdp_comp);
uint32_t bondp_fb_switch_path(bondp_comp_t *bdp_comp);

/* Sentinel for "no second cold-standby path". */
#define BONDP_LAZY_IDX_NONE UINT32_MAX

/*
 * Payload for a lazy-create task: BOTH cold-standby cross-IODIE pjettys are
 * created by ONE worker event from the cfg template copied at failover time,
 * then in-flight WRs stranded on invalid paths are resent, alternating
 * between the two new paths.
 */
typedef struct bondp_lazy_create_info {
    urma_jetty_cfg_t jetty_cfg; /* cfg template copied from the vjetty (user cfg) */
    uint64_t base_wr_id;        /* wr id used as the send_wr_buf scan anchor */
    uint32_t old_send_idx;      /* failed path (diagnostics) */
    uint32_t old_target_idx;
    uint32_t new_send_idx;      /* cold-standby path 1 (local slot) */
    uint32_t new_target_idx;
    uint32_t new_send_idx2;     /* cold-standby path 2, BONDP_LAZY_IDX_NONE if absent */
    uint32_t new_target_idx2;
} bondp_lazy_create_info_t;

int bondp_fb_add_task(bondp_context_t *bond_ctx, uint32_t vjetty_id, uint32_t pjetty_idx);
int bondp_fb_add_lazy_task(bondp_context_t *bond_ctx, uint32_t vjetty_id, uint32_t pjetty_idx,
                           const bondp_lazy_create_info_t *info);
int bondp_fb_cancel_tasks(bondp_context_t *bond_ctx, uint32_t vjetty_id);

int bondp_fb_init(bondp_context_t *bond_ctx);
int bondp_fb_uninit(bondp_context_t *bond_ctx);

#ifdef __cplusplus
}
#endif

#endif // BONDP_DP_FAILBACK_H
