/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2025. All rights reserved.
 * Description: uoip — provider ops, device registration, context lifecycle
 */

#include "uoip_u_ops.h"
#include "uoip_u.h"
#include "uoip_u_log.h"
#include "uoip_u_net.h"
#include "uoip_u_jfc.h"
#include "uoip_u_jfr.h"
#include "uoip_u_jfs.h"
#include "uoip_u_jetty.h"
#include "uoip_u_segment.h"

/* Private headers from liburma core (in the include search path) */
#include "urma_private.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Globals                                                            */
/* ------------------------------------------------------------------ */

urma_sysfs_dev_t *g_uoip_sysfs_dev = NULL;
urma_device_t    *g_uoip_device    = NULL;
uint32_t          g_uoip_local_ipv4 = 0;
atomic_uint       g_uoip_id_seq     = 0;

/* ------------------------------------------------------------------ */
/*  get_async_event stub                                               */
/* ------------------------------------------------------------------ */

static urma_status_t uoip_u_get_async_event(urma_context_t *ctx,
                                              urma_async_event_t *event)
{
    (void)ctx;
    (void)event;
    return URMA_EAGAIN;
}

static void uoip_u_ack_async_event(urma_async_event_t *event)
{
    (void)event;
}

/* ------------------------------------------------------------------ */
/*  user_ctl stub                                                      */
/* ------------------------------------------------------------------ */

static int uoip_u_user_ctl(urma_context_t *ctx,
                             urma_user_ctl_in_t *in,
                             urma_user_ctl_out_t *out)
{
    (void)ctx;
    (void)in;
    (void)out;
    return -1;
}

/* ------------------------------------------------------------------ */
/*  urma_ops_t — data-plane + control-plane ops                       */
/* ------------------------------------------------------------------ */

static urma_ops_t g_uoip_ops = {
    .name = "UOIP_OPS",

    /* JFC */
    .create_jfc = uoip_u_create_jfc,
    .delete_jfc = uoip_u_delete_jfc,

    /* JFS */
    .create_jfs = uoip_u_create_jfs,
    .delete_jfs = uoip_u_delete_jfs,

    /* JFR */
    .create_jfr       = uoip_u_create_jfr,
    .delete_jfr       = uoip_u_delete_jfr,
    .import_jfr       = uoip_u_import_jfr,
    .unimport_jfr     = uoip_u_unimport_jfr,
    .advise_jfr       = uoip_u_advise_jfr,
    .unadvise_jfr     = uoip_u_unadvise_jfr,

    /* Jetty */
    .create_jetty     = uoip_u_create_jetty,
    .delete_jetty     = uoip_u_delete_jetty,
    .import_jetty     = uoip_u_import_jetty,
    .unimport_jetty   = uoip_u_unimport_jetty,
    .advise_jetty     = uoip_u_advise_jetty,
    .unadvise_jetty   = uoip_u_unadvise_jetty,
    .bind_jetty       = uoip_u_bind_jetty,
    .unbind_jetty     = uoip_u_unbind_jetty,

    /* JFCE */
    .create_jfce = uoip_u_create_jfce,
    .delete_jfce = uoip_u_delete_jfce,

    /* Segment */
    .register_seg   = uoip_u_register_seg,
    .unregister_seg = uoip_u_unregister_seg,

    /* Events */
    .get_async_event = uoip_u_get_async_event,
    .ack_async_event = uoip_u_ack_async_event,

    /* Control */
    .user_ctl = uoip_u_user_ctl,

    /* Data-plane */
    .post_jfs_wr          = uoip_u_post_jfs_wr,
    .post_jfr_wr          = uoip_u_post_jfr_wr,
    .post_jetty_send_wr   = uoip_u_post_jetty_send_wr,
    .post_jetty_recv_wr   = uoip_u_post_jetty_recv_wr,
    .poll_jfc             = uoip_u_poll_jfc,
    .rearm_jfc            = uoip_u_rearm_jfc,
    .wait_jfc             = uoip_u_wait_jfc,
    .ack_jfc              = uoip_u_ack_jfc,
};

/* ------------------------------------------------------------------ */
/*  provider_ops: init / uninit / query_device                        */
/* ------------------------------------------------------------------ */

