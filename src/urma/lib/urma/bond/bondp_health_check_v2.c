/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bond provider health check v2 implementation (per-context)
 *   - Node-granularity health probing (one-to-many / many-to-one)
 *   - Out-of-band jetty per path, isolated from user datapath
 *   - Driven by bondp_worker (epoll + timewheel)
 */

#include <errno.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bondp_hash_table.h"
#include "bondp_health_check_v2.h"
#include "bondp_types.h"
#include "bondp_worker.h"
#include "topo_info.h"
#include "ub_hash.h"
#include "ub_list.h"
#include "urma_api.h"
#include "urma_log.h"

#define BONDP_HCV2_PROBE_BUF_LEN     (4096)
#define BONDP_HCV2_PROBE_ALIGN       (4096)
#define BONDP_HCV2_PROBE_INTERVAL_MS (20)
#define BONDP_HCV2_PROBE_TIMEOUT_MS  (100)
#define BONDP_HCV2_CQE_BATCH         (8)
#define BONDP_HCV2_NODE_TABLE_SIZE   (64)
#define BONDP_HCV2_NODE_HASH_BASIS   (0x983572U)
#define BONDP_HCV2_TJETTY_MAP_SIZE   (256)
#define BONDP_HCV2_TJETTY_HASH_BASIS (0x983573U)

/* A node may be shared by multiple vjettys (many-to-one). Each import_jetty creates a ref attached to node->vjetty_list */
typedef struct bondp_hcv2_vjetty_ref {
    struct bondp_comp *cfg_jetty;          /* Local virtual jetty, used to clone probe jetty configuration */
    struct bondp_target_jetty *bdp_tjetty; /* Imported target jetty, used to obtain p_tjetty / p_check_tseg */
    struct ub_list entry;                  /* List node attached to node->vjetty_list */
} bondp_hcv2_vjetty_ref_t;

typedef struct bondp_hcv2_node bondp_hcv2_node_t;

/* State of one probe path: corresponds to a (local_idx, target_idx) pair */
typedef struct bondp_hcv2_path {
    int target_idx; /* Peer device index (determined by topo at creation time) */
    bool valid;     /* Whether this path has been initialized */
#ifndef __cplusplus
    atomic_bool link_ok; /* Link state of this path (atomic, for lock-free reading by datapath) */
#else
    std::atomic_bool link_ok;
#endif
    uint64_t last_cqe_ts_us; /* Timestamp of the last received CQE */
} bondp_hcv2_path_t;

/* A remote node: aggregated by node_id, all vjettys share the same set of probe paths */
struct bondp_hcv2_node {
    uint32_t node_id;    /* Node ID in topo (key for node_table) */
    urma_eid_t node_eid; /* Node EID, used only for logging/debugging */
    uint32_t refcnt;     /* Number of associated vjettys, node can be destroyed when 0 */
#ifndef __cplusplus
    atomic_bool node_link_ok; /* Node-level link state: aggregation of all path->link_ok */
#else
    std::atomic_bool node_link_ok;
#endif
    pthread_rwlock_t lock;     /* Protects vjetty_list and refcnt */
    struct ub_list node_entry; /* List node attached to ctx->node_list */

    bondp_hcv2_path_t paths[URMA_UBAGG_DEV_MAX_NUM][URMA_UBAGG_DEV_MAX_NUM]; /* 2D path matrix */
    uint32_t path_count;                                                     /* Number of valid paths */

    struct bondp_target_jetty *primary_tjetty; /* Selected primary tjetty, its p_tjetty/p_check_tseg are used directly for probing */
    struct ub_list vjetty_list;                /* Reference list of all vjettys under this node */

    hmap_node_t hmap_node; /* Hash node attached to ctx->node_table */
};

/* Reverse mapping entry: during unregister_node, lookup the owning node via bdp_tjetty to avoid O(n) traversal */
typedef struct bondp_hcv2_tjetty_map {
    hmap_node_t hmap_node;                 /* Hash node attached to ctx->tjetty_map */
    struct bondp_target_jetty *bdp_tjetty; /* key: target_jetty pointer */
    bondp_hcv2_node_t *node;               /* val: owning node */
} bondp_hcv2_tjetty_map_t;

/* Per-context health-check context */
typedef struct bondp_hcv2_ctx {
    pthread_rwlock_t lock;         /* Protects node_list / node_table / tjetty_map */
    struct ub_list node_list;      /* Doubly-linked list of all nodes, for traversal and destruction */
    bondp_hash_table_t node_table; /* node_id → node hash table, for O(1) dedup/lookup */

    bondp_hash_table_t tjetty_map; /* bdp_tjetty → tjetty_map hash table, for reverse lookup */

    void *probe_buf;                      /* Global shared probe buffer (lazy alloc) */
    uint64_t probe_buf_len;               /* Buffer length */
    bondp_worker_task_id_t probe_task_id; /* Globally unique periodic probe task ID */

    bondp_hcv2_node_t *probe_cur_node; /* Current polling position: each tick handles one node */

    /* Probe resources shared per local device: at most URMA_UBAGG_DEV_MAX_NUM sets per process (usually only 2~4 active) */
    urma_jfce_t *probe_jfce[URMA_UBAGG_DEV_MAX_NUM];
    urma_jfc_t *probe_jfc[URMA_UBAGG_DEV_MAX_NUM];
    urma_jfr_t *probe_jfr[URMA_UBAGG_DEV_MAX_NUM];
    urma_target_seg_t *probe_seg[URMA_UBAGG_DEV_MAX_NUM];
    urma_jetty_t *probe_jetty[URMA_UBAGG_DEV_MAX_NUM]; /* Probe jetty shared per local_idx */
    int probe_jfce_fd[URMA_UBAGG_DEV_MAX_NUM];
    bool probe_dev_inited[URMA_UBAGG_DEV_MAX_NUM];

    /* Argument for jfce_handler, must be freed during cleanup */
    struct bondp_hcv2_jfce_arg *jfce_arg[URMA_UBAGG_DEV_MAX_NUM];
} bondp_hcv2_ctx_t;

