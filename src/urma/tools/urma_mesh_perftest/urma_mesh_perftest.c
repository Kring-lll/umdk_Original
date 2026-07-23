/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: urma_mesh_perftest - n-node mesh traffic generator using URMA
 *              Each node sends 4MB write WRs to all other nodes periodically,
 *              measuring latency from post to poll.
 * Author: Trae
 * Create: 2026-07-21
 * Note:
 * History: 2026-07-21 Create file
 */

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <malloc.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "urma_api.h"
#include "ub_get_clock.h"
#include "uvs_api.h"

#define MESH_MAX_NODES          64
#define MESH_MAX_EIDS           16
#define MESH_MAX_TRAFFIC_THREADS (MESH_MAX_NODES * MESH_MAX_EIDS)
#define MESH_DEFAULT_PORT       21115
#define MESH_DEFAULT_MSG_SIZE   (4 * 1024 * 1024) /* 4 MB */
#define MESH_DEFAULT_INTERVAL   1000              /* ms */
#define MESH_DEFAULT_ITERS      1000
#define MESH_JFC_DEPTH          1024
#define MESH_JFS_DEPTH          128
#define MESH_JFR_DEPTH          128
#define MESH_PAGE_SIZE          4096
#define MESH_CONNECT_RETRY_US   (200 * 1000)  /* 200 ms */
#define MESH_CONNECT_MAX_RETRY  10000         /* ~33 minutes */
#define MESH_SOCK_BUF_SIZE      (1024 * 1024)
#define MESH_MAX_IP_LEN         64
#define LAT_TAIL                2  /* discard top-N max values */

#define MESH_DEF_ACCESS (URMA_ACCESS_READ | URMA_ACCESS_WRITE | URMA_ACCESS_ATOMIC)

static bool g_verbose = false;

#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  fprintf(stdout, "[INFO]  " fmt "\n", ##__VA_ARGS__)
#define LOG_QUIET(fmt, ...) fprintf(stdout, fmt "\n", ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) do { \
    if (g_verbose) fprintf(stdout, "[DEBUG] " fmt "\n", ##__VA_ARGS__); \
} while (0)

/* ------------------------------------------------------------------ */
/* Exchange message structures (packed for TCP transfer)              */
/* ------------------------------------------------------------------ */

/* Per-EID information exchanged between peers */
typedef struct __attribute__((packed)) seg_jetty_info {
    uint8_t  eid[URMA_EID_SIZE]; /* EID of the context */
    uint32_t uasid;              /* uasid of the context */
    uint64_t seg_va;             /* virtual address of registered segment */
    uint64_t seg_len;            /* length of registered segment */
    uint32_t seg_attr;           /* urma_seg_attr_t.value */
    uint32_t seg_token_id;       /* token id of segment */
    uint8_t  jetty_eid[URMA_EID_SIZE]; /* EID of the jetty */
    uint32_t jetty_uasid;        /* uasid of the jetty */
    uint32_t jetty_id;           /* jetty id */
} seg_jetty_info_t;

/* Full exchange message: all EIDs of a node */
typedef struct __attribute__((packed)) exchange_msg {
    uint32_t eid_cnt;
    seg_jetty_info_t eids[MESH_MAX_EIDS];
} exchange_msg_t;

/* ------------------------------------------------------------------ */
/* Latency statistics                                                  */
/* ------------------------------------------------------------------ */

typedef struct lat_stats {
    uint64_t *deltas;  /* cycle deltas, one per send */
    uint64_t  count;   /* number of successful sends */
    uint64_t  cap;     /* capacity of deltas array */
} lat_stats_t;

/* ------------------------------------------------------------------ */
/* Per-local-EID resources                                             */
/* ------------------------------------------------------------------ */

typedef struct local_eid {
    urma_eid_t         eid;
    uint32_t           eid_index;
    urma_context_t    *ctx;
    urma_jfc_t        *jfc;
    urma_jfr_t        *jfr;
    urma_jetty_t      *jetty;
    urma_target_seg_t *tseg;
    urma_token_id_t   *token_id;
    void              *buf;       /* registered memory buffer */
    uint64_t           buf_len;
    pthread_mutex_t    jetty_mutex; /* serialises post+poll on this jetty */
} local_eid_t;

/* ------------------------------------------------------------------ */
/* Per-remote-node resources                                           */
/* ------------------------------------------------------------------ */

typedef struct remote_eid_res {
    urma_target_seg_t   *import_tseg;
    urma_target_jetty_t *import_tjetty;
    seg_jetty_info_t     info;  /* received from peer */
} remote_eid_res_t;

typedef struct remote_node {
    char              ip[MESH_MAX_IP_LEN];
    int               sock_fd;
    bool              connected;
    pthread_mutex_t   sock_mutex;
    /* remote exchange info */
    uint32_t          eid_cnt;
    remote_eid_res_t  eids[MESH_MAX_EIDS];
} remote_node_t;

/* ------------------------------------------------------------------ */
/* Traffic thread argument                                             */
/* ------------------------------------------------------------------ */

struct mesh_ctx; /* forward declaration */

typedef struct traffic_arg {
    struct mesh_ctx   *mesh;
    remote_node_t     *node;
    uint32_t       remote_eid_idx;
    local_eid_t   *local;         /* local EID whose jetty we use */
    uint32_t       thread_idx;
    lat_stats_t    stats;
    uint64_t       send_cnt;
    uint64_t       err_cnt;
} traffic_arg_t;

/* ------------------------------------------------------------------ */
/* Global mesh context                                                 */
/* ------------------------------------------------------------------ */

struct mesh_ctx {
    /* config */
    char     dev_name[URMA_MAX_NAME];
    uint16_t tcp_port;
    uint32_t interval_ms;
    uint32_t msg_size;
    uint32_t iters;
    /* local URMA resources */
    urma_device_t      *dev;
    urma_device_attr_t  dev_attr;
    urma_token_t        token;
    uint32_t            eid_cnt;
    local_eid_t         eids[MESH_MAX_EIDS];
    /* remote nodes */
    uint32_t            node_cnt;
    remote_node_t       nodes[MESH_MAX_NODES];
    /* TCP listener */
    int                 listen_fd;
    pthread_t           listen_thread;
    /* node identity for duplicate-connection resolution */
    uint64_t            node_id;
    /* runtime */
    volatile bool       running;
    double              cpu_mhz;
    /* traffic */
    traffic_arg_t       traffic_args[MESH_MAX_TRAFFIC_THREADS];
    uint32_t            traffic_thread_cnt;
    pthread_t           traffic_threads[MESH_MAX_TRAFFIC_THREADS];
};

