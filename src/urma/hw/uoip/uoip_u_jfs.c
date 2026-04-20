/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2025. All rights reserved.
 * Description: uoip — JFS (send queue) implementation
 *
 * post_jfs_wr supports SEND/SEND_IMM opcodes.
 * WRITE/READ/atomic opcodes return URMA_FAIL (not implemented).
 *
 * For SEND: iterate the source SGE list, gather the data, send it as one
 * TCP frame to the target JFR's socket (established by import_jfr), then
 * immediately post a send-completion CQE to the local JFC.
 */

#include "uoip_u_jfs.h"
#include "uoip_u.h"
#include "uoip_u_log.h"
#include "uoip_u_net.h"
#include "uoip_u_jfc.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  create / delete                                                    */
/* ------------------------------------------------------------------ */

urma_jfs_t *uoip_u_create_jfs(urma_context_t *ctx, urma_jfs_cfg_t *cfg)
{
    if (!ctx || !cfg || !cfg->jfc) {
        errno = EINVAL;
        return NULL;
    }

    struct uoip_jfs *ujfs = calloc(1, sizeof(*ujfs));
    if (!ujfs)
        return NULL;

    ujfs->jfs.urma_ctx  = ctx;
    ujfs->jfs.jfs_cfg   = *cfg;
    ujfs->jfs.jfs_id.id  = uoip_alloc_id();
    uoip_ipv4_to_eid(g_uoip_local_ipv4, &ujfs->jfs.jfs_id.eid);
    pthread_mutex_init(&ujfs->jfs.event_mutex, NULL);
    pthread_cond_init(&ujfs->jfs.event_cond, NULL);

    ujfs->ujfc = to_uoip_jfc(cfg->jfc);
    return &ujfs->jfs;
}

urma_status_t uoip_u_delete_jfs(urma_jfs_t *jfs)
{
    if (!jfs)
        return URMA_EINVAL;
    struct uoip_jfs *ujfs = to_uoip_jfs(jfs);
    pthread_mutex_destroy(&jfs->event_mutex);
    pthread_cond_destroy(&jfs->event_cond);
    free(ujfs);
    return URMA_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Gather SGE list into a flat buffer                                 */
/* ------------------------------------------------------------------ */

static void *gather_sg(const urma_sg_t *sg, uint32_t *total_len)
{
    *total_len = 0;
    for (uint32_t i = 0; i < sg->num_sge; i++)
        *total_len += sg->sge[i].len;

    if (*total_len == 0)
        return NULL;

    void *buf = malloc(*total_len);
    if (!buf)
        return NULL;

    char *p = (char *)buf;
    for (uint32_t i = 0; i < sg->num_sge; i++) {
        if (sg->sge[i].len == 0)
            continue;
        memcpy(p, (const void *)(uintptr_t)sg->sge[i].addr, sg->sge[i].len);
        p += sg->sge[i].len;
    }
    return buf;
}

/* ------------------------------------------------------------------ */
/*  post_jfs_wr: send data over TCP                                   */
/* ------------------------------------------------------------------ */

urma_status_t uoip_u_post_jfs_wr(urma_jfs_t *jfs, urma_jfs_wr_t *wr,
                                   urma_jfs_wr_t **bad_wr)
{
    if (!jfs || !wr)
        return URMA_EINVAL;

    struct uoip_jfs *ujfs = to_uoip_jfs(jfs);
    urma_jfs_wr_t   *cur  = wr;

    while (cur) {
        urma_cr_status_t cr_status = URMA_CR_SUCCESS;

        switch (cur->opcode) {
        case URMA_OPC_SEND:
        case URMA_OPC_SEND_IMM: {
            if (!cur->tjetty) {
                UOIP_LOG_ERR("post_jfs_wr: NULL tjetty\n");
                cr_status = URMA_CR_LOC_OPERATION_ERR;
                break;
            }
            struct uoip_target *ut = to_uoip_target(cur->tjetty);
            if (ut->conn_fd < 0) {
                UOIP_LOG_ERR("post_jfs_wr: conn_fd not connected\n");
                cr_status = URMA_CR_LOC_OPERATION_ERR;
                break;
            }

            uint32_t total = 0;
            void *buf = gather_sg(&cur->send.src, &total);
            int rc = uoip_frame_send(ut->conn_fd, buf, total);
            free(buf);

            if (rc < 0) {
                UOIP_LOG_ERR("post_jfs_wr: TCP send failed\n");
                cr_status = URMA_CR_LOC_OPERATION_ERR;
            }
            break;
        }
        default:
            UOIP_LOG_WARN("post_jfs_wr: unsupported opcode 0x%x\n",
                          (unsigned)cur->opcode);
            cr_status = URMA_CR_UNSUPPORTED_OPCODE_ERR;
            break;
        }

        /* Post send-completion CQE */
        urma_cr_t cr = {
            .status         = cr_status,
            .user_ctx       = cur->user_ctx,
            .opcode         = URMA_CR_OPC_SEND,
            .completion_len = 0,
        };
        cr.flag.bs.s_r = 0;  /* send */

        if (uoip_jfc_post_cqe(ujfs->ujfc, &cr) < 0) {
            if (bad_wr)
                *bad_wr = cur;
            return URMA_FAIL;
        }

        if (cr_status != URMA_CR_SUCCESS) {
            if (bad_wr)
                *bad_wr = cur;
            return URMA_FAIL;
        }

        cur = cur->next;
    }
    return URMA_SUCCESS;
}
