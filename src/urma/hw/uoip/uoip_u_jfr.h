/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2025. All rights reserved.
 * Description: uoip — JFR (receive queue) interface
 */

#ifndef UOIP_U_JFR_H
#define UOIP_U_JFR_H

#include "urma_provider.h"

urma_jfr_t *uoip_u_create_jfr(urma_context_t *ctx, urma_jfr_cfg_t *cfg);
urma_status_t uoip_u_delete_jfr(urma_jfr_t *jfr);

urma_status_t uoip_u_post_jfr_wr(urma_jfr_t *jfr, urma_jfr_wr_t *wr,
                                   urma_jfr_wr_t **bad_wr);

urma_target_jetty_t *uoip_u_import_jfr(urma_context_t *ctx,
                                         urma_rjfr_t *rjfr,
                                         urma_token_t *token);
urma_status_t uoip_u_unimport_jfr(urma_target_jetty_t *tjfr);

urma_status_t uoip_u_advise_jfr(urma_jfs_t *jfs,
                                  urma_target_jetty_t *tjfr);
urma_status_t uoip_u_unadvise_jfr(urma_jfs_t *jfs,
                                    urma_target_jetty_t *tjfr);

#endif /* UOIP_U_JFR_H */