/* jfce_handler obtains hcv2 + local_idx through arg */
typedef struct bondp_hcv2_jfce_arg {
    bondp_hcv2_ctx_t *hcv2;
    int local_idx;
} bondp_hcv2_jfce_arg_t;

/* Get monotonic clock timestamp in microseconds, used for probe timeout detection */
static uint64_t bondp_hcv2_get_monotonic_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* Underlying hash for node_table: hash the uint32_t data pointed to by key */
static uint32_t bondp_hcv2_node_hash_f(void *key)
{
    return ub_hash_bytes(key, sizeof(uint32_t), BONDP_HCV2_NODE_HASH_BASIS);
}

/* Upper-level hash wrapper for node_table: pass in node_id, return hash value */
static uint32_t bondp_hcv2_node_hash(uint32_t node_id)
{
    return bondp_hcv2_node_hash_f(&node_id);
}

/* Comparison function for node_table: compare candidate node's node_id with the node_id pointed to by key */
static bool bondp_hcv2_node_comp_f(hmap_node_t *node, void *key)
{
    bondp_hcv2_node_t *n = CONTAINER_OF_FIELD(node, bondp_hcv2_node_t, hmap_node);
    return n->node_id == *(uint32_t *)key;
}

/* Free callback for node_table: node memory is managed externally by destroy_node, do nothing here */
static void bondp_hcv2_node_free_f(hmap_node_t *node)
{
    bondp_hcv2_node_t *n = CONTAINER_OF_FIELD(node, bondp_hcv2_node_t, hmap_node);
    (void)n;
}

/* Underlying hash for tjetty_map: hash the value of the key variable itself (pointer value) */
static uint32_t bondp_hcv2_tjetty_hash_f(void *key)
{
    return ub_hash_bytes(&key, sizeof(key), BONDP_HCV2_TJETTY_HASH_BASIS);
}

/* Upper-level hash wrapper for tjetty_map: pass in bdp_tjetty pointer value, return hash value */
static uint32_t bondp_hcv2_tjetty_hash(struct bondp_target_jetty *bdp_tjetty)
{
    return bondp_hcv2_tjetty_hash_f(bdp_tjetty);
}

/* Comparison function for tjetty_map: compare candidate entry's bdp_tjetty pointer value with key */
static bool bondp_hcv2_tjetty_comp_f(hmap_node_t *node, void *key)
{
    bondp_hcv2_tjetty_map_t *m = CONTAINER_OF_FIELD(node, bondp_hcv2_tjetty_map_t, hmap_node);
    return m->bdp_tjetty == (struct bondp_target_jetty *)key;
}

/* Free callback for tjetty_map: free tjetty_map entry memory */
static void bondp_hcv2_tjetty_free_f(hmap_node_t *node)
{
    bondp_hcv2_tjetty_map_t *m = CONTAINER_OF_FIELD(node, bondp_hcv2_tjetty_map_t, hmap_node);
    free(m);
}

/*
 * Find the corresponding node_id in topo by bonding EID.
 * bdp_tjetty->v_tjetty.id.eid is the bonding aggregated EID, directly match agg_eid.
 * Return 0 and write to *node_id on success, -1 on failure.
 */
static int bondp_hcv2_find_node_id_by_eid(topo_map_t *topo_map, const urma_eid_t *eid, uint32_t *node_id)
{
    if (topo_map == NULL || eid == NULL || node_id == NULL) {
        return -1;
    }

    for (uint32_t i = 0; i < topo_map->node_num; ++i) {
        bondp_topo_node_t *node = &topo_map->topo_infos[i];

        for (int d = 0; d < DEV_NUM; ++d) {
            bondp_topo_agg_dev_t *dev = &node->agg_devs[d];
            bool empty = true;
            for (int k = 0; k < EID_LEN; ++k) {
                if (dev->agg_eid[k] != 0) {
                    empty = false;
                    break;
                }
            }
            if (empty) {
                continue;
            }
            if (!memcmp(dev->agg_eid, eid, EID_LEN)) {
                *node_id = node->node_id;
                return 0;
            }
        }
    }

    return -1;
}

static void bondp_hcv2_update_node_link_state(bondp_hcv2_node_t *node);
static void bondp_hcv2_jfce_handler(int fd, void *arg);

/*
 * bondp_worker epoll callback: triggered by jfce fd of local device.
 * This jfc is shared by all nodes' paths with the same local_idx, locate specific path via CQE user_ctx.
 */
