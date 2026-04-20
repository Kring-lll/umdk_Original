/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2025. All rights reserved.
 * Description: uoip — JFC (completion queue) interface
 */

#ifndef UOIP_U_JFC_H
#define UOIP_U_JFC_H

#include "urma_provider.h"

urma_jfce_t *uoip_u_create_jfce(urma_context_t *ctx);
urma_status_t uoip_u_delete_jfce(urma_jfce_t *jfce);

urma_jfc_t *uoip_u_create_jfc(urma_context_t *ctx, urma_jfc_cfg_t *cfg);
urma_status_t uoip_u_delete_jfc(urma_jfc_t *jfc);

int uoip_u_poll_jfc(urma_jfc_t *jfc, int cr_cnt, urma_cr_t *cr);
urma_status_t uoip_u_rearm_jfc(urma_jfc_t *jfc, bool solicited_only);
int uoip_u_wait_jfc(urma_jfce_t *jfce, uint32_t jfc_cnt,
                    int time_out, urma_jfc_t *jfc[]);
void uoip_u_ack_jfc(urma_jfc_t *jfc[], uint32_t nevents[], uint32_t jfc_cnt);

#endif /* UOIP_U_JFC_H */
