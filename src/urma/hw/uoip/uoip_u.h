/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2025. All rights reserved.
 * Description: uoip (URMA over IP) provider — internal data structures
 *
 * Design overview
 * ---------------
 *  uoip is a pure-userspace URMA provider that tunnels SEND operations over
 *  TCP sockets, enabling URMA applications to run on any host with a plain
 *  IP network stack.
 *
 *  Each JFR binds a TCP listen socket; its TCP port is encoded in jfr_id.id.
 *  A background epoll thread per JFR accepts connections and drains incoming
 *  frames into the posted recv WR queue, then posts CQEs to the JFC.
 *
 *  import_jfr() opens a TCP connection to the remote JFR's port.
 *  post_jfs_wr() with SEND writes a framed message over that connection and
 *  immediately posts a send-completion CQE to the local JFC.
 *
 *  Wire frame:  [ magic:4 ][ payload_len:4 ][ payload:N ]
 */

#ifndef UOIP_U_H
#define UOIP_U_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/epoll.h>

#include "urma_types.h"
#include "urma_provider.h"
#include "urma_private.h"

/* ------------------------------------------------------------------ */
/*  Wire-protocol constants                                            */
/* ------------------------------------------------------------------ */

#define UOIP_FRAME_MAGIC    0x554F4950u  /* "UOIP" */
#define UOIP_MAX_PAYLOAD    (1u << 20)   /* 1 MiB per frame */

/* ------------------------------------------------------------------ */
/*  Global virtual device – one per process                           */
/* ------------------------------------------------------------------ */

/* The virtual sysfs device registered at init() */
extern urma_sysfs_dev_t *g_uoip_sysfs_dev;
extern urma_device_t    *g_uoip_device;

/* Local IPv4 address used to fill JFR IDs (network byte order) */
extern uint32_t g_uoip_local_ipv4;

/* Monotonically increasing ID allocator for JFC/JFS/JFR/jetty */
extern atomic_uint g_uoip_id_seq;

static inline uint32_t uoip_alloc_id(void)
{
    return atomic_fetch_add(&g_uoip_id_seq, 1u) + 1u;  /* never zero */
}

/* ------------------------------------------------------------------ */
/*  Helper: build EID from IPv4 address (IPv4-mapped IPv6)            */
/* ------------------------------------------------------------------ */

static inline void uoip_ipv4_to_eid(uint32_t ipv4_nbo, urma_eid_t *eid)
{
    __builtin_memset(eid, 0, sizeof(*eid));
    eid->in4.prefix = __builtin_bswap32(0x0000ffffu);
    eid->in4.addr   = ipv4_nbo;
}

static inline uint32_t uoip_eid_to_ipv4(const urma_eid_t *eid)
{
    return eid->in4.addr;  /* network byte order */
}

/* ------------------------------------------------------------------ */
/*  Completion queue (JFC)                                             */
/* ------------------------------------------------------------------ */

#define UOIP_JFC_MAX_DEPTH  4096

struct uoip_jfc {
    urma_jfc_t      jfc;        /* must be first – cast to/from urma_jfc_t */

    /* CQ ring: a simple FIFO of urma_cr_t */
    urma_cr_t      *ring;
    uint32_t        depth;      /* power of 2 capacity */
    uint32_t        head;       /* consumer index */
    uint32_t        tail;       /* producer index */
    pthread_mutex_t lock;

    /* eventfd polled by wait_jfc / rearm_jfc */
    int             event_fd;
};

#define to_uoip_jfc(j)  ((struct uoip_jfc *)(j))

/* Post one CQE to a JFC and signal the associated JFCE */
int uoip_jfc_post_cqe(struct uoip_jfc *ujfc, const urma_cr_t *cr);

/* ------------------------------------------------------------------ */
/*  JFCE (completion event)                                            */
/* ------------------------------------------------------------------ */

struct uoip_jfce {
    urma_jfce_t     jfce;       /* must be first */
    /* jfce.fd is the eventfd; no extra state needed */
};

#define to_uoip_jfce(j) ((struct uoip_jfce *)(j))

/* ------------------------------------------------------------------ */
/*  JFR (receive queue)                                                */
/* ------------------------------------------------------------------ */

#define UOIP_JFR_MAX_DEPTH  1024
#define UOIP_EPOLL_MAX_EVT  32

/* Receive work-request queue – posted by the application */
struct uoip_rq {
    urma_jfr_wr_t **wrs;       /* ring of WR pointers */
    uint32_t        depth;
    uint32_t        head;
    uint32_t        tail;
    pthread_mutex_t lock;
};

struct uoip_jfr {
    urma_jfr_t      jfr;       /* must be first */

    int             listen_fd; /* bound TCP listen socket */
    int             epoll_fd;  /* epoll for listen_fd + accepted conns */

    struct uoip_rq  rq;

    pthread_t       recv_thread;
    volatile bool   stop;

    struct uoip_jfc *ujfc;     /* recv completion queue */
};

#define to_uoip_jfr(j)  ((struct uoip_jfr *)(j))

/* ------------------------------------------------------------------ */
/*  JFS (send queue)                                                   */
/* ------------------------------------------------------------------ */

struct uoip_jfs {
    urma_jfs_t      jfs;       /* must be first */
    struct uoip_jfc *ujfc;     /* send completion queue */
};

#define to_uoip_jfs(j)  ((struct uoip_jfs *)(j))

/* ------------------------------------------------------------------ */
/*  Target JFR / target jetty (remote peer connection)                */
/* ------------------------------------------------------------------ */

struct uoip_target {
    urma_target_jetty_t tjetty; /* must be first */
    int                 conn_fd;/* TCP-connected socket to remote JFR */
};

#define to_uoip_target(t) ((struct uoip_target *)(t))

/* ------------------------------------------------------------------ */
/*  Registered segment                                                 */
/* ------------------------------------------------------------------ */

struct uoip_seg {
    urma_target_seg_t   tseg;  /* must be first */
    /* Virtual driver: no hardware pinning – just record the mapping */
    uint64_t            va;
    uint64_t            len;
};

#define to_uoip_seg(s)  ((struct uoip_seg *)(s))

/* ------------------------------------------------------------------ */
/*  Jetty (combined send + recv queue)                                 */
/* ------------------------------------------------------------------ */

struct uoip_jetty {
    urma_jetty_t        jetty; /* must be first */
    struct uoip_jfs    *ujfs;
    struct uoip_jfr    *ujfr;  /* may be NULL if shared_jfr used */
    urma_jfr_t         *shared_jfr;
};

#define to_uoip_jetty(j) ((struct uoip_jetty *)(j))

/* ------------------------------------------------------------------ */
/*  Context                                                            */
/* ------------------------------------------------------------------ */

struct uoip_ctx {
    urma_context_t  urma_ctx;  /* must be first */
    uint32_t        local_ipv4;/* cached local IPv4 (network byte order) */
};

#define to_uoip_ctx(c)  ((struct uoip_ctx *)(c))

#endif /* UOIP_U_H */