static void bondp_hcv2_jfce_handler(int fd, void *arg)
{
    (void)fd;
    bondp_hcv2_jfce_arg_t *jarg = (bondp_hcv2_jfce_arg_t *)arg;
    if (jarg == NULL) {
        return;
    }
    int local_idx = jarg->local_idx;
    bondp_hcv2_ctx_t *hcv2 = jarg->hcv2;

    if (hcv2 == NULL || local_idx < 0 || local_idx >= URMA_UBAGG_DEV_MAX_NUM ||
        !hcv2->probe_dev_inited[local_idx]) {
        return;
    }

    urma_jfc_t *jfc = hcv2->probe_jfc[local_idx];
    urma_jfce_t *jfce = hcv2->probe_jfce[local_idx];

    /* Wait and poll the shared jfc, batch up to 8 CQEs at a time to avoid frequent epoll wakeups */
    urma_jfc_t *p_jfc = NULL;
    int p_num = urma_wait_jfc(jfce, 1, 0, &p_jfc);
    if (p_num <= 0) {
        return;
    }

    urma_cr_t cr[BONDP_HCV2_CQE_BATCH];
    int n = urma_poll_jfc(jfc, BONDP_HCV2_CQE_BATCH, cr);
    if (n <= 0) {
        return;
    }

    /* Batch process CQEs: acquire read lock once and process all, reducing lock overhead */
    bondp_hcv2_node_t *updated_nodes[BONDP_HCV2_CQE_BATCH];
    int updated_cnt = 0;

    pthread_rwlock_rdlock(&hcv2->lock);
    for (int k = 0; k < n; k++) {
        uint32_t node_id = (uint32_t)(cr[k].user_ctx >> 32);
        uint32_t path_lidx = (uint32_t)((cr[k].user_ctx >> 16) & 0xFFFF);
        uint32_t path_tidx = (uint32_t)(cr[k].user_ctx & 0xFFFF);

        if (path_lidx != (uint32_t)local_idx) {
            continue;
        }

        uint32_t hash = bondp_hcv2_node_hash(node_id);
        hmap_node_t *hn = bondp_hash_table_lookup_without_lock(&hcv2->node_table, &node_id, hash);
        if (hn == NULL) {
            continue;
        }
        bondp_hcv2_node_t *node = CONTAINER_OF_FIELD(hn, bondp_hcv2_node_t, hmap_node);

        if ((int)path_tidx < 0 || (int)path_tidx >= URMA_UBAGG_DEV_MAX_NUM) {
            continue;
        }
        bondp_hcv2_path_t *path = &node->paths[path_lidx][path_tidx];
        if (!path->valid) {
            continue;
        }

        path->last_cqe_ts_us = bondp_hcv2_get_monotonic_us();
        bool ok = (cr[k].status == URMA_CR_SUCCESS);
        atomic_store(&path->link_ok, ok);

        /* Record unique updated nodes to avoid redundant aggregation state calculation inside the loop */
        bool already = false;
        for (int m = 0; m < updated_cnt; m++) {
            if (updated_nodes[m] == node) {
                already = true;
                break;
            }
        }
        if (!already) {
            updated_nodes[updated_cnt++] = node;
        }
    }

    /* Update aggregation state for all involved nodes outside the loop */
    for (int m = 0; m < updated_cnt; m++) {
        bondp_hcv2_update_node_link_state(updated_nodes[m]);
    }
    pthread_rwlock_unlock(&hcv2->lock);
}

/*
 * Send probe WRITE WR.
 * Use p_tjetty/p_check_tseg of the selected primary_tjetty on the node,
 * write 8 bytes to peer via shared probe_jetty[local_idx],
 * user_ctx encodes node_id + local_idx + target_idx for CQE lookup.
 */
static void bondp_hcv2_send_probe(bondp_hcv2_ctx_t *hcv2, bondp_hcv2_node_t *node,
                                  int local_idx, int target_idx, bondp_hcv2_path_t *path)
{
    if (hcv2 == NULL || node == NULL || path == NULL || !path->valid ||
        local_idx < 0 || local_idx >= URMA_UBAGG_DEV_MAX_NUM ||
        target_idx < 0 || target_idx >= URMA_UBAGG_DEV_MAX_NUM ||
        hcv2->probe_jetty[local_idx] == NULL ||
        node->primary_tjetty == NULL) {
        return;
    }

    bondp_target_jetty_t *bdp_tjetty = node->primary_tjetty;

    urma_target_jetty_t *tjetty = bdp_tjetty->p_tjetty[local_idx][target_idx];
    urma_target_seg_t *tseg = bdp_tjetty->p_check_tseg[local_idx][target_idx];

    if (tjetty == NULL || tseg == NULL) {
        URMA_LOG_WARN("hcv2: no target resource for probe, node=%u lidx=%d tidx=%d\n",
                      node->node_id, local_idx, target_idx);
        return;
    }

    urma_sge_t src_sge = {
        .addr = (uint64_t)hcv2->probe_buf,
        .len = sizeof(uint64_t),
        .tseg = hcv2->probe_seg[local_idx],
        .user_tseg = NULL,
    };
    urma_sge_t dst_sge = {
        .addr = tseg->seg.ubva.va,
        .len = sizeof(uint64_t),
        .tseg = tseg,
        .user_tseg = NULL,
    };

    /* user_ctx encodes node_id + local_idx + target_idx, used to locate the path when CQE arrives */
    uint64_t user_ctx = ((uint64_t)node->node_id << 32) |
                        ((uint32_t)local_idx << 16) |
                        (uint32_t)target_idx;

    urma_jfs_wr_t wr = {
        .opcode = URMA_OPC_WRITE,
        .flag.bs.complete_enable = 1,
        .tjetty = tjetty,
        .user_ctx = user_ctx,
        .rw = {
            .src = {.sge = &src_sge, .num_sge = 1},
            .dst = {.sge = &dst_sge, .num_sge = 1},
        },
        .next = NULL,
    };

    urma_jfs_wr_t *bad_wr = NULL;
    urma_status_t ret = urma_post_jetty_send_wr(hcv2->probe_jetty[local_idx], &wr, &bad_wr);
    if (ret != URMA_SUCCESS) {
        URMA_LOG_WARN("hcv2: probe send failed, node=%u lidx=%d tidx=%d ret=%d\n",
                      node->node_id, local_idx, target_idx, ret);
        atomic_store(&path->link_ok, false);
    }
}

/*
 * bondp_worker global periodic task: triggered every 20ms.
 * Round-robin processing: each tick handles all valid paths of one node,
 * record current position probe_cur_node, next tick handles the next node.
 * Start from head after reaching tail. Finally reschedule next tick.
 */
