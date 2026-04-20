/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2025. All rights reserved.
 * Description: uoip — JFR (receive queue) implementation
 *
 * Each JFR owns:
 *  - A TCP listen socket (port stored in jfr_id.id)
 *  - An epoll fd multiplexing the listen socket + accepted connections
 *  - A background recv_thread that accepts new connections and drains
 *    incoming frames into the posted receive work-request queue,
 *    then posts CQEs to the JFC.
 */

#define _GNU_SOURCE

#include "uoip_u_jfr.h"
#include "uoip_u.h"
#include "uoip_u_log.h"
#include "uoip_u_net.h"
#include "uoip_u_jfc.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>

/* ------------------------------------------------------------------ */
/*  Recv-queue helpers                                                 */
/* ------------------------------------------------------------------ */

static int rq_init(struct uoip_rq *rq, uint32_t depth)
{
    uint32_t cap = depth < 2 ? 2 : depth;
    /* make it a power of two */
    cap--;
    cap |= cap >> 1; cap |= cap >> 2; cap |= cap >> 4;
    cap |= cap >> 8; cap |= cap >> 16;
    cap++;

    rq->wrs = calloc(cap, sizeof(urma_jfr_wr_t *));
    if (!rq->wrs)
        return -1;
    rq->depth = cap;
    rq->head  = 0;
    rq->tail  = 0;
    pthread_mutex_init(&rq->lock, NULL);
    return 0;
}

static void rq_destroy(struct uoip_rq *rq)
{
    pthread_mutex_destroy(&rq->lock);
    free(rq->wrs);
}

static int rq_push(struct uoip_rq *rq, urma_jfr_wr_t *wr)
{
    pthread_mutex_lock(&rq->lock);
    uint32_t next = (rq->tail + 1) & (rq->depth - 1);
    if (next == rq->head) {
        pthread_mutex_unlock(&rq->lock);
        return -1;  /* full */
    }
    rq->wrs[rq->tail] = wr;
    rq->tail = next;
    pthread_mutex_unlock(&rq->lock);
    return 0;
}

static urma_jfr_wr_t *rq_pop(struct uoip_rq *rq)
{
    pthread_mutex_lock(&rq->lock);
    if (rq->head == rq->tail) {
        pthread_mutex_unlock(&rq->lock);
        return NULL;
    }
    urma_jfr_wr_t *wr = rq->wrs[rq->head];
    rq->head = (rq->head + 1) & (rq->depth - 1);
    pthread_mutex_unlock(&rq->lock);
    return wr;
}

/* ------------------------------------------------------------------ */
/*  Background recv thread                                             */
/* ------------------------------------------------------------------ */