typedef struct mesh_ctx mesh_ctx_t;

/* ------------------------------------------------------------------ */
/* Utility helpers                                                     */
/* ------------------------------------------------------------------ */

static uint64_t generate_node_id(void)
{
    struct timeval tv;
    (void)gettimeofday(&tv, NULL);
    uint64_t id = ((uint64_t)tv.tv_sec << 20) | (uint64_t)(tv.tv_usec);
    id ^= (uint64_t)pthread_self();
    return id;
}

static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(fd, p + total, len - total, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len)
{
    char *p = (char *)buf;
    size_t total = 0;
    while (total < len) {
        ssize_t n = recv(fd, p + total, len - total, 0);
        if (n == 0) {
            return -1; /* peer closed */
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

static int sock_sync_data(int fd, const void *local, void *remote, size_t len)
{
    if (send_all(fd, local, len) != 0) {
        return -1;
    }
    if (recv_all(fd, remote, len) != 0) {
        return -1;
    }
    return 0;
}

static int set_sock_opts(int fd)
{
    int enable = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0) {
        return -1;
    }
    if (setsockopt(fd, SOL_TCP, TCP_NODELAY, &enable, sizeof(enable)) < 0) {
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Argument parsing                                                    */
/* ------------------------------------------------------------------ */

static void print_usage(const char *prog)
{
    LOG_QUIET("Usage: %s --ips <ip1,ip2,...> [options]", prog);
    LOG_QUIET("  --dev <name>         device name (default: bonding_dev_0)");
    LOG_QUIET("  --ips <ip1,ip2,...>  comma-separated list of remote node IPs (required)");
    LOG_QUIET("  --port <port>        TCP port for info exchange (default: %u)", MESH_DEFAULT_PORT);
    LOG_QUIET("  --interval <ms>      interval between sends in ms (default: %u)", MESH_DEFAULT_INTERVAL);
    LOG_QUIET("  --size <bytes>       write size in bytes (default: %u)", MESH_DEFAULT_MSG_SIZE);
    LOG_QUIET("  --iters <count>      number of iterations per tjetty (default: %u)", MESH_DEFAULT_ITERS);
    LOG_QUIET("  --verbose            enable debug logging");
    LOG_QUIET("  --help               show this help");
}

static int parse_ips(const char *str, remote_node_t *nodes, uint32_t *cnt)
{
    char buf[MESH_MAX_IP_LEN * MESH_MAX_NODES];
    (void)snprintf(buf, sizeof(buf), "%s", str);

    uint32_t n = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(buf, ",", &saveptr);
    while (tok != NULL && n < MESH_MAX_NODES) {
        (void)snprintf(nodes[n].ip, MESH_MAX_IP_LEN, "%s", tok);
        nodes[n].sock_fd = -1;
        nodes[n].connected = false;
        (void)pthread_mutex_init(&nodes[n].sock_mutex, NULL);
        n++;
        tok = strtok_r(NULL, ",", &saveptr);
    }
    if (n == 0) {
        LOG_ERROR("No IPs parsed from --ips argument");
        return -1;
    }
    *cnt = n;
    return 0;
}

static int parse_args(int argc, char *argv[], mesh_ctx_t *mctx)
{
    static struct option opts[] = {
        {"dev",      required_argument, 0, 'd'},
        {"ips",      required_argument, 0, 'i'},
        {"port",     required_argument, 0, 'p'},
        {"interval", required_argument, 0, 't'},
        {"size",     required_argument, 0, 's'},
        {"iters",    required_argument, 0, 'n'},
        {"verbose",  no_argument,       0, 'v'},
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0},
    };

    (void)snprintf(mctx->dev_name, URMA_MAX_NAME, "bonding_dev_0");
    mctx->tcp_port = MESH_DEFAULT_PORT;
    mctx->interval_ms = MESH_DEFAULT_INTERVAL;
    mctx->msg_size = MESH_DEFAULT_MSG_SIZE;
    mctx->iters = MESH_DEFAULT_ITERS;

    const char *ips = NULL;
    int opt;
    while ((opt = getopt_long(argc, argv, "d:i:p:t:s:n:vh", opts, NULL)) != -1) {
        switch (opt) {
            case 'd':
                (void)snprintf(mctx->dev_name, URMA_MAX_NAME, "%s", optarg);
                break;
            case 'i':
                ips = optarg;
                break;
            case 'p':
                mctx->tcp_port = (uint16_t)atoi(optarg);
                break;
            case 't':
                mctx->interval_ms = (uint32_t)atoi(optarg);
                break;
            case 's':
                mctx->msg_size = (uint32_t)atoi(optarg);
                break;
            case 'n':
                mctx->iters = (uint32_t)atoi(optarg);
                break;
            case 'v':
                g_verbose = true;
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return -1;
        }
    }

    if (ips == NULL) {
        LOG_ERROR("--ips is required");
        print_usage(argv[0]);
        return -1;
    }

    if (parse_ips(ips, mctx->nodes, &mctx->node_cnt) != 0) {
        return -1;
    }

    mctx->token.token = 0xABCDEF;
    mctx->node_id = generate_node_id();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Local URMA resource management                                      */
/* ------------------------------------------------------------------ */

static int init_local_resources(mesh_ctx_t *mctx)
{
    urma_status_t status;
    urma_init_attr_t init_attr = { .token = 0, .uasid = 0 };

    status = urma_init(&init_attr);
    if (status != URMA_SUCCESS) {
        LOG_ERROR("urma_init failed: %d", (int)status);
        return -1;
    }

    mctx->dev = urma_get_device_by_name(mctx->dev_name);
    if (mctx->dev == NULL) {
        LOG_ERROR("urma_get_device_by_name(%s) returned NULL", mctx->dev_name);
        goto uninit;
    }

    if (urma_query_device(mctx->dev, &mctx->dev_attr) != URMA_SUCCESS) {
        LOG_ERROR("urma_query_device(%s) failed", mctx->dev_name);
        goto uninit;
    }

    LOG_INFO("Device: %s, type: %d", mctx->dev->name, (int)mctx->dev->type);

    /* Get EID list */
    uint32_t eid_cnt = 0;
    urma_eid_info_t *eid_list = urma_get_eid_list(mctx->dev, &eid_cnt);
    if (eid_list == NULL || eid_cnt == 0) {
        LOG_ERROR("urma_get_eid_list returned no EIDs");
        goto uninit;
    }

    if (eid_cnt > MESH_MAX_EIDS) {
        eid_cnt = MESH_MAX_EIDS;
    }
    mctx->eid_cnt = eid_cnt;

    for (uint32_t i = 0; i < eid_cnt; i++) {
        LOG_INFO("  EID[%u]: index=%u, " EID_FMT, i, eid_list[i].eid_index,
                 EID_ARGS(eid_list[i].eid));
    }

    /* Query path set via UVS for each EID (best-effort) */
    for (uint32_t i = 0; i < eid_cnt; i++) {
        uvs_eid_t uvs_eid;
        (void)memcpy(uvs_eid.raw, eid_list[i].eid.raw, UVS_EID_SIZE);
        uvs_path_set_t path_set;
        (void)memset(&path_set, 0, sizeof(path_set));
        int uvs_ret = uvs_get_path_set(&uvs_eid, &uvs_eid, UVS_RTP, false, &path_set);
        if (uvs_ret == 0) {
            LOG_INFO("  EID[%u] path_set: path_count=%u, chip_count=%u, die_count=%u",
                     i, path_set.path_count, path_set.chip_count, path_set.die_count);
        } else {
            LOG_INFO("  EID[%u] uvs_get_path_set returned %d (continuing)", i, uvs_ret);
        }
    }

    /* Create per-EID resources */
    bool is_ub = (mctx->dev->type == URMA_TRANSPORT_UB);
    for (uint32_t i = 0; i < eid_cnt; i++) {
        local_eid_t *le = &mctx->eids[i];
        le->eid_index = eid_list[i].eid_index;
        le->eid = eid_list[i].eid;
        (void)pthread_mutex_init(&le->jetty_mutex, NULL);

        /* Create context */
        le->ctx = urma_create_context(mctx->dev, le->eid_index);
        if (le->ctx == NULL) {
            LOG_ERROR("urma_create_context(eid_idx=%u) failed", le->eid_index);
            goto destroy_eids;
        }

        /* Allocate token_id (UB devices) */
        if (is_ub) {
            le->token_id = urma_alloc_token_id(le->ctx);
            if (le->token_id == NULL) {
                LOG_ERROR("urma_alloc_token_id(eid_idx=%u) failed", le->eid_index);
                goto destroy_eids;
            }
        }

        /* Create JFC */
        urma_jfc_cfg_t jfc_cfg = {
            .depth = MESH_JFC_DEPTH,
            .flag = { .value = 0 },
            .jfce = NULL,
            .user_ctx = (uint64_t)NULL,
        };
        le->jfc = urma_create_jfc(le->ctx, &jfc_cfg);
        if (le->jfc == NULL) {
            LOG_ERROR("urma_create_jfc(eid_idx=%u) failed", le->eid_index);
            goto destroy_eids;
        }

        /* Create JFR (shared) */
        urma_jfr_cfg_t jfr_cfg = {
            .id = 0,
            .depth = MESH_JFR_DEPTH,
            .flag = { .value = 0 },
            .trans_mode = URMA_TM_RM,
            .min_rnr_timer = URMA_TYPICAL_MIN_RNR_TIMER,
            .jfc = le->jfc,
            .token_value = mctx->token,
            .max_sge = 1,
            .user_ctx = (uint64_t)NULL,
        };
        le->jfr = urma_create_jfr(le->ctx, &jfr_cfg);
        if (le->jfr == NULL) {
            LOG_ERROR("urma_create_jfr(eid_idx=%u) failed", le->eid_index);
            goto destroy_eids;
        }

        /* Create jetty (sharing the JFR) */
        urma_jfs_cfg_t jfs_cfg = {
            .depth = MESH_JFS_DEPTH,
            .flag = { .value = 0 },
            .trans_mode = URMA_TM_RM,
            .priority = URMA_MAX_PRIORITY,
            .max_sge = 1,
            .max_rsge = 1,
            .max_inline_data = 0,
            .rnr_retry = URMA_TYPICAL_RNR_RETRY,
            .err_timeout = URMA_TYPICAL_ERR_TIMEOUT,
            .jfc = le->jfc,
            .user_ctx = (uint64_t)NULL,
        };
        urma_jetty_cfg_t jetty_cfg = {
            .id = 0,
            .flag = { .value = 0 },
            .jfs_cfg = jfs_cfg,
        };
        jetty_cfg.flag.bs.share_jfr = 1;
        jetty_cfg.shared.jfr = le->jfr;
        jetty_cfg.shared.jfc = le->jfc;

        le->jetty = urma_create_jetty(le->ctx, &jetty_cfg);
        if (le->jetty == NULL) {
            LOG_ERROR("urma_create_jetty(eid_idx=%u) failed", le->eid_index);
            goto destroy_eids;
        }

        LOG_INFO("  EID[%u] jetty_id: " EID_FMT ", id=%u, uasid=%u",
                 i, EID_ARGS(le->jetty->jetty_id.eid),
                 le->jetty->jetty_id.id, le->jetty->jetty_id.uasid);

        /* Allocate and register 4 MB segment */
        le->buf_len = mctx->msg_size;
        if (le->buf_len < MESH_PAGE_SIZE) {
            le->buf_len = MESH_PAGE_SIZE;
        }
        le->buf = memalign(MESH_PAGE_SIZE, le->buf_len);
        if (le->buf == NULL) {
            LOG_ERROR("memalign(eid_idx=%u, len=%lu) failed", le->eid_index,
                      (unsigned long)le->buf_len);
            goto destroy_eids;
        }
        (void)memset(le->buf, 0, le->buf_len);

        urma_reg_seg_flag_t reg_flag = {
            .bs.token_policy = URMA_TOKEN_NONE,
            .bs.cacheable = URMA_NON_CACHEABLE,
            .bs.access = MESH_DEF_ACCESS,
            .bs.token_id_valid = is_ub ? 1 : 0,
            .bs.reserved = 0,
        };
        urma_seg_cfg_t seg_cfg = {
            .va = (uint64_t)le->buf,
            .len = le->buf_len,
            .token_id = is_ub ? le->token_id : NULL,
            .token_value = mctx->token,
            .flag = reg_flag,
            .user_ctx = (uintptr_t)NULL,
            .iova = 0,
        };
        le->tseg = urma_register_seg(le->ctx, &seg_cfg);
        if (le->tseg == NULL) {
            LOG_ERROR("urma_register_seg(eid_idx=%u) failed", le->eid_index);
            goto destroy_eids;
        }

        LOG_DEBUG("  EID[%u] resources created: ctx=%p jfc=%p jfr=%p jetty=%p tseg=%p buf=%p len=%lu",
                  i, (void *)le->ctx, (void *)le->jfc, (void *)le->jfr,
                  (void *)le->jetty, (void *)le->tseg, (void *)le->buf,
                  (unsigned long)le->buf_len);
    }

    urma_free_eid_list(eid_list);
    mctx->cpu_mhz = get_cpu_mhz(false);
    LOG_INFO("CPU MHz = %.2f", mctx->cpu_mhz);
    return 0;

destroy_eids:
    urma_free_eid_list(eid_list);
    for (uint32_t i = 0; i < mctx->eid_cnt; i++) {
        local_eid_t *le = &mctx->eids[i];
        if (le->tseg != NULL) {
            (void)urma_unregister_seg(le->tseg);
            le->tseg = NULL;
        }
        if (le->buf != NULL) {
            free(le->buf);
            le->buf = NULL;
        }
        if (le->jetty != NULL) {
            (void)urma_delete_jetty(le->jetty);
            le->jetty = NULL;
        }
        if (le->jfr != NULL) {
            (void)urma_delete_jfr(le->jfr);
            le->jfr = NULL;
        }
        if (le->jfc != NULL) {
            (void)urma_delete_jfc(le->jfc);
            le->jfc = NULL;
        }
        if (le->token_id != NULL) {
            (void)urma_free_token_id(le->token_id);
            le->token_id = NULL;
        }
        if (le->ctx != NULL) {
            (void)urma_delete_context(le->ctx);
            le->ctx = NULL;
        }
    }
uninit:
    (void)urma_uninit();
    return -1;
}

static void destroy_local_resources(mesh_ctx_t *mctx)
{
    for (uint32_t i = 0; i < mctx->eid_cnt; i++) {
        local_eid_t *le = &mctx->eids[i];
        if (le->tseg != NULL) {
            (void)urma_unregister_seg(le->tseg);
        }
        if (le->buf != NULL) {
            free(le->buf);
        }
        if (le->jetty != NULL) {
            (void)urma_delete_jetty(le->jetty);
        }
        if (le->jfr != NULL) {
            (void)urma_delete_jfr(le->jfr);
        }
        if (le->jfc != NULL) {
            (void)urma_delete_jfc(le->jfc);
        }
        if (le->token_id != NULL) {
            (void)urma_free_token_id(le->token_id);
        }
        if (le->ctx != NULL) {
            (void)urma_delete_context(le->ctx);
        }
    }
    (void)urma_uninit();
}

/* ------------------------------------------------------------------ */
/* TCP mesh connection management                                      */
/* ------------------------------------------------------------------ */

static int start_listener(mesh_ctx_t *mctx)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("socket(listen) failed: %s", strerror(errno));
        return -1;
    }

    if (set_sock_opts(fd) != 0) {
        LOG_ERROR("set_sock_opts(listen) failed");
        (void)close(fd);
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(mctx->tcp_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG_ERROR("bind(port=%u) failed: %s", mctx->tcp_port, strerror(errno));
        (void)close(fd);
        return -1;
    }

    if (listen(fd, MESH_MAX_NODES) != 0) {
        LOG_ERROR("listen failed: %s", strerror(errno));
        (void)close(fd);
        return -1;
    }

    mctx->listen_fd = fd;
    LOG_INFO("Listening on port %u", mctx->tcp_port);
    return 0;
}

/* Find a remote_node by IP string; returns index or -1 */
static int find_remote_node(mesh_ctx_t *mctx, const char *ip)
{
    for (uint32_t i = 0; i < mctx->node_cnt; i++) {
        if (strncmp(mctx->nodes[i].ip, ip, MESH_MAX_IP_LEN) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Exchange node_ids on a freshly established socket */
static int exchange_node_ids(int sock, uint64_t my_id, uint64_t *peer_id)
{
    return sock_sync_data(sock, &my_id, peer_id, sizeof(uint64_t));
}

/* Determine whether to keep an outgoing connection */
static bool keep_outgoing(uint64_t my_id, uint64_t peer_id)
{
    return my_id < peer_id;
}

/* Determine whether to keep an incoming connection */
static bool keep_incoming(uint64_t my_id, uint64_t peer_id)
{
    return my_id > peer_id;
}

static void *listen_thread_main(void *arg)
{
    mesh_ctx_t *mctx = (mesh_ctx_t *)arg;

    while (mctx->running) {
        struct sockaddr_in peer_addr;
        socklen_t addr_len = sizeof(peer_addr);
        int sock = accept(mctx->listen_fd, (struct sockaddr *)&peer_addr, &addr_len);
        if (sock < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        (void)set_sock_opts(sock);

        char ip_buf[INET_ADDRSTRLEN];
        (void)inet_ntop(AF_INET, &peer_addr.sin_addr, ip_buf, sizeof(ip_buf));

        int idx = find_remote_node(mctx, ip_buf);
        if (idx < 0) {
            LOG_ERROR("Incoming connection from unknown peer %s, closing", ip_buf);
            (void)close(sock);
            continue;
        }

        /* Exchange node_ids to decide who keeps the connection */
        uint64_t peer_id = 0;
        if (exchange_node_ids(sock, mctx->node_id, &peer_id) != 0) {
            LOG_ERROR("exchange_node_ids failed for peer %s", ip_buf);
            (void)close(sock);
            continue;
        }

        if (!keep_incoming(mctx->node_id, peer_id)) {
            /* Our outgoing connection will be kept instead */
            LOG_DEBUG("accept from %s: my_id=%lu peer_id=%lu -> incoming discarded",
                      ip_buf, (unsigned long)mctx->node_id, (unsigned long)peer_id);
            (void)close(sock);
            continue;
        }

        /* Keep this incoming connection */
        remote_node_t *node = &mctx->nodes[idx];
        pthread_mutex_lock(&node->sock_mutex);
        if (node->sock_fd >= 0) {
            /* Already have a connection — duplicate, close */
            pthread_mutex_unlock(&node->sock_mutex);
            (void)close(sock);
            continue;
        }
        node->sock_fd = sock;
        node->connected = true;
        pthread_mutex_unlock(&node->sock_mutex);
        LOG_INFO("Accepted connection from %s (incoming, kept)", ip_buf);
    }

    return NULL;
}

static int connect_to_peer(mesh_ctx_t *mctx, uint32_t idx)
{
    remote_node_t *node = &mctx->nodes[idx];
    int retry = 0;

    while (mctx->running && !node->connected && retry < MESH_CONNECT_MAX_RETRY) {
        /* Check if listener already established connection */
        pthread_mutex_lock(&node->sock_mutex);
        if (node->sock_fd >= 0) {
            node->connected = true;
            pthread_mutex_unlock(&node->sock_mutex);
            return 0;
        }
        pthread_mutex_unlock(&node->sock_mutex);

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            usleep(MESH_CONNECT_RETRY_US);
            retry++;
            continue;
        }

        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(mctx->tcp_port);
        if (inet_pton(AF_INET, node->ip, &addr.sin_addr) != 1) {
            LOG_ERROR("Invalid IP: %s", node->ip);
            (void)close(sock);
            return -1;
        }

        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            LOG_DEBUG("connect to %s failed (retry %u/%u): %s",
                      node->ip, retry, MESH_CONNECT_MAX_RETRY, strerror(errno));
            (void)close(sock);
            usleep(MESH_CONNECT_RETRY_US);
            retry++;
            continue;
        }

        (void)set_sock_opts(sock);
        LOG_DEBUG("TCP connected to %s, exchanging node_id", node->ip);

        /* Exchange node_ids */
        uint64_t peer_id = 0;
        if (exchange_node_ids(sock, mctx->node_id, &peer_id) != 0) {
            (void)close(sock);
            usleep(MESH_CONNECT_RETRY_US);
            retry++;
            continue;
        }

        if (!keep_outgoing(mctx->node_id, peer_id)) {
            /* Peer's outgoing (our incoming) will be kept - stop connecting, wait for listener */
            LOG_DEBUG("connect to %s: my_id=%lu peer_id=%lu -> outgoing discarded, waiting for incoming",
                      node->ip, (unsigned long)mctx->node_id, (unsigned long)peer_id);
            (void)close(sock);
            /* Wait for listener thread to accept the incoming connection */
            while (mctx->running && !node->connected) {
                pthread_mutex_lock(&node->sock_mutex);
                if (node->sock_fd >= 0) {
                    node->connected = true;
                    pthread_mutex_unlock(&node->sock_mutex);
                    return 0;
                }
                pthread_mutex_unlock(&node->sock_mutex);
                usleep(MESH_CONNECT_RETRY_US);
                retry++;
                if (retry >= MESH_CONNECT_MAX_RETRY) {
                    LOG_ERROR("Timed out waiting for incoming from %s", node->ip);
                    return -1;
                }
            }
            return 0;
        }

        /* Keep this outgoing connection */
        pthread_mutex_lock(&node->sock_mutex);
        if (node->sock_fd >= 0) {
            /* Listener already established — duplicate */
            pthread_mutex_unlock(&node->sock_mutex);
            (void)close(sock);
            return 0;
        }
        node->sock_fd = sock;
        node->connected = true;
        pthread_mutex_unlock(&node->sock_mutex);
        LOG_DEBUG("connect to %s: my_id=%lu peer_id=%lu -> outgoing kept",
                  node->ip, (unsigned long)mctx->node_id, (unsigned long)peer_id);
        LOG_INFO("Connected to %s (outgoing, kept)", node->ip);
        return 0;
    }

    if (!node->connected) {
        LOG_ERROR("Failed to connect to %s after %d retries", node->ip, retry);
        return -1;
    }
    return 0;
}

static int establish_mesh_connections(mesh_ctx_t *mctx)
{
    mctx->running = true;

    if (start_listener(mctx) != 0) {
        return -1;
    }

    if (pthread_create(&mctx->listen_thread, NULL, listen_thread_main, mctx) != 0) {
        LOG_ERROR("pthread_create(listen) failed");
        return -1;
    }

    /* Connect to all peers (retry loop per peer) */
    for (uint32_t i = 0; i < mctx->node_cnt; i++) {
        if (connect_to_peer(mctx, i) != 0) {
            return -1;
        }
    }

    LOG_INFO("All %u peer connections established", mctx->node_cnt);

    /* Sync: confirm all links established (like perftest sync_time) */
    const char *sync_msg = "MESH_SYNC_READY";
    for (uint32_t i = 0; i < mctx->node_cnt; i++) {
        char echo[64] = {0};
        if (sock_sync_data(mctx->nodes[i].sock_fd, sync_msg, echo, strlen(sync_msg)) != 0) {
            LOG_ERROR("sync with %s failed", mctx->nodes[i].ip);
            return -1;
        }
        if (memcmp(sync_msg, echo, strlen(sync_msg)) != 0) {
            LOG_ERROR("sync mismatch with %s", mctx->nodes[i].ip);
            return -1;
        }
        LOG_DEBUG("sync OK with %s", mctx->nodes[i].ip);
    }
    LOG_INFO("All peers synced");
    return 0;
}

static void close_connections(mesh_ctx_t *mctx)
{
    mctx->running = false;
    if (mctx->listen_fd >= 0) {
        (void)shutdown(mctx->listen_fd, SHUT_RDWR);
        (void)close(mctx->listen_fd);
        mctx->listen_fd = -1;
    }
    if (mctx->listen_thread != 0) {
        (void)pthread_join(mctx->listen_thread, NULL);
        mctx->listen_thread = 0;
    }
    for (uint32_t i = 0; i < mctx->node_cnt; i++) {
        if (mctx->nodes[i].sock_fd >= 0) {
            (void)close(mctx->nodes[i].sock_fd);
            mctx->nodes[i].sock_fd = -1;
        }
        mctx->nodes[i].connected = false;
    }
}

/* ------------------------------------------------------------------ */
/* Exchange seg/jetty info and import remote resources                 */
/* ------------------------------------------------------------------ */

static void pack_local_info(mesh_ctx_t *mctx, exchange_msg_t *msg)
{
    (void)memset(msg, 0, sizeof(*msg));
    msg->eid_cnt = mctx->eid_cnt;
    for (uint32_t i = 0; i < mctx->eid_cnt; i++) {
        local_eid_t *le = &mctx->eids[i];
        seg_jetty_info_t *info = &msg->eids[i];

        (void)memcpy(info->eid, le->ctx->eid.raw, URMA_EID_SIZE);
        info->uasid = le->ctx->uasid;
        info->seg_va = le->tseg->seg.ubva.va;
        info->seg_len = le->tseg->seg.len;
        info->seg_attr = le->tseg->seg.attr.value;
        info->seg_token_id = le->tseg->seg.token_id;
        (void)memcpy(info->jetty_eid, le->jetty->jetty_id.eid.raw, URMA_EID_SIZE);
        info->jetty_uasid = le->jetty->jetty_id.uasid;
        info->jetty_id = le->jetty->jetty_id.id;
    }
}

static int exchange_info_with_peer(mesh_ctx_t *mctx, uint32_t idx)
{
    remote_node_t *node = &mctx->nodes[idx];

    exchange_msg_t local_msg;
    pack_local_info(mctx, &local_msg);

    exchange_msg_t remote_msg;
    (void)memset(&remote_msg, 0, sizeof(remote_msg));

    /* Send local info, receive remote info */
    if (sock_sync_data(node->sock_fd, &local_msg, &remote_msg, sizeof(exchange_msg_t)) != 0) {
        LOG_ERROR("exchange_info with %s failed", node->ip);
        return -1;
    }

    if (remote_msg.eid_cnt == 0 || remote_msg.eid_cnt > MESH_MAX_EIDS) {
        LOG_ERROR("Invalid remote eid_cnt=%u from %s", remote_msg.eid_cnt, node->ip);
        return -1;
    }

    node->eid_cnt = remote_msg.eid_cnt;
    for (uint32_t i = 0; i < remote_msg.eid_cnt; i++) {
        node->eids[i].info = remote_msg.eids[i];
        node->eids[i].import_tseg = NULL;
        node->eids[i].import_tjetty = NULL;
        LOG_DEBUG("  remote %s EID[%u]: seg_va=0x%lx seg_len=%lu jetty_id=%u uasid=%u",
                  node->ip, i, (unsigned long)remote_msg.eids[i].seg_va,
                  (unsigned long)remote_msg.eids[i].seg_len,
                  remote_msg.eids[i].jetty_id, remote_msg.eids[i].uasid);
    }

    LOG_INFO("Exchanged info with %s: %u remote EIDs", node->ip, remote_msg.eid_cnt);
    return 0;
}

static int import_remote_resources(mesh_ctx_t *mctx)
{
    /* Use the first local context for all imports */
    urma_context_t *ctx = mctx->eids[0].ctx;

    urma_import_seg_flag_t imp_flag = {
        .bs.cacheable = URMA_NON_CACHEABLE,
        .bs.access = MESH_DEF_ACCESS,
        .bs.mapping = URMA_SEG_NOMAP,
        .bs.reserved = 0,
    };

    for (uint32_t n = 0; n < mctx->node_cnt; n++) {
        remote_node_t *node = &mctx->nodes[n];
        for (uint32_t e = 0; e < node->eid_cnt; e++) {
            seg_jetty_info_t *ri = &node->eids[e].info;

            /* Reconstruct urma_seg_t */
            urma_seg_t remote_seg = {0};
            (void)memcpy(remote_seg.ubva.eid.raw, ri->eid, URMA_EID_SIZE);
            remote_seg.ubva.uasid = ri->uasid;
            remote_seg.ubva.va = ri->seg_va;
            remote_seg.len = ri->seg_len;
            remote_seg.attr.value = ri->seg_attr;
            remote_seg.token_id = ri->seg_token_id;

            /* Import remote seg */
            node->eids[e].import_tseg = urma_import_seg(ctx, &remote_seg, &mctx->token, 0, imp_flag);
            if (node->eids[e].import_tseg == NULL) {
                LOG_ERROR("urma_import_seg failed for %s EID[%u]", node->ip, e);
                return -1;
            }
            LOG_DEBUG("  urma_import_seg OK: %s EID[%u] va=0x%lx len=%lu",
                      node->ip, e, (unsigned long)ri->seg_va, (unsigned long)ri->seg_len);

            /* Reconstruct urma_rjetty_t and import */
            urma_rjetty_t rjetty = {0};
            (void)memcpy(rjetty.jetty_id.eid.raw, ri->jetty_eid, URMA_EID_SIZE);
            rjetty.jetty_id.uasid = ri->jetty_uasid;
            rjetty.jetty_id.id = ri->jetty_id;
            rjetty.trans_mode = URMA_TM_RM;
            rjetty.type = URMA_JETTY;
            rjetty.tp_type = URMA_RTP;

            node->eids[e].import_tjetty = urma_import_jetty(ctx, &rjetty, &mctx->token);
            if (node->eids[e].import_tjetty == NULL) {
                LOG_ERROR("urma_import_jetty failed for %s EID[%u]", node->ip, e);
                return -1;
            }
            LOG_DEBUG("  urma_import_jetty OK: %s EID[%u] jetty_id=%u",
                      node->ip, e, ri->jetty_id);

            LOG_INFO("  Imported seg+jetty from %s EID[%u] (jetty_id=%u)",
                     node->ip, e, ri->jetty_id);
        }
    }

    /* Sync: confirm all imports are complete on both sides */
    const char *sync_msg = "MESH_IMPORT_DONE";
    for (uint32_t i = 0; i < mctx->node_cnt; i++) {
        char echo[64] = {0};
        if (sock_sync_data(mctx->nodes[i].sock_fd, sync_msg, echo, strlen(sync_msg)) != 0) {
            LOG_ERROR("import sync with %s failed", mctx->nodes[i].ip);
            return -1;
        }
    }
    LOG_INFO("All remote resources imported and synced");
    return 0;
}

static void unimport_remote_resources(mesh_ctx_t *mctx)
{
    for (uint32_t n = 0; n < mctx->node_cnt; n++) {
        remote_node_t *node = &mctx->nodes[n];
        for (uint32_t e = 0; e < node->eid_cnt; e++) {
            if (node->eids[e].import_tjetty != NULL) {
                (void)urma_unimport_jetty(node->eids[e].import_tjetty);
                node->eids[e].import_tjetty = NULL;
            }
            if (node->eids[e].import_tseg != NULL) {
                (void)urma_unimport_seg(node->eids[e].import_tseg);
                node->eids[e].import_tseg = NULL;
            }
        }
        node->eid_cnt = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Traffic generation                                                  */
/* ------------------------------------------------------------------ */

static int lat_stats_init(lat_stats_t *s, uint32_t cap)
{
    s->deltas = (uint64_t *)calloc(cap, sizeof(uint64_t));
    if (s->deltas == NULL) {
        return -1;
    }
    s->count = 0;
    s->cap = cap;
    return 0;
}

static void lat_stats_record(lat_stats_t *s, uint64_t delta)
{
    if (s->count < s->cap) {
        s->deltas[s->count] = delta;
        s->count++;
    }
}

static void lat_stats_free(lat_stats_t *s)
{
    if (s->deltas != NULL) {
        free(s->deltas);
        s->deltas = NULL;
    }
    s->count = 0;
    s->cap = 0;
}

static int cycles_compare(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

static void print_lat_stats(const traffic_arg_t *targ, double cpu_mhz)
{
    const lat_stats_t *s = &targ->stats;
    if (s->count == 0) {
        LOG_QUIET("  [%s EID%u] no data", targ->node->ip, targ->remote_eid_idx);
        return;
    }

    /* Sort deltas */
    uint64_t *sorted = (uint64_t *)calloc(s->count, sizeof(uint64_t));
    if (sorted == NULL) {
        return;
    }
    (void)memcpy(sorted, s->deltas, s->count * sizeof(uint64_t));
    qsort(sorted, s->count, sizeof(uint64_t), cycles_compare);

    uint64_t cnt = s->count;
    /* Discard top LAT_TAIL values (outliers) */
    uint64_t measure_cnt = (cnt > LAT_TAIL) ? (cnt - LAT_TAIL) : cnt;

    /* Average */
    double sum = 0.0;
    for (uint64_t i = 0; i < measure_cnt; i++) {
        sum += (double)sorted[i] / cpu_mhz;
    }
    double avg = sum / (double)measure_cnt;

    /* Percentiles */
    uint64_t p99_idx   = (uint64_t)ceil((double)measure_cnt * 0.99);
    uint64_t p9999_idx = (uint64_t)ceil((double)measure_cnt * 0.9999);
    if (p99_idx >= measure_cnt) p99_idx = measure_cnt - 1;
    if (p9999_idx >= measure_cnt) p9999_idx = measure_cnt - 1;

    double min_us = (double)sorted[0] / cpu_mhz;
    double max_us = (double)sorted[measure_cnt - 1] / cpu_mhz;
    double p99_us = (double)sorted[p99_idx] / cpu_mhz;
    double p9999_us = (double)sorted[p9999_idx] / cpu_mhz;

    LOG_QUIET("  [%s EID%u] sends=%lu  err=%lu  avg=%.2f us  p99=%.2f us  p99.99=%.2f us  max=%.2f us  min=%.2f us",
              targ->node->ip, targ->remote_eid_idx,
              (unsigned long)targ->send_cnt, (unsigned long)targ->err_cnt,
              avg, p99_us, p9999_us, max_us, min_us);

    free(sorted);
}

static void *traffic_thread_main(void *arg)
{
    traffic_arg_t *targ = (traffic_arg_t *)arg;
    mesh_ctx_t *mctx = targ->mesh;
    local_eid_t *le = targ->local;
    remote_node_t *node = targ->node;
    remote_eid_res_t *re = &node->eids[targ->remote_eid_idx];

    if (lat_stats_init(&targ->stats, mctx->iters) != 0) {
        LOG_ERROR("lat_stats_init failed for %s EID%u", node->ip, targ->remote_eid_idx);
        return NULL;
    }

    LOG_DEBUG("traffic thread started: peer=%s remote_eid=%u local_eid=%u iters=%u interval=%ums size=%u",
              node->ip, targ->remote_eid_idx, le->eid_index, mctx->iters, mctx->interval_ms, mctx->msg_size);

    uint64_t rid = 0;
    for (uint32_t iter = 0; iter < mctx->iters && mctx->running; iter++) {
        /* Build write WR: 4 MB from local seg to remote imported seg */
        urma_sge_t src_sge = {
            .addr = (uint64_t)le->buf,
            .len = mctx->msg_size,
            .tseg = le->tseg,
            .user_tseg = NULL,
        };
        urma_sge_t dst_sge = {
            .addr = re->info.seg_va,
            .len = mctx->msg_size,
            .tseg = re->import_tseg,
            .user_tseg = NULL,
        };
        urma_sg_t src_sg = { .sge = &src_sge, .num_sge = 1 };
        urma_sg_t dst_sg = { .sge = &dst_sge, .num_sge = 1 };
        urma_rw_wr_t rw = { .src = src_sg, .dst = dst_sg, .target_hint = 0, .notify_data = 0 };
        urma_jfs_wr_t wr = {
            .opcode = URMA_OPC_WRITE,
            .flag = { .value = 0 },
            .tjetty = re->import_tjetty,
            .user_ctx = ++rid,
            .rw = rw,
            .next = NULL,
        };
        wr.flag.bs.complete_enable = 1;

        /* Serialise post+poll on this jetty (shared among threads) */
        pthread_mutex_lock(&le->jetty_mutex);

        uint64_t t_post = get_cycles();

        urma_jfs_wr_t *bad_wr = NULL;
        urma_status_t status = urma_post_jetty_send_wr(le->jetty, &wr, &bad_wr);
        if (status != URMA_SUCCESS) {
            pthread_mutex_unlock(&le->jetty_mutex);
            targ->err_cnt++;
            LOG_ERROR("urma_post_jetty_send_wr failed: %d (node=%s, iter=%u)",
                      (int)status, node->ip, iter);
            usleep(mctx->interval_ms * 1000);
            continue;
        }

        /* Poll JFC until completion */
        urma_cr_t cr = {0};
        int poll_cnt = 0;
        do {
            poll_cnt = urma_poll_jfc(le->jfc, 1, &cr);
            if (poll_cnt < 0) {
                pthread_mutex_unlock(&le->jetty_mutex);
                targ->err_cnt++;
                LOG_ERROR("urma_poll_jfc error: %d (node=%s, iter=%u)",
                          poll_cnt, node->ip, iter);
                break;
            }
        } while (poll_cnt == 0 && mctx->running);

        uint64_t t_poll = get_cycles();
        pthread_mutex_unlock(&le->jetty_mutex);

        if (poll_cnt > 0) {
            if (cr.status != URMA_CR_SUCCESS) {
                targ->err_cnt++;
                LOG_ERROR("CR status=%d (node=%s, iter=%u)", (int)cr.status, node->ip, iter);
            } else {
                lat_stats_record(&targ->stats, t_poll - t_post);
                targ->send_cnt++;
                if (iter < 3) {
                    LOG_DEBUG("  iter=%u peer=%s post->poll=%lu cycles (%.2f us)",
                              iter, node->ip, (unsigned long)(t_poll - t_post),
                              (double)(t_poll - t_post) / mctx->cpu_mhz);
                }
            }
        }

        /* Sleep for interval_ms */
        if (mctx->interval_ms > 0) {
            usleep(mctx->interval_ms * 1000);
        }
    }

    LOG_DEBUG("traffic thread done: peer=%s remote_eid=%u sends=%lu errors=%lu",
              node->ip, targ->remote_eid_idx,
              (unsigned long)targ->send_cnt, (unsigned long)targ->err_cnt);
    return NULL;
}

static int start_traffic(mesh_ctx_t *mctx)
{
    mctx->traffic_thread_cnt = 0;

    for (uint32_t n = 0; n < mctx->node_cnt; n++) {
        remote_node_t *node = &mctx->nodes[n];
        for (uint32_t e = 0; e < node->eid_cnt; e++) {
            uint32_t idx = mctx->traffic_thread_cnt;
            if (idx >= MESH_MAX_TRAFFIC_THREADS) {
                LOG_ERROR("Too many traffic threads (max %u)", MESH_MAX_TRAFFIC_THREADS);
                return -1;
            }
            traffic_arg_t *targ = &mctx->traffic_args[idx];
            targ->mesh = mctx;
            targ->node = node;
            targ->remote_eid_idx = e;
            /* Round-robin assign local jetty */
            targ->local = &mctx->eids[idx % mctx->eid_cnt];
            targ->thread_idx = idx;
            targ->send_cnt = 0;
            targ->err_cnt = 0;
            (void)memset(&targ->stats, 0, sizeof(targ->stats));
            LOG_DEBUG("traffic[%u]: peer=%s remote_eid=%u -> local_eid=%u",
                      idx, node->ip, e, targ->local->eid_index);

            if (pthread_create(&mctx->traffic_threads[idx], NULL,
                               traffic_thread_main, targ) != 0) {
                LOG_ERROR("pthread_create(traffic %u) failed", idx);
                return -1;
            }
            mctx->traffic_thread_cnt++;
        }
    }

    LOG_INFO("Started %u traffic threads (interval=%u ms, size=%u bytes, iters=%u)",
             mctx->traffic_thread_cnt, mctx->interval_ms, mctx->msg_size, mctx->iters);
    return 0;
}

static void wait_traffic_done(mesh_ctx_t *mctx)
{
    for (uint32_t i = 0; i < mctx->traffic_thread_cnt; i++) {
        (void)pthread_join(mctx->traffic_threads[i], NULL);
    }
    LOG_INFO("All traffic threads completed");
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    int ret = 0;
    mesh_ctx_t mctx;
    (void)memset(&mctx, 0, sizeof(mctx));

    ret = parse_args(argc, argv, &mctx);
    if (ret != 0) {
        return EXIT_FAILURE;
    }

    LOG_INFO("=== urma_mesh_perftest ===");
    LOG_INFO("Device: %s, Peers: %u, Port: %u, Interval: %u ms, Size: %u bytes, Iters: %u",
             mctx.dev_name, mctx.node_cnt, mctx.tcp_port,
             mctx.interval_ms, mctx.msg_size, mctx.iters);

    /* Step 1: Init local URMA resources */
    LOG_INFO("[Step 1] Initialising local URMA resources...");
    ret = init_local_resources(&mctx);
    if (ret != 0) {
        LOG_ERROR("init_local_resources failed");
        goto clean_args;
    }

    /* Step 2: Establish TCP mesh connections */
    LOG_INFO("[Step 2] Establishing mesh connections...");
    ret = establish_mesh_connections(&mctx);
    if (ret != 0) {
        LOG_ERROR("establish_mesh_connections failed");
        goto destroy_local;
    }

    /* Step 3: Exchange seg/jetty info with each peer */
    LOG_INFO("[Step 3] Exchanging seg/jetty info...");
    for (uint32_t i = 0; i < mctx.node_cnt; i++) {
        ret = exchange_info_with_peer(&mctx, i);
        if (ret != 0) {
            LOG_ERROR("exchange_info_with_peer(%u) failed", i);
            goto close_conn;
        }
    }

    /* Step 4: Import remote segs and jettys */
    LOG_INFO("[Step 4] Importing remote resources...");
    ret = import_remote_resources(&mctx);
    if (ret != 0) {
        LOG_ERROR("import_remote_resources failed");
        goto close_conn;
    }

    /* Step 5: Run traffic */
    LOG_INFO("[Step 5] Starting traffic...");
    ret = start_traffic(&mctx);
    if (ret != 0) {
        LOG_ERROR("start_traffic failed");
        goto unimport;
    }

    wait_traffic_done(&mctx);

    /* Step 6: Print latency statistics */
    LOG_QUIET("");
    LOG_QUIET("=== Latency Statistics (post_jetty_send_wr -> poll_jfc) ===");
    LOG_QUIET("  %-20s %-6s %-8s %-8s %-12s %-12s %-14s %-12s %-12s",
              "Peer", "EID", "Sends", "Errors",
              "avg[us]", "p99[us]", "p99.99[us]", "max[us]", "min[us]");
    LOG_QUIET("  %s",
              "-------------------------------------------------------------------------------------------");
    for (uint32_t i = 0; i < mctx.traffic_thread_cnt; i++) {
        print_lat_stats(&mctx.traffic_args[i], mctx.cpu_mhz);
    }

unimport:
    unimport_remote_resources(&mctx);
    for (uint32_t i = 0; i < mctx.traffic_thread_cnt; i++) {
        lat_stats_free(&mctx.traffic_args[i].stats);
    }
close_conn:
    close_connections(&mctx);
destroy_local:
    destroy_local_resources(&mctx);
clean_args:
    for (uint32_t i = 0; i < mctx.node_cnt; i++) {
        (void)pthread_mutex_destroy(&mctx.nodes[i].sock_mutex);
    }
    for (uint32_t i = 0; i < mctx.eid_cnt; i++) {
        (void)pthread_mutex_destroy(&mctx.eids[i].jetty_mutex);
    }
    return (ret == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