static void bondp_hcv2_probe_task_fn(void *arg)
{
    bondp_hcv2_ctx_t *hcv2 = (bondp_hcv2_ctx_t *)arg;
    if (hcv2 == NULL) {
        return;
    }

    pthread_rwlock_rdlock(&hcv2->lock);

    /* Fast check: skip directly when no probe resources exist, avoid spinning through nodes */
    bool has_probe_res = false;
    for (int i = 0; i < URMA_UBAGG_DEV_MAX_NUM; ++i) {
        if (hcv2->probe_dev_inited[i]) {
            has_probe_res = true;
            break;
        }
    }
    if (!has_probe_res) {
        pthread_rwlock_unlock(&hcv2->lock);
        goto RESCHEDULE;
    }

    /* Determine the node to be processed in this tick */
    bondp_hcv2_node_t *node = hcv2->probe_cur_node;
    if (node == NULL) {
        if (!ub_list_is_empty(&hcv2->node_list)) {
            node = CONTAINER_OF_FIELD(hcv2->node_list.next, bondp_hcv2_node_t, node_entry);
        }
    }

    if (node == NULL) {
        pthread_rwlock_unlock(&hcv2->lock);
        goto RESCHEDULE;
    }

    /* Send all valid paths of the current node (2D matrix traversal), update node state uniformly after completion */
    pthread_rwlock_rdlock(&node->lock);
    for (int i = 0; i < URMA_UBAGG_DEV_MAX_NUM; ++i) {
        for (int j = 0; j < URMA_UBAGG_DEV_MAX_NUM; ++j) {
            bondp_hcv2_path_t *path = &node->paths[i][j];
            if (!path->valid) {
                continue;
            }
            bondp_hcv2_send_probe(hcv2, node, i, j, path);
        }
    }
    bondp_hcv2_update_node_link_state(node);
    pthread_rwlock_unlock(&node->lock);

    /* Move to the next node; set NULL at tail, start from head next time */
    if (node->node_entry.next != &hcv2->node_list) {
        hcv2->probe_cur_node = CONTAINER_OF_FIELD(node->node_entry.next, bondp_hcv2_node_t, node_entry);
    } else {
        hcv2->probe_cur_node = NULL;
    }

    pthread_rwlock_unlock(&hcv2->lock);

RESCHEDULE:
    /* Reschedule next global probe */
    bondp_worker_task_id_t task_id = 0;
    if (bondp_worker_schedule(BONDP_HCV2_PROBE_INTERVAL_MS, bondp_hcv2_probe_task_fn, hcv2, &task_id) == 0) {
        hcv2->probe_task_id = task_id;
    }
}

/* Update node_link_ok based on all path link_ok states under the node (node is ok if any path is ok) */
static void bondp_hcv2_update_node_link_state(bondp_hcv2_node_t *node)
{
    bool any_ok = false;
    for (int i = 0; i < URMA_UBAGG_DEV_MAX_NUM; ++i) {
        for (int j = 0; j < URMA_UBAGG_DEV_MAX_NUM; ++j) {
            bondp_hcv2_path_t *path = &node->paths[i][j];
            if (!path->valid) {
                continue;
            }
            if (atomic_load(&path->link_ok)) {
                any_ok = true;
                goto DONE;
            }
        }
    }
DONE:
    atomic_store(&node->node_link_ok, any_ok);
}

/*
 * Create a new health-check node.
 * Create probe_path for each active local device, attach the first vjetty_ref,
 * and start worker periodic probe task. Return NULL on failure.
 */
static bondp_hcv2_node_t *bondp_hcv2_create_node(uint32_t node_id,
                                                 bondp_context_t *bdp_ctx, bondp_comp_t *cfg_jetty, bondp_target_jetty_t *bdp_tjetty)
{
    bondp_hcv2_node_t *node = (bondp_hcv2_node_t *)calloc(1, sizeof(bondp_hcv2_node_t));
    if (node == NULL) {
        return NULL;
    }

    node->node_id = node_id;
    node->node_eid = bdp_tjetty->v_tjetty.id.eid;
    atomic_init(&node->node_link_ok, true);
    pthread_rwlock_init(&node->lock, NULL);
    ub_list_init(&node->vjetty_list);
    ub_list_init(&node->node_entry);
    node->refcnt = 0;
    node->path_count = 0;

    /* Fill 2D paths[l][t] based on whether bdp_tjetty->p_tjetty[l][t] is non-NULL */
    for (uint32_t l = 0; l < URMA_UBAGG_DEV_MAX_NUM; ++l) {
        for (uint32_t t = 0; t < URMA_UBAGG_DEV_MAX_NUM; ++t) {
            if (bdp_tjetty->p_tjetty[l][t] == NULL) {
                continue;
            }
            bondp_hcv2_path_t *path = &node->paths[l][t];
            if (path->valid) {
                continue;
            }
            path->target_idx = (int)t;
            path->valid = true;
            atomic_init(&path->link_ok, true);
            path->last_cqe_ts_us = bondp_hcv2_get_monotonic_us();
            node->path_count++;
        }
    }

    node->primary_tjetty = bdp_tjetty;

    /* Attach the first vjetty ref */
    bondp_hcv2_vjetty_ref_t *ref = (bondp_hcv2_vjetty_ref_t *)calloc(1, sizeof(bondp_hcv2_vjetty_ref_t));
    if (ref != NULL) {
        ref->cfg_jetty = cfg_jetty;
        ref->bdp_tjetty = bdp_tjetty;
        ub_list_push_back(&node->vjetty_list, &ref->entry);
        node->refcnt = 1;
    }

    return node;
}

/* Destroy node: destroy all probe_paths, free vjetty_ref and node memory */
static void bondp_hcv2_destroy_node(bondp_hcv2_node_t *node)
{
    if (node == NULL) {
        return;
    }

    for (int i = 0; i < URMA_UBAGG_DEV_MAX_NUM; ++i) {
        for (int j = 0; j < URMA_UBAGG_DEV_MAX_NUM; ++j) {
            memset(&node->paths[i][j], 0, sizeof(node->paths[i][j]));
        }
    }

    bondp_hcv2_vjetty_ref_t *ref = NULL;
    bondp_hcv2_vjetty_ref_t *next = NULL;
    UB_LIST_FOR_EACH_SAFE (ref, next, entry, &node->vjetty_list) {
        ub_list_remove(&ref->entry);
        free(ref);
    }

    pthread_rwlock_destroy(&node->lock);
    free(node);
}

/* ==================================================================== */
/*                         External interfaces                          */
/* ==================================================================== */

/* ==================================================================== */
/*                    Per-context probe resource setup                    */
/* ==================================================================== */

/**
 * bondp_hc_v2_setup_ctx - Called from bondp_create_context(), creates out-of-band probe resources (jfce/jfc/jfr/jetty/seg) for each active local device
 * and registers jfce fd to bondp_worker epoll.
 *
 * Uses bdp_ctx->p_ctxs[i] as urma context, creates if non-NULL.
 * All resources are shared per local_idx for subsequent node probes.
 */
