/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2025. All rights reserved.
 * Description: uoip — JFC (completion queue) implementation
 */

#include "uoip_u_jfc.h"
#include "uoip_u.h"
#include "uoip_u_log.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  JFCE (completion event fd)                                         */
/* ------------------------------------------------------------------ */

urma_jfce_t *uoip_u_create_jfce(urma_context_t *ctx)
{
    struct uoip_jfce *ujfce = calloc(1, sizeof(*ujfce));
    if (!ujfce)
        return NULL;

    int efd = eventfd(0, EFD_CLOEXEC | EFD_SEMAPHORE);
    if (efd < 0) {
        UOIP_LOG_ERR("eventfd: %s\n", strerror(errno));
        free(ujfce);
        return NULL;
    }

    ujfce->jfce.urma_ctx = ctx;
    ujfce->jfce.fd       = efd;
    return &ujfce->jfce;
}

urma_status_t uoip_u_delete_jfce(urma_jfce_t *jfce)
{
    if (!jfce)
        return URMA_EINVAL;
    struct uoip_jfce *ujfce = to_uoip_jfce(jfce);
    close(jfce->fd);
    free(ujfce);
    return URMA_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  JFC (completion queue)                                             */
/* ------------------------------------------------------------------ */

/* Round up to next power-of-two, minimum 2 */
static uint32_t round_pow2(uint32_t n)
{
    if (n < 2)
        return 2;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1;
}

/* Post one CQE — called from JFR recv thread and JFS post path */
int uoip_jfc_post_cqe(struct uoip_jfc *ujfc, const urma_cr_t *cr)
{
    pthread_mutex_lock(&ujfc->lock);

    uint32_t next_tail = (ujfc->tail + 1) & (ujfc->depth - 1);
    if (next_tail == ujfc->head) {
        pthread_mutex_unlock(&ujfc->lock);
        UOIP_LOG_WARN("JFC ring full (depth=%u)\n", ujfc->depth);
        return -1;
    }

    ujfc->ring[ujfc->tail] = *cr;
    ujfc->tail = next_tail;
    pthread_mutex_unlock(&ujfc->lock);

    /* Signal JFCE if one is associated */
    urma_jfce_t *jfce = ujfc->jfc.jfc_cfg.jfce;
    if (jfce && jfce->fd >= 0) {
        uint64_t one = 1;
        ssize_t ret = write(jfce->fd, &one, sizeof(one));
        (void)ret;  /* Suppress unused result warning */
    }
    return 0;
}

urma_jfc_t *uoip_u_create_jfc(urma_context_t *ctx, urma_jfc_cfg_t *cfg)
{
    if (!ctx || !cfg || cfg->depth == 0) {
        errno = EINVAL;
        return NULL;
    }

    struct uoip_jfc *ujfc = calloc(1, sizeof(*ujfc));
    if (!ujfc)
        return NULL;

    uint32_t depth = round_pow2(cfg->depth);
    if (depth > UOIP_JFC_MAX_DEPTH)
        depth = UOIP_JFC_MAX_DEPTH;

    ujfc->ring = calloc(depth, sizeof(urma_cr_t));
    if (!ujfc->ring) {
        free(ujfc);
        return NULL;
    }

    ujfc->depth = depth;
    pthread_mutex_init(&ujfc->lock, NULL);
    ujfc->event_fd = -1;  /* not used directly; JFCE is used instead */

    /* Fill urma_jfc_t header */
    ujfc->jfc.urma_ctx = ctx;
    ujfc->jfc.jfc_cfg  = *cfg;
    ujfc->jfc.jfc_id.id = uoip_alloc_id();
    ujfc->jfc.jfc_id.eid = ctx->eid;
    pthread_mutex_init(&ujfc->jfc.event_mutex, NULL);
    pthread_cond_init(&ujfc->jfc.event_cond, NULL);

    return &ujfc->jfc;
}

urma_status_t uoip_u_delete_jfc(urma_jfc_t *jfc)
{
    if (!jfc)
        return URMA_EINVAL;
    struct uoip_jfc *ujfc = to_uoip_jfc(jfc);
    pthread_mutex_destroy(&ujfc->lock);
    pthread_mutex_destroy(&jfc->event_mutex);
    pthread_cond_destroy(&jfc->event_cond);
    free(ujfc->ring);
    free(ujfc);
    return URMA_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  poll / wait / rearm / ack                                          */
/* ------------------------------------------------------------------ */

int uoip_u_poll_jfc(urma_jfc_t *jfc, int cr_cnt, urma_cr_t *cr)
{
    if (!jfc || cr_cnt <= 0 || !cr)
        return 0;

    struct uoip_jfc *ujfc = to_uoip_jfc(jfc);
    int polled = 0;

    pthread_mutex_lock(&ujfc->lock);
    while (polled < cr_cnt && ujfc->head != ujfc->tail) {
        cr[polled++] = ujfc->ring[ujfc->head];
        ujfc->head = (ujfc->head + 1) & (ujfc->depth - 1);
    }
    pthread_mutex_unlock(&ujfc->lock);
    return polled;
}

urma_status_t uoip_u_rearm_jfc(urma_jfc_t *jfc, bool solicited_only)
{
    (void)jfc;
    (void)solicited_only;
    return URMA_SUCCESS;
}

/*
 * wait_jfc: wait on the JFCE's eventfd until at least one CQE is available
 * in any of the jfc[] entries, or timeout expires.
 *
 * time_out < 0  → block indefinitely
 * time_out == 0 → non-blocking
 * time_out > 0  → milliseconds
 *
 * Returns number of JFCs with CQEs, or -1 on error.
 */
int uoip_u_wait_jfc(urma_jfce_t *jfce, uint32_t jfc_cnt,
                    int time_out, urma_jfc_t *jfc[])
{
    if (!jfce || jfce->fd < 0)
        return -1;

    struct epoll_event ev;
    int efd = epoll_create1(EPOLL_CLOEXEC);
    if (efd < 0)
        return -1;

    ev.events   = EPOLLIN;
    ev.data.fd  = jfce->fd;
    epoll_ctl(efd, EPOLL_CTL_ADD, jfce->fd, &ev);

    int nfds = epoll_wait(efd, &ev, 1, time_out);
    close(efd);

    if (nfds <= 0)
        return nfds;  /* 0 = timeout, -1 = error */

    /* Drain one token from the semaphore-mode eventfd */
    uint64_t val;
    ssize_t ret = read(jfce->fd, &val, sizeof(val));
    (void)ret;  /* Suppress unused result warning */

    /* Count JFCs that have CQEs */
    int ready = 0;
    for (uint32_t i = 0; i < jfc_cnt; i++) {
        if (!jfc[i])
            continue;
        struct uoip_jfc *ujfc = to_uoip_jfc(jfc[i]);
        pthread_mutex_lock(&ujfc->lock);
        bool has_cqe = (ujfc->head != ujfc->tail);
        pthread_mutex_unlock(&ujfc->lock);
        if (has_cqe)
            ready++;
    }
    return ready > 0 ? ready : 1;  /* at least 1 if eventfd fired */
}

void uoip_u_ack_jfc(urma_jfc_t *jfc[], uint32_t nevents[], uint32_t jfc_cnt)
{
    /* No kernel state to ack for virtual JFCs */
    (void)jfc;
    (void)nevents;
    (void)jfc_cnt;
}
