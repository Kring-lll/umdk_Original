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

int bondp_fb_add_task(bondp_context_t *bond_ctx, uint32_t vjetty_id, uint32_t pjetty_idx);
void bondp_fb_cancel_tasks(bondp_context_t *bond_ctx, uint32_t vjetty_id);

bool bondp_fb_cr_from_inactive_twin(bondp_comp_t *bdp_comp, uint32_t pjetty_idx, uint32_t cr_local_id);
bool bondp_fb_swap_to_standby(bondp_comp_t *bdp_comp, uint32_t pjetty_idx);

int bondp_fb_init(bondp_context_t *bond_ctx);
void bondp_fb_uninit(bondp_context_t *bond_ctx);

#ifdef __cplusplus
}
#endif

#endif // BONDP_DP_FAILBACK_H