int bondp_hc_v2_setup_ctx(bondp_context_t *bdp_ctx)
{
    if (bdp_ctx == NULL) {
        return -1;
    }

    if (bdp_ctx->hc_v2 != NULL) {
        return 0;
    }

    bondp_hcv2_ctx_t *hcv2 = (bondp_hcv2_ctx_t *)calloc(1, sizeof(bondp_hcv2_ctx_t));
    if (hcv2 == NULL) {
        URMA_LOG_ERR("hcv2: failed to alloc per-ctx hcv2\n");
        return -1;
    }

    pthread_rwlock_init(&hcv2->lock, NULL);
    ub_list_init(&hcv2->node_list);

    if (bondp_hash_table_create(&hcv2->node_table, BONDP_HCV2_NODE_TABLE_SIZE,
                                bondp_hcv2_node_comp_f, bondp_hcv2_node_free_f,
                                bondp_hcv2_node_hash_f) != 0) {
        URMA_LOG_ERR("hcv2: failed to create node table\n");
        goto ERR;
    }

    if (bondp_hash_table_create(&hcv2->tjetty_map, BONDP_HCV2_TJETTY_MAP_SIZE,
                                bondp_hcv2_tjetty_comp_f, bondp_hcv2_tjetty_free_f,
                                bondp_hcv2_tjetty_hash_f) != 0) {
        URMA_LOG_ERR("hcv2: failed to create tjetty map\n");
        goto ERR_NODE_TABLE;
    }

    int inited_cnt = 0;

    for (int i = 0; i < URMA_UBAGG_DEV_MAX_NUM; ++i) {
        if (bdp_ctx->p_ctxs[i] == NULL || hcv2->probe_dev_inited[i]) {
            continue;
        }

        urma_context_t *p_ctx = bdp_ctx->p_ctxs[i];

        /* 1. Create jfce */
        urma_jfce_t *jfce = urma_create_jfce(p_ctx);
        if (jfce == NULL) {
            URMA_LOG_ERR("hcv2: failed to create probe jfce, lidx=%d\n", i);
            continue;
        }

        /* 2. Create jfc */
        urma_jfc_cfg_t jfc_cfg = {
            .depth = 4096,
            .flag = {.value = 0},
            .jfce = jfce,
            .user_ctx = 0,
        };
        urma_jfc_t *jfc = urma_create_jfc(p_ctx, &jfc_cfg);
        if (jfc == NULL) {
            URMA_LOG_ERR("hcv2: failed to create probe jfc, lidx=%d\n", i);
            urma_delete_jfce(jfce);
            continue;
        }

        /* 3. Create jfr (simplest recv queue, shared by probe jetty) */
        urma_jfr_cfg_t jfr_cfg = {
            .id = 0,
            .depth = 1,
            .flag = {.value = 0},
            .trans_mode = URMA_TM_RM,
            .max_sge = 1,
            .min_rnr_timer = 0,
            .jfc = jfc,
            .token_value = {.token = 0},
            .user_ctx = 0,
        };
        urma_jfr_t *jfr = urma_create_jfr(p_ctx, &jfr_cfg);
        if (jfr == NULL) {
            URMA_LOG_ERR("hcv2: failed to create probe jfr, lidx=%d\n", i);
            urma_delete_jfc(jfc);
            urma_delete_jfce(jfce);
            continue;
        }

        /* 4. Lazy-alloc global probe buffer */
        if (hcv2->probe_buf == NULL) {
            hcv2->probe_buf = memalign(BONDP_HCV2_PROBE_ALIGN, BONDP_HCV2_PROBE_BUF_LEN);
            if (hcv2->probe_buf == NULL) {
                URMA_LOG_ERR("hcv2: failed to alloc probe buf\n");
                urma_delete_jfr(jfr);
                urma_delete_jfc(jfc);
                urma_delete_jfce(jfce);
                continue;
            }
            hcv2->probe_buf_len = BONDP_HCV2_PROBE_BUF_LEN;
        }

        /* 5. Register seg */
        urma_seg_cfg_t seg_cfg = {
            .va = (uint64_t)hcv2->probe_buf,
            .len = BONDP_HCV2_PROBE_BUF_LEN,
            .token_id = NULL,
            .token_value = {.token = 0},
            .flag = {
                .bs.token_policy = URMA_TOKEN_NONE,
                .bs.cacheable = URMA_NON_CACHEABLE,
                .bs.access = URMA_ACCESS_WRITE | URMA_ACCESS_READ,
                .bs.reserved = 0,
            },
            .user_ctx = 0,
            .iova = 0,
        };
        urma_target_seg_t *seg = urma_register_seg(p_ctx, &seg_cfg);
        if (seg == NULL) {
            URMA_LOG_ERR("hcv2: failed to register probe seg, lidx=%d\n", i);
            urma_delete_jfr(jfr);
            urma_delete_jfc(jfc);
            urma_delete_jfce(jfce);
            continue;
        }

        /* 6. Create probe jetty */
        urma_jetty_cfg_t p_cfg = {
            .id = 0,
            .flag = {.bs = {.share_jfr = URMA_SHARE_JFR}},
            .jfs_cfg = {
                .depth = 1,
                .flag = {.value = 0},
                .trans_mode = URMA_TM_RM,
                .priority = 0,
                .max_sge = 1,
                .max_rsge = 0,
                .max_inline_data = 0,
                .rnr_retry = 7,
                .err_timeout = 0,
                .jfc = jfc,
                .user_ctx = 0,
            },
            .shared.jfr = jfr,
            .shared.jfc = NULL,
            .jetty_grp = NULL,
            .user_ctx = 0,
        };
        urma_jetty_t *jetty = urma_create_jetty(p_ctx, &p_cfg);
        if (jetty == NULL) {
            URMA_LOG_ERR("hcv2: failed to create probe jetty, lidx=%d\n", i);
            urma_unregister_seg(seg);
            urma_delete_jfr(jfr);
            urma_delete_jfc(jfc);
            urma_delete_jfce(jfce);
            continue;
        }

        /* 7. Store and register fd to worker */
        hcv2->probe_jfce[i] = jfce;
        hcv2->probe_jfc[i] = jfc;
        hcv2->probe_jfr[i] = jfr;
        hcv2->probe_jetty[i] = jetty;
        hcv2->probe_seg[i] = seg;
        hcv2->probe_jfce_fd[i] = jfce->fd;
        hcv2->probe_dev_inited[i] = true;

        bondp_hcv2_jfce_arg_t *jarg = (bondp_hcv2_jfce_arg_t *)calloc(1, sizeof(bondp_hcv2_jfce_arg_t));
        if (jarg != NULL) {
            jarg->hcv2 = hcv2;
            jarg->local_idx = i;
            hcv2->jfce_arg[i] = jarg;
            if (bondp_worker_add_fd(jfce->fd, bondp_hcv2_jfce_handler, jarg) != 0) {
                URMA_LOG_ERR("hcv2: failed to add probe jfce fd to worker, lidx=%d\n", i);
            }
        }

        inited_cnt++;
        URMA_LOG_INFO("hcv2: probe resources created for local_idx=%d\n", i);
    }

    if (inited_cnt == 0) {
        URMA_LOG_WARN("hcv2: no probe resources created for this ctx\n");
        goto ERR_TJETTY_MAP;
    }

    /* Start periodic task after first probe resource creation (only once) */
    if (hcv2->probe_task_id == 0) {
        bondp_worker_task_id_t task_id = 0;
        if (bondp_worker_schedule(BONDP_HCV2_PROBE_INTERVAL_MS, bondp_hcv2_probe_task_fn, hcv2, &task_id) == 0) {
            hcv2->probe_task_id = task_id;
            URMA_LOG_INFO("hcv2: probe task scheduled\n");
        }
    }

    bdp_ctx->hc_v2 = hcv2;
    return 0;

ERR_TJETTY_MAP:
    bondp_hash_table_destroy(&hcv2->tjetty_map);
ERR_NODE_TABLE:
    bondp_hash_table_destroy(&hcv2->node_table);
ERR:
    pthread_rwlock_destroy(&hcv2->lock);
    free(hcv2);
    return -1;
}