static urma_status_t uoip_u_init(urma_init_attr_t *conf)
{
    (void)conf;

    /* Detect local IP once */
    g_uoip_local_ipv4 = uoip_get_local_ipv4();

    /* Allocate the virtual sysfs device and its urma_device_t */
    g_uoip_sysfs_dev = calloc(1, sizeof(urma_sysfs_dev_t));
    g_uoip_device    = calloc(1, sizeof(urma_device_t));
    if (!g_uoip_sysfs_dev || !g_uoip_device)
        goto err_alloc;

    /* Fill sysfs_dev */
    strncpy(g_uoip_sysfs_dev->dev_name,    "uoip0", URMA_MAX_NAME - 1);
    strncpy(g_uoip_sysfs_dev->driver_name, "uoip",  URMA_MAX_NAME - 1);
    g_uoip_sysfs_dev->transport_type = URMA_TRANSPORT_UB;
    g_uoip_sysfs_dev->flag           = URMA_SYSFS_DEV_FLAG_DRIVER_CREATED;

    /* Advertise enough capability for typical URMA applications */
    urma_device_cap_t *cap = &g_uoip_sysfs_dev->dev_attr.dev_cap;
    cap->max_eid_cnt   = 1;
    cap->max_jfc       = 1024;
    cap->max_jfs       = 1024;
    cap->max_jfr       = 1024;
    cap->max_jetty     = 1024;
    cap->max_jfc_depth = UOIP_JFC_MAX_DEPTH;
    cap->max_jfs_depth = UOIP_JFR_MAX_DEPTH;
    cap->max_jfr_depth = UOIP_JFR_MAX_DEPTH;
    cap->max_jfs_sge   = 16;
    cap->max_jfr_sge   = 16;
    cap->max_msg_size  = UOIP_MAX_PAYLOAD;
    cap->page_size_cap = 4096;

    /* Pre-fill the urma_device_t fields not set by urma_register_sysfs_dev */
    strncpy(g_uoip_device->name, "uoip0", URMA_MAX_NAME - 1);
    snprintf(g_uoip_device->path, URMA_MAX_PATH, "/dev/uburma/uoip0");
    g_uoip_device->type     = URMA_TRANSPORT_UB;
    g_uoip_device->sysfs_dev = g_uoip_sysfs_dev;

    /* Cross-link sysfs_dev ↔ device */
    g_uoip_sysfs_dev->urma_device = g_uoip_device;

    /* Register with liburma: urma_register_sysfs_dev sets device->ops */
    if (urma_register_sysfs_dev(g_uoip_sysfs_dev) != 0) {
        UOIP_LOG_ERR("urma_register_sysfs_dev failed\n");
        goto err_alloc;
    }

    UOIP_LOG_INFO("uoip provider initialised (device uoip0)\n");
    return URMA_SUCCESS;

err_alloc:
    free(g_uoip_device);
    free(g_uoip_sysfs_dev);
    g_uoip_device    = NULL;
    g_uoip_sysfs_dev = NULL;
    return URMA_FAIL;
}

static urma_status_t uoip_u_uninit(void)
{
    /* The sysfs_dev/device are owned by liburma's g_dev_list after
     * registration; we leave them there to avoid use-after-free.
     * In a production driver a proper unregister path would be added. */
    UOIP_LOG_INFO("uoip provider uninitialised\n");
    return URMA_SUCCESS;
}

static urma_status_t uoip_u_query_device(urma_device_t *dev,
                                           urma_device_attr_t *attr)
{
    if (!dev || !attr)
        return URMA_EINVAL;
    *attr = dev->sysfs_dev->dev_attr;
    return URMA_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  create_context / delete_context                                   */
/* ------------------------------------------------------------------ */

static urma_context_t *uoip_u_create_context(urma_device_t *dev,
                                               uint32_t eid_index,
                                               int dev_fd)
{
    (void)eid_index;
    (void)dev_fd;  /* -1 for DRIVER_CREATED devices */

    struct uoip_ctx *uctx = calloc(1, sizeof(*uctx));
    if (!uctx)
        return NULL;

    uctx->local_ipv4           = g_uoip_local_ipv4;
    uctx->urma_ctx.dev         = dev;
    uctx->urma_ctx.ops         = &g_uoip_ops;
    uctx->urma_ctx.dev_fd      = -1;
    uctx->urma_ctx.async_fd    = -1;
    pthread_mutex_init(&uctx->urma_ctx.mutex, NULL);

    /* Set EID from local IPv4 — the framework will overwrite ctx->eid with
     * zeros (DRIVER_CREATED path), so we also cache it in uoip_ctx and use
     * g_uoip_local_ipv4 directly when building JFR IDs. */
    uoip_ipv4_to_eid(g_uoip_local_ipv4, &uctx->urma_ctx.eid);

    UOIP_LOG_INFO("context created for device %s\n", dev->name);
    return &uctx->urma_ctx;
}

static urma_status_t uoip_u_delete_context(urma_context_t *ctx)
{
    if (!ctx)
        return URMA_EINVAL;
    struct uoip_ctx *uctx = to_uoip_ctx(ctx);
    pthread_mutex_destroy(&ctx->mutex);
    free(uctx);
    return URMA_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  urma_provider_ops_t                                               */
/* ------------------------------------------------------------------ */

urma_provider_ops_t g_uoip_provider_ops = {
    .name  = "uoip",
    .attr  = {
        .version        = 1,
        .transport_type = URMA_TRANSPORT_UB,
    },
    .match_table   = NULL,   /* match by driver_name "uoip" */
    .init          = uoip_u_init,
    .uninit        = uoip_u_uninit,
    .query_device  = uoip_u_query_device,
    .create_context = uoip_u_create_context,
    .delete_context = uoip_u_delete_context,
};
