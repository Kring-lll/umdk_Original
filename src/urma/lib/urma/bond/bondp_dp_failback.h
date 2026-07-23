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

typedef enum {
    BONDP_FB_PRIMARY = 0,
    BONDP_FB_BACKUP  = 1,
} bondp_fb_which_t;

void bondp_fb_handle_notify_nl_msg(struct nlattr *attrs[]);
void bondp_fb_handle_done_nl_msg(struct nlattr *attrs[]);

int bondp_fb_add_task(bondp_context_t *bond_ctx, uint32_t vjetty_id, uint32_t pjetty_idx,
                      bondp_fb_which_t which);
void bondp_fb_cancel_tasks(bondp_context_t *bond_ctx, uint32_t vjetty_id);

void bondp_switch_to_backup_pjetty(bondp_comp_t *bdp_comp, uint32_t pjetty_idx);

int bondp_fb_init(bondp_context_t *bond_ctx);
void bondp_fb_uninit(bondp_context_t *bond_ctx);

#ifdef __cplusplus
}
#endif

#endif // BONDP_DP_FAILBACK_H
