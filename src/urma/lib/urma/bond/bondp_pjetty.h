/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Physical jetty references and deferred reclamation.
 */
#ifndef BONDP_PJETTY_H
#define BONDP_PJETTY_H

#include "bondp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

bondp_pjetty_ref_t *bondp_pjetty_create(urma_context_t *ctx, urma_jetty_cfg_t *cfg);
/* A successful get holds the component until put completes reclamation. */
bondp_pjetty_ref_t *bondp_pjetty_get(bondp_comp_t *comp, uint32_t idx);
void bondp_pjetty_put(bondp_comp_t *comp, bondp_pjetty_ref_t *ref);
void bondp_pjetty_retire(bondp_comp_t *comp, bondp_pjetty_ref_t *ref);
/* Caller must hold a component reference or have excluded/cancelled all users. */
int bondp_pjetty_reap(bondp_comp_t *comp);

#ifdef __cplusplus
}
#endif

#endif