/**
 * bondp_hc_v2_cleanup_ctx - Called from bondp_delete_context(), cleans up all out-of-band probe resources created by this ctx
 * and removes corresponding fd from bondp_worker epoll.
 */
void bondp_hc_v2_cleanup_ctx(bondp_context_t *bdp_ctx)
{
    if (bdp_ctx == NULL || bdp_ctx->hc_v2 == NULL) {
        return;
    }

    bondp_hcv2_ctx_t *hcv2 = bdp_ctx->hc_v2;
    bdp_ctx->hc_v2 = NULL;

    /* 1. Cancel periodic task first to prevent task_fn from being triggered during destroy */
    if (hcv2->probe_task_id != 0) {
        (void)bondp_worker_cancel(hcv2->probe_task_id);
        hcv2->probe_task_id = 0;
        URMA_LOG_INFO("hcv2: probe task cancelled\n");
    }

    /* 2. Clean up probe resources (URMA resources) */
    for (int i = 0; i < URMA_UBAGG_DEV_MAX_NUM; ++i) {
        if (!hcv2->probe_dev_inited[i]) {
            continue;
        }

        (void)bondp_worker_del_fd(hcv2->probe_jfce_fd[i]);

        if (hcv2->probe_jetty[i] != NULL) {
            urma_delete_jetty(hcv2->probe_jetty[i]);
            hcv2->probe_jetty[i] = NULL;
        }
        if (hcv2->probe_jfr[i] != NULL) {
            urma_delete_jfr(hcv2->probe_jfr[i]);
            hcv2->probe_jfr[i] = NULL;
        }
        if (hcv2->probe_seg[i] != NULL) {
            urma_unregister_seg(hcv2->probe_seg[i]);
            hcv2->probe_seg[i] = NULL;
        }
        if (hcv2->probe_jfc[i] != NULL) {
            urma_delete_jfc(hcv2->probe_jfc[i]);
            hcv2->probe_jfc[i] = NULL;
        }
        if (hcv2->probe_jfce[i] != NULL) {
            urma_delete_jfce(hcv2->probe_jfce[i]);
            hcv2->probe_jfce[i] = NULL;
        }
        if (hcv2->jfce_arg[i] != NULL) {
            free(hcv2->jfce_arg[i]);
            hcv2->jfce_arg[i] = NULL;
        }

        hcv2->probe_dev_inited[i] = false;
        hcv2->probe_jfce_fd[i] = -1;
    }

    if (hcv2->probe_buf != NULL) {
        free(hcv2->probe_buf);
        hcv2->probe_buf = NULL;
    }

    /* 3. Destroy nodes and hash tables under hcv2->lock protection
     *    Order: first destroy hash table (remove nodes from ub_hmap without freeing node memory),
     *           then traverse node_list to free node memory.
     *    Avoids use-after-free caused by free(node) before ub_hmap_remove.
     */
    pthread_rwlock_wrlock(&hcv2->lock);

    bondp_hash_table_destroy(&hcv2->tjetty_map);
    bondp_hash_table_destroy(&hcv2->node_table);

    bondp_hcv2_node_t *node = NULL;
    bondp_hcv2_node_t *next = NULL;
    UB_LIST_FOR_EACH_SAFE (node, next, node_entry, &hcv2->node_list) {
        ub_list_remove(&node->node_entry);
        bondp_hcv2_destroy_node(node);
    }

    pthread_rwlock_unlock(&hcv2->lock);

    pthread_rwlock_destroy(&hcv2->lock);

    free(hcv2);
    URMA_LOG_INFO("hcv2: per-ctx resources cleaned up\n");
}

/*
 * Called when import_jetty: register remote node into health-check system.
 * If node already exists, append vjetty_ref; otherwise create new node.
 * Return 0 on success, 0 when topo unavailable (skip), -1 on failure.
 */
