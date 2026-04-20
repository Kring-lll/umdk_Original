/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2025. All rights reserved.
 * Description: uoip — Jetty (combined send+recv queue) interface
 */

#ifndef UOIP_U_JETTY_H
#define UOIP_U_JETTY_H

#include "urma_provider.h"

urma_jetty_t *uoip_u_create_jetty(urma_context_t *ctx,
                                    urma_jetty_cfg_t *cfg);
urma_status_t uoip_u_delete_jetty(urma_jetty_t *jetty);

urma_target_jetty_t *uoip_u_import_jetty(urma_context_t *ctx,
                                           urma_rjetty_t *rjetty,
                                           urma_token_t *token);
urma_status_t uoip_u_unimport_jetty(urma_target_jetty_t *tjetty);

urma_status_t uoip_u_advise_jetty(urma_jetty_t *jetty,
                                    urma_target_jetty_t *tjetty);
urma_status_t uoip_u_unadvise_jetty(urma_jetty_t *jetty,
                                      urma_target_jetty_t *tjetty);
urma_status_t uoip_u_bind_jetty(urma_jetty_t *jetty,
                                  urma_target_jetty_t *tjetty);
urma_status_t uoip_u_unbind_jetty(urma_jetty_t *jetty);

urma_status_t uoip_u_post_jetty_send_wr(urma_jetty_t *jetty,
                                          urma_jfs_wr_t *wr,
                                          urma_jfs_wr_t **bad_wr);
urma_status_t uoip_u_post_jetty_recv_wr(urma_jetty_t *jetty,
                                          urma_jfr_wr_t *wr,
                                          urma_jfr_wr_t **bad_wr);

#endif /* UOIP_U_JETTY_H */
