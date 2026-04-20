/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2025. All rights reserved.
 * Description: uoip — Jetty implementation
 *
 * A Jetty is a bidirectional endpoint: it wraps an internal JFS for
 * sending and either an owned JFR or a user-supplied shared JFR for
 * receiving.
 */

#include "uoip_u_jetty.h"
#include "uoip_u.h"
#include "uoip_u_log.h"
#include "uoip_u_jfs.h"
#include "uoip_u_jfr.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

urma_jetty_t *uoip_u_create_jetty(urma_context_t *ctx,
                                    urma_jetty_cfg_t *cfg)
{
    if (!ctx || !cfg) {
        errno = EINVAL;
        return NULL;
    }

    struct uoip_jetty *uj = calloc(1, sizeof(*uj));
    if (!uj)
        return NULL;

    /* Create internal JFS */
    uj->ujfs = to_uoip_jfs(uoip_u_create_jfs(ctx, &cfg->jfs_cfg));
    if (!uj->ujfs)
        goto err_jfs;

    /* Shared JFR path: user passes an existing JFR */
    if (cfg->flag.bs.share_jfr && cfg->shared.jfr) {
        uj->shared_jfr = cfg->shared.jfr;
        uj->ujfr       = NULL;
    } else {
        /* Owned JFR: build a jfr_cfg from jetty's jfs send jfc */
        urma_jfr_cfg_t jfr_cfg = {
            .depth      = cfg->jfs_cfg.depth,
            .trans_mode = cfg->jfs_cfg.trans_mode,
            .jfc        = cfg->jfs_cfg.jfc,  /* share send JFC for recv too */
        };
        if (cfg->flag.bs.share_jfr == 0 && cfg->shared.jfc)
            jfr_cfg.jfc = cfg->shared.jfc;

        uj->ujfr = to_uoip_jfr(uoip_u_create_jfr(ctx, &jfr_cfg));
        if (!uj->ujfr)
            goto err_jfr;
        uj->shared_jfr = NULL;
    }

    /* Fill urma_jetty_t header */
    uj->jetty.urma_ctx  = ctx;
    uj->jetty.jetty_cfg = *cfg;
    uj->jetty.jetty_id.id = uoip_alloc_id();
    uoip_ipv4_to_eid(g_uoip_local_ipv4, &uj->jetty.jetty_id.eid);
    pthread_mutex_init(&uj->jetty.event_mutex, NULL);
    pthread_cond_init(&uj->jetty.event_cond, NULL);

    return &uj->jetty;

err_jfr:
    uoip_u_delete_jfs(&uj->ujfs->jfs);
err_jfs:
    free(uj);
    return NULL;
}

urma_status_t uoip_u_delete_jetty(urma_jetty_t *jetty)
{
    if (!jetty)
        return URMA_EINVAL;

    struct uoip_jetty *uj = to_uoip_jetty(jetty);

    if (uj->ujfr)
        uoip_u_delete_jfr(&uj->ujfr->jfr);

    if (uj->ujfs)
        uoip_u_delete_jfs(&uj->ujfs->jfs);

    pthread_mutex_destroy(&jetty->event_mutex);
    pthread_cond_destroy(&jetty->event_cond);
    free(uj);
    return URMA_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  import / bind (RC-mode connection setup)                           */
/* ------------------------------------------------------------------ */

urma_target_jetty_t *uoip_u_import_jetty(urma_context_t *ctx,
                                           urma_rjetty_t *rjetty,
                                           urma_token_t *token)
{
    if (!ctx || !rjetty)
        return NULL;

    struct uoip_target *ut = calloc(1, sizeof(*ut));
    if (!ut)
        return NULL;

    /* Use the jetty_id.id as the remote TCP port (same encoding as JFR) */
    uint32_t remote_ip   = uoip_eid_to_ipv4(&rjetty->jetty_id.eid);
    uint16_t remote_port = (uint16_t)rjetty->jetty_id.id;

    ut->conn_fd = uoip_tcp_connect(remote_ip, htons(remote_port));
    if (ut->conn_fd < 0) {
        free(ut);
        return NULL;
    }

    ut->tjetty.urma_ctx   = ctx;
    ut->tjetty.id         = rjetty->jetty_id;
    ut->tjetty.trans_mode = rjetty->trans_mode;
    ut->tjetty.flag       = rjetty->flag;
    ut->tjetty.type       = URMA_JETTY;
    return &ut->tjetty;
}

urma_status_t uoip_u_unimport_jetty(urma_target_jetty_t *tjetty)
{
    return uoip_u_unimport_jfr(tjetty);  /* same structure */
}

urma_status_t uoip_u_advise_jetty(urma_jetty_t *jetty,
                                    urma_target_jetty_t *tjetty)
{
    (void)jetty;
    (void)tjetty;
    return URMA_SUCCESS;
}

urma_status_t uoip_u_unadvise_jetty(urma_jetty_t *jetty,
                                      urma_target_jetty_t *tjetty)
{
    (void)jetty;
    (void)tjetty;
    return URMA_SUCCESS;
}

urma_status_t uoip_u_bind_jetty(urma_jetty_t *jetty,
                                  urma_target_jetty_t *tjetty)
{
    if (!jetty || !tjetty)
        return URMA_EINVAL;
    jetty->remote_jetty = tjetty;
    return URMA_SUCCESS;
}

urma_status_t uoip_u_unbind_jetty(urma_jetty_t *jetty)
{
    if (!jetty)
        return URMA_EINVAL;
    jetty->remote_jetty = NULL;
    return URMA_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  post send/recv WR — delegate to internal JFS / JFR               */
/* ------------------------------------------------------------------ */

urma_status_t uoip_u_post_jetty_send_wr(urma_jetty_t *jetty,
                                          urma_jfs_wr_t *wr,
                                          urma_jfs_wr_t **bad_wr)
{
    if (!jetty)
        return URMA_EINVAL;
    struct uoip_jetty *uj = to_uoip_jetty(jetty);
    if (!uj->ujfs)
        return URMA_FAIL;
    return uoip_u_post_jfs_wr(&uj->ujfs->jfs, wr, bad_wr);
}

urma_status_t uoip_u_post_jetty_recv_wr(urma_jetty_t *jetty,
                                          urma_jfr_wr_t *wr,
                                          urma_jfr_wr_t **bad_wr)
{
    if (!jetty)
        return URMA_EINVAL;
    struct uoip_jetty *uj = to_uoip_jetty(jetty);
    urma_jfr_t *jfr = uj->shared_jfr ? uj->shared_jfr : &uj->ujfr->jfr;
    if (!jfr)
        return URMA_FAIL;
    return uoip_u_post_jfr_wr(jfr, wr, bad_wr);
}