int bondp_hc_v2_register_node(bondp_context_t *bdp_ctx,
                              bondp_comp_t *cfg_jetty, bondp_target_jetty_t *bdp_tjetty)
{
    if (bdp_ctx == NULL || bdp_ctx->hc_v2 == NULL || cfg_jetty == NULL || bdp_tjetty == NULL) {
        return -1;
    }

    bondp_hcv2_ctx_t *hcv2 = bdp_ctx->hc_v2;

    const urma_eid_t *dst_eid = &bdp_tjetty->v_tjetty.id.eid;
    uint32_t node_id = 0;
    if (bondp_hcv2_find_node_id_by_eid(bdp_ctx->topo_map, dst_eid, &node_id) != 0) {
        URMA_LOG_WARN("hcv2: cannot resolve node-id from eid, skip registration\n");
        return 0;
    }

    uint32_t hash = bondp_hcv2_node_hash(node_id);
    pthread_rwlock_wrlock(&hcv2->lock);

    hmap_node_t *n = bondp_hash_table_lookup_without_lock(&hcv2->node_table, &node_id, hash);
    if (n != NULL) {
        bondp_hcv2_node_t *node = CONTAINER_OF_FIELD(n, bondp_hcv2_node_t, hmap_node);

        /* Defensive duplicate registration check: each bdp_tjetty can only be registered once */
        uint32_t tmap_hash = bondp_hcv2_tjetty_hash(bdp_tjetty);
        hmap_node_t *existing = bondp_hash_table_lookup_without_lock(&hcv2->tjetty_map, bdp_tjetty, tmap_hash);
        if (existing != NULL) {
            pthread_rwlock_unlock(&hcv2->lock);
            return 0;
        }

        /* Allocate reverse mapping entry first to avoid the complexity of rolling back paths later */
        bondp_hcv2_tjetty_map_t *tmap = (bondp_hcv2_tjetty_map_t *)calloc(1, sizeof(bondp_hcv2_tjetty_map_t));
        if (tmap == NULL) {
            pthread_rwlock_unlock(&hcv2->lock);
            return -1;
        }

        /* Append vjetty to existing node */
        bondp_hcv2_vjetty_ref_t *ref = (bondp_hcv2_vjetty_ref_t *)calloc(1, sizeof(bondp_hcv2_vjetty_ref_t));
        if (ref == NULL) {
            free(tmap);
            pthread_rwlock_unlock(&hcv2->lock);
            return -1;
        }
        ref->cfg_jetty = cfg_jetty;
        ref->bdp_tjetty = bdp_tjetty;

        pthread_rwlock_wrlock(&node->lock);
        ub_list_push_back(&node->vjetty_list, &ref->entry);
        node->refcnt++;

        /* Merge new vjetty's paths (union): p_tjetty[l][t] non-NULL means valid path */
        for (uint32_t l = 0; l < URMA_UBAGG_DEV_MAX_NUM; ++l) {
            for (uint32_t t = 0; t < URMA_UBAGG_DEV_MAX_NUM; ++t) {
                if (bdp_tjetty->p_tjetty[l][t] == NULL) {
                    continue;
                }
                bondp_hcv2_path_t *path = &node->paths[l][t];
                if (!path->valid) {
                    path->target_idx = (int)t;
                    path->valid = true;
                    atomic_init(&path->link_ok, true);
                    path->last_cqe_ts_us = bondp_hcv2_get_monotonic_us();
                    node->path_count++;
                }
            }
        }
        pthread_rwlock_unlock(&node->lock);

        tmap->bdp_tjetty = bdp_tjetty;
        tmap->node = node;
        bondp_hash_table_add_with_hash_without_lock(&hcv2->tjetty_map, &tmap->hmap_node, tmap_hash);

        pthread_rwlock_unlock(&hcv2->lock);
        URMA_LOG_INFO("hcv2: vjetty appended to node=%u, refcnt=%u\n", node_id, node->refcnt);
        return 0;
    }

    /* Create new node */
    bondp_hcv2_node_t *node = bondp_hcv2_create_node(node_id, bdp_ctx, cfg_jetty, bdp_tjetty);
    if (node == NULL) {
        pthread_rwlock_unlock(&hcv2->lock);
        URMA_LOG_ERR("hcv2: failed to create node=%u\n", node_id);
        return -1;
    }

    bondp_hcv2_tjetty_map_t *tmap = (bondp_hcv2_tjetty_map_t *)calloc(1, sizeof(bondp_hcv2_tjetty_map_t));
    if (tmap == NULL) {
        bondp_hcv2_destroy_node(node);
        pthread_rwlock_unlock(&hcv2->lock);
        return -1;
    }

    bondp_hash_table_add_with_hash_without_lock(&hcv2->node_table, &node->hmap_node, hash);
    ub_list_push_back(&hcv2->node_list, &node->node_entry);

    tmap->bdp_tjetty = bdp_tjetty;
    tmap->node = node;
    uint32_t tmap_hash = bondp_hcv2_tjetty_hash(bdp_tjetty);
    bondp_hash_table_add_with_hash_without_lock(&hcv2->tjetty_map, &tmap->hmap_node, tmap_hash);

    pthread_rwlock_unlock(&hcv2->lock);
    URMA_LOG_INFO("hcv2: node=%u registered, path_count=%u\n", node_id, node->path_count);
    return 0;
}

/*
 * Called when unimport_jetty: unregister node from health-check system or decrement reference.
 * Lookup node via tjetty_map, remove vjetty_ref, destroy entire node when refcnt reaches zero.
 */
