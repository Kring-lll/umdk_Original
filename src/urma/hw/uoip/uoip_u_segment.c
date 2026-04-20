/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2025. All rights reserved.
 * Description: uoip — memory segment registration
 *
 * For a virtual software driver there is no hardware pinning.
 * We simply record the virtual address range and hand out a token.
 */

#include "uoip_u_segment.h"
#include "uoip_u.h"
#include "uoip_u_log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

urma_target_seg_t *uoip_u_register_seg(urma_context_t *ctx,
                                        urma_seg_cfg_t *cfg)
{
    if (!ctx || !cfg) {
        errno = EINVAL;
        return NULL;
    }

    struct uoip_seg *us = calloc(1, sizeof(*us));
    if (!us)
        return NULL;

    us->va  = cfg->va;
    us->len = cfg->len;

    /* Populate the embedded urma_target_seg_t */
    us->tseg.urma_ctx     = ctx;
    us->tseg.seg.ubva.va  = cfg->va;
    us->tseg.seg.len      = cfg->len;
    us->tseg.seg.token_id = uoip_alloc_id();
    us->tseg.user_ctx     = cfg->user_ctx;

    UOIP_LOG_INFO("register_seg: va=0x%lx len=%lu token_id=%u\n",
                  (unsigned long)cfg->va, (unsigned long)cfg->len,
                  us->tseg.seg.token_id);
    return &us->tseg;
}

urma_status_t uoip_u_unregister_seg(urma_target_seg_t *tseg)
{
    if (!tseg)
        return URMA_EINVAL;
    struct uoip_seg *us = to_uoip_seg(tseg);
    free(us);
    return URMA_SUCCESS;
}