static void handle_data(struct uoip_jfr *ujfr, int fd)
{
    void    *buf  = NULL;
    uint32_t len  = 0;

    if (uoip_frame_recv(fd, &buf, &len) < 0) {
        /* Connection closed or error: remove from epoll */
        epoll_ctl(ujfr->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
        return;
    }

    urma_jfr_wr_t *wr = rq_pop(&ujfr->rq);
    if (!wr) {
        UOIP_LOG_WARN("JFR recv: no posted WR, dropping %u bytes\n", len);
        free(buf);
        return;
    }

    /* Copy payload into the first SGE of the recv WR */
    uint32_t copy_len = len;
    if (wr->src.num_sge > 0 && wr->src.sge) {
        uint32_t avail = wr->src.sge[0].len;
        if (copy_len > avail)
            copy_len = avail;
        if (copy_len > 0 && buf)
            memcpy((void *)(uintptr_t)wr->src.sge[0].addr, buf, copy_len);
    }
    free(buf);

    /* Post receive CQE */
    urma_cr_t cr = {
        .status         = URMA_CR_SUCCESS,
        .user_ctx       = wr->user_ctx,
        .opcode         = URMA_CR_OPC_SEND,
        .completion_len = copy_len,
    };
    cr.flag.bs.s_r = 1;  /* receive */
    uoip_jfc_post_cqe(ujfr->ujfc, &cr);
}

static void *uoip_jfr_recv_thread(void *arg)
{
    struct uoip_jfr *ujfr = (struct uoip_jfr *)arg;
    struct epoll_event events[UOIP_EPOLL_MAX_EVT];

    while (!ujfr->stop) {
        int nfds = epoll_wait(ujfr->epoll_fd, events, UOIP_EPOLL_MAX_EVT, 200);
        if (nfds < 0) {
            if (errno == EINTR)
                continue;
            UOIP_LOG_ERR("epoll_wait: %s\n", strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == ujfr->listen_fd) {
                /* New incoming connection */
                int cli = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
                if (cli < 0) {
                    UOIP_LOG_WARN("accept: %s\n", strerror(errno));
                    continue;
                }
                struct epoll_event ev = { .events = EPOLLIN, .data.fd = cli };
                epoll_ctl(ujfr->epoll_fd, EPOLL_CTL_ADD, cli, &ev);
                UOIP_LOG_INFO("JFR accepted connection fd=%d\n", cli);
            } else {
                handle_data(ujfr, fd);
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  create / delete                                                    */
/* ------------------------------------------------------------------ */

urma_jfr_t *uoip_u_create_jfr(urma_context_t *ctx, urma_jfr_cfg_t *cfg)
{
    if (!ctx || !cfg || !cfg->jfc) {
        errno = EINVAL;
        return NULL;
    }

    struct uoip_jfr *ujfr = calloc(1, sizeof(*ujfr));
    if (!ujfr)
        return NULL;

    /* Receive work-request queue */
    uint32_t depth = cfg->depth ? cfg->depth : 256;
    if (rq_init(&ujfr->rq, depth) < 0)
        goto err_rq;

    /* TCP listen socket; OS assigns the port */
    uint16_t port = 0;
    ujfr->listen_fd = uoip_tcp_listen(&port);
    if (ujfr->listen_fd < 0)
        goto err_listen;

    /* epoll fd */
    ujfr->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (ujfr->epoll_fd < 0)
        goto err_epoll;

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = ujfr->listen_fd };
    epoll_ctl(ujfr->epoll_fd, EPOLL_CTL_ADD, ujfr->listen_fd, &ev);

    /* Fill urma_jfr_t header */
    ujfr->jfr.urma_ctx = ctx;
    ujfr->jfr.jfr_cfg  = *cfg;
    ujfr->jfr.jfr_id.id  = port;            /* TCP port → JFR ID */
    uoip_ipv4_to_eid(g_uoip_local_ipv4, &ujfr->jfr.jfr_id.eid);
    pthread_mutex_init(&ujfr->jfr.event_mutex, NULL);
    pthread_cond_init(&ujfr->jfr.event_cond, NULL);

    ujfr->ujfc = to_uoip_jfc(cfg->jfc);
    ujfr->stop = false;

    /* Start background thread */
    if (pthread_create(&ujfr->recv_thread, NULL,
                       uoip_jfr_recv_thread, ujfr) != 0)
        goto err_thread;

    UOIP_LOG_INFO("JFR created: port=%u id=%u\n", port, ujfr->jfr.jfr_id.id);
    return &ujfr->jfr;

err_thread:
    close(ujfr->epoll_fd);
err_epoll:
    close(ujfr->listen_fd);
err_listen:
    rq_destroy(&ujfr->rq);
err_rq:
    free(ujfr);
    return NULL;
}

urma_status_t uoip_u_delete_jfr(urma_jfr_t *jfr)
{
    if (!jfr)
        return URMA_EINVAL;

    struct uoip_jfr *ujfr = to_uoip_jfr(jfr);

    /* Stop the background thread */
    ujfr->stop = true;
    pthread_join(ujfr->recv_thread, NULL);

    close(ujfr->epoll_fd);
    close(ujfr->listen_fd);
    rq_destroy(&ujfr->rq);
    pthread_mutex_destroy(&jfr->event_mutex);
    pthread_cond_destroy(&jfr->event_cond);
    free(ujfr);
    return URMA_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  post_jfr_wr: enqueue a receive buffer                             */
/* ------------------------------------------------------------------ */

urma_status_t uoip_u_post_jfr_wr(urma_jfr_t *jfr, urma_jfr_wr_t *wr,
                                   urma_jfr_wr_t **bad_wr)
{
    if (!jfr || !wr)
        return URMA_EINVAL;

    struct uoip_jfr *ujfr = to_uoip_jfr(jfr);
    urma_jfr_wr_t *cur = wr;

    while (cur) {
        if (rq_push(&ujfr->rq, cur) < 0) {
            UOIP_LOG_WARN("RQ full\n");
            if (bad_wr)
                *bad_wr = cur;
            return URMA_FAIL;
        }
        cur = cur->next;
    }
    return URMA_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  import_jfr: establish TCP connection to remote JFR               */
/* ------------------------------------------------------------------ */

urma_target_jetty_t *uoip_u_import_jfr(urma_context_t *ctx,
                                         urma_rjfr_t *rjfr,
                                         urma_token_t *token)
{
    (void)token;  /* UOIP does not use tokens */
    if (!ctx || !rjfr)
        return NULL;

    struct uoip_target *ut = calloc(1, sizeof(*ut));
    if (!ut)
        return NULL;

    uint32_t remote_ip   = uoip_eid_to_ipv4(&rjfr->jfr_id.eid);
    uint16_t remote_port = (uint16_t)rjfr->jfr_id.id;

    ut->conn_fd = uoip_tcp_connect(remote_ip, htons(remote_port));
    if (ut->conn_fd < 0) {
        free(ut);
        return NULL;
    }

    ut->tjetty.urma_ctx    = ctx;
    ut->tjetty.id          = rjfr->jfr_id;
    ut->tjetty.trans_mode  = rjfr->trans_mode;
    ut->tjetty.flag        = rjfr->flag;
    ut->tjetty.type        = URMA_JFR;

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &remote_ip, ip_str, sizeof(ip_str));
    UOIP_LOG_INFO("import_jfr: connected to %s:%u fd=%d\n",
                  ip_str, remote_port, ut->conn_fd);
    return &ut->tjetty;
}

urma_status_t uoip_u_unimport_jfr(urma_target_jetty_t *tjfr)
{
    if (!tjfr)
        return URMA_EINVAL;
    struct uoip_target *ut = to_uoip_target(tjfr);
    if (ut->conn_fd >= 0)
        close(ut->conn_fd);
    free(ut);
    return URMA_SUCCESS;
}

urma_status_t uoip_u_advise_jfr(urma_jfs_t *jfs,
                                  urma_target_jetty_t *tjfr)
{
    (void)jfs;
    (void)tjfr;
    return URMA_SUCCESS;
}

urma_status_t uoip_u_unadvise_jfr(urma_jfs_t *jfs,
                                    urma_target_jetty_t *tjfr)
{
    (void)jfs;
    (void)tjfr;
    return URMA_SUCCESS;
}