void bondp_hc_v2_unregister_node(bondp_context_t *bdp_ctx, bondp_target_jetty_t *bdp_tjetty)
{
    if (bdp_ctx == NULL || bdp_ctx->hc_v2 == NULL || bdp_tjetty == NULL) {
        return;
    }

    bondp_hcv2_ctx_t *hcv2 = bdp_ctx->hc_v2;

    uint32_t tmap_hash = bondp_hcv2_tjetty_hash(bdp_tjetty);
    pthread_rwlock_wrlock(&hcv2->lock);

    hmap_node_t *tm = bondp_hash_table_lookup_without_lock(&hcv2->tjetty_map, bdp_tjetty, tmap_hash);
    if (tm == NULL) {
        pthread_rwlock_unlock(&hcv2->lock);
        return;
    }

    bondp_hcv2_tjetty_map_t *tmap = CONTAINER_OF_FIELD(tm, bondp_hcv2_tjetty_map_t, hmap_node);
    bondp_hcv2_node_t *node = tmap->node;

    bondp_hash_table_remove_without_lock(&hcv2->tjetty_map, tm);
    free(tmap);

    pthread_rwlock_wrlock(&node->lock);
    bondp_hcv2_vjetty_ref_t *ref = NULL;
    bondp_hcv2_vjetty_ref_t *ref_next = NULL;
    UB_LIST_FOR_EACH_SAFE (ref, ref_next, entry, &node->vjetty_list) {
        if (ref->bdp_tjetty == bdp_tjetty) {
            ub_list_remove(&ref->entry);
            free(ref);
            node->refcnt--;
            break;
        }
    }

    /* If primary_tjetty is unregistered, elect a new primary from remaining vjettys */
    if (node->primary_tjetty == bdp_tjetty && node->refcnt > 0) {
        bondp_hcv2_vjetty_ref_t *first =
            CONTAINER_OF_FIELD(node->vjetty_list.next, bondp_hcv2_vjetty_ref_t, entry);
        node->primary_tjetty = first->bdp_tjetty;
    }

    /* Clean up paths no longer supported by any remaining vjetty */
    for (int l = 0; l < URMA_UBAGG_DEV_MAX_NUM; ++l) {
        for (int t = 0; t < URMA_UBAGG_DEV_MAX_NUM; ++t) {
            bondp_hcv2_path_t *path = &node->paths[l][t];
            if (!path->valid) {
                continue;
            }
            bool still_needed = false;
            bondp_hcv2_vjetty_ref_t *r = NULL;
            UB_LIST_FOR_EACH (r, entry, &node->vjetty_list) {
                if (r->bdp_tjetty->p_tjetty[l][t] != NULL) {
                    still_needed = true;
                    break;
                }
            }
            if (!still_needed) {
                memset(path, 0, sizeof(*path));
                node->path_count--;
            }
        }
    }

    uint32_t remaining = node->refcnt;
    pthread_rwlock_unlock(&node->lock);

    /* When refcnt reaches zero, remove node from global table first, then release lock, finally destroy node resources */
    if (remaining == 0) {
        /* If probe_cur_node points to the node about to be deleted, update to next or prev in advance */
        if (hcv2->probe_cur_node == node) {
            if (node->node_entry.next != &hcv2->node_list) {
                hcv2->probe_cur_node = CONTAINER_OF_FIELD(node->node_entry.next,
                                                          bondp_hcv2_node_t, node_entry);
            } else if (node->node_entry.prev != &hcv2->node_list) {
                hcv2->probe_cur_node = CONTAINER_OF_FIELD(node->node_entry.prev,
                                                          bondp_hcv2_node_t, node_entry);
            } else {
                hcv2->probe_cur_node = NULL; /* Only one node */
            }
        }

        ub_list_remove(&node->node_entry);
        bondp_hash_table_remove_without_lock(&hcv2->node_table, &node->hmap_node);
        pthread_rwlock_unlock(&hcv2->lock);

        bondp_hcv2_destroy_node(node);
        URMA_LOG_INFO("hcv2: node destroyed after last vjetty unregistered\n");
    } else {
        pthread_rwlock_unlock(&hcv2->lock);
        URMA_LOG_INFO("hcv2: vjetty unregistered, remaining refcnt=%u\n", remaining);
    }
}

/* Query link state of path for node_id + (local_idx, target_idx), default true for unknown */
bool bondp_hc_v2_is_node_link_ok(bondp_context_t *bdp_ctx, uint32_t node_id,
                                 uint32_t local_idx, uint32_t target_idx)
{
    if (bdp_ctx == NULL || bdp_ctx->hc_v2 == NULL) {
        return true;
    }
    if (local_idx >= URMA_UBAGG_DEV_MAX_NUM || target_idx >= URMA_UBAGG_DEV_MAX_NUM) {
        return false;
    }

    bondp_hcv2_ctx_t *hcv2 = bdp_ctx->hc_v2;
    uint32_t hash = bondp_hcv2_node_hash(node_id);
    pthread_rwlock_rdlock(&hcv2->lock);
    hmap_node_t *n = bondp_hash_table_lookup_without_lock(&hcv2->node_table, &node_id, hash);
    if (n == NULL) {
        pthread_rwlock_unlock(&hcv2->lock);
        return true; /* Unknown node treated as ok */
    }

    bondp_hcv2_node_t *node = CONTAINER_OF_FIELD(n, bondp_hcv2_node_t, hmap_node);
    bondp_hcv2_path_t *path = &node->paths[local_idx][target_idx];
    bool ok = path->valid ? atomic_load(&path->link_ok) : false;
    pthread_rwlock_unlock(&hcv2->lock);
    return ok;
}

/* Query aggregated node-level link state for node_id (node is ok if any path is ok), default true for unknown nodes */
bool bondp_hc_v2_is_node_ok(bondp_context_t *bdp_ctx, uint32_t node_id)
{
    if (bdp_ctx == NULL || bdp_ctx->hc_v2 == NULL) {
        return true;
    }

    bondp_hcv2_ctx_t *hcv2 = bdp_ctx->hc_v2;
    uint32_t hash = bondp_hcv2_node_hash(node_id);
    pthread_rwlock_rdlock(&hcv2->lock);
    hmap_node_t *n = bondp_hash_table_lookup_without_lock(&hcv2->node_table, &node_id, hash);
    if (n == NULL) {
        pthread_rwlock_unlock(&hcv2->lock);
        return true; /* Unknown node treated as ok */
    }

    bondp_hcv2_node_t *node = CONTAINER_OF_FIELD(n, bondp_hcv2_node_t, hmap_node);
    bool ok = atomic_load(&node->node_link_ok);
    pthread_rwlock_unlock(&hcv2->lock);
    return ok;
}
