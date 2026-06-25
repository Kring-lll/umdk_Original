/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: jetty pool implementation for UMQ UB
 * Create: 2026-06-08
 * Note:
 * History: 2026-06-08
 */

#include <sys/eventfd.h>
#include <unistd.h>

#include "umq_vlog.h"
#include "umq_errno.h"
#include "umq_ub_private.h"
#include "umq_ub_jetty_pool.h"

#define JETTY_POOL_NOTIFY_THRESHOLD 1
#define JETTY_POOL_MAX_NODES 65536
#define JETTY_POOL_BORROW_LIMIT 1024  // Max WRs per borrow (prevents jetty hogging)

typedef struct jetty_pool {
    umq_ub_jetty_node_list_t jetty_node_list;
    urpc_list_t free_q;             // Pre-allocated nodes NOT assigned to any Sub UMQ
    jetty_active_ring_t active_ring; // Lock-free MPMC ring: nodes available for Logic UMQ
    urpc_list_t relay_q;
    pthread_spinlock_t lock;        // Pool-level lock (minimal lock usage)
    int event_fd;                   // Eventfd for idle jetty notification

    uint32_t free_count;            // Current count in free_q (protected by lock)
    uint64_t in_use_count;          // Nodes currently borrowed by a Logic UMQ (state == IN_USE)
    uint64_t err_count;             // Nodes marked with is_jetty_err == true
    uint64_t acc_alloc_count;        // Cumulative allocs (nodes borrowed by Logic UMQ)
    uint64_t acc_free_count;         // Cumulative frees (nodes returned to pool)
    uint32_t node_count;            // Total allocated nodes
    uint32_t max_nodes;             // Max nodes allowed in pool (0 means use default JETTY_POOL_MAX_NODES)

    uint32_t notify_threshold;      // Notify via eventfd when idle_count >= threshold (default 16)
    uint32_t borrow_limit;          // Max WRs per borrow (default JETTY_POOL_BORROW_LIMIT)
} jetty_pool_t;

static jetty_pool_t g_jetty_pool;
static bool g_jetty_pool_inited = false;

static ALWAYS_INLINE void recycle_node_to_free_q(jetty_pool_t *pool, jetty_pool_node_t *node)
{
    __atomic_store_n(&node->state, JETTY_POOL_NODE_IDLE, __ATOMIC_RELEASE);
    node->in_global_pool = true;
    urpc_list_push_back(&pool->free_q, &node->node);
    pool->free_count++;
}

static ALWAYS_INLINE void recycle_node_to_relay_q(jetty_pool_t *pool, jetty_pool_node_t *node)
{
    __atomic_store_n(&node->state, JETTY_POOL_NODE_IDLE, __ATOMIC_RELEASE);
    node->in_global_pool = true;
    urpc_list_push_back(&pool->relay_q, &node->node);
}

static int umq_ub_jetty_node_list_init(umq_ub_jetty_node_list_t *jetty_node_list, uint32_t node_cnt)
{
    if (node_cnt == 0 || node_cnt > JETTY_POOL_MAX_NODES) {
        UMQ_VLOG_ERR(VLOG_UMQ, "node cnt %u invalid\n", node_cnt);
        return -UMQ_ERR_EINVAL;
    }

    jetty_node_list->list_len = node_cnt;
    jetty_node_list->bitmap = urpc_bitmap_alloc(node_cnt);
    if (jetty_node_list->bitmap == NULL) {
        UMQ_VLOG_INFO(VLOG_UMQ, "alloc bitmap failed\n");
        return -UMQ_ERR_ENOMEM;
    }

    jetty_node_list->node_list = (jetty_pool_node_t **)calloc(node_cnt, sizeof(jetty_pool_node_t *));
    if (jetty_node_list->node_list == NULL) {
        urpc_bitmap_free(jetty_node_list->bitmap);
        UMQ_VLOG_INFO(VLOG_UMQ, "calloc node list failed\n");
        return -UMQ_ERR_ENOMEM;
    }

    (void)pthread_spin_init(&jetty_node_list->lock, PTHREAD_PROCESS_PRIVATE);
    return UMQ_SUCCESS;
}

int umq_ub_jetty_pool_init(jetty_pool_config_t *config)
{
    if (g_jetty_pool_inited) {
        UMQ_VLOG_ERR(VLOG_UMQ, "jetty pool already exists, only one pool allowed per process\n");
        return -UMQ_ERR_EEXIST;
    }

    urpc_list_init(&g_jetty_pool.free_q);
    urpc_mpmc_ring_init(&g_jetty_pool.active_ring);
    urpc_list_init(&g_jetty_pool.relay_q);

    int event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd < 0) {
        UMQ_VLOG_ERR(VLOG_UMQ, "create jetty pool eventfd failed, errno: %d\n", errno);
        return -UMQ_ERR_EINVAL;
    }

    g_jetty_pool.event_fd = event_fd;
    g_jetty_pool.max_nodes = JETTY_POOL_MAX_NODES;
    g_jetty_pool.notify_threshold = (config->notify_threshold > 0) ?
        config->notify_threshold : JETTY_POOL_NOTIFY_THRESHOLD;
    g_jetty_pool.borrow_limit = (config->borrow_limit > 0) ? config->borrow_limit : JETTY_POOL_BORROW_LIMIT;

    int ret = umq_ub_jetty_node_list_init(&g_jetty_pool.jetty_node_list, g_jetty_pool.max_nodes);
    if (ret != UMQ_SUCCESS) {
        goto CLOSE_FD;
    }

    (void)pthread_spin_init(&g_jetty_pool.lock, PTHREAD_PROCESS_PRIVATE);
    g_jetty_pool_inited = true;
    return UMQ_SUCCESS;

CLOSE_FD:
    (void)close(g_jetty_pool.event_fd);
    return ret;
}

static void umq_ub_jetty_node_list_uninit(umq_ub_jetty_node_list_t *jetty_node_list)
{
    if (jetty_node_list->node_list != NULL) {
        jetty_node_list->list_len = 0;
        free(jetty_node_list->node_list);
    }

    if (jetty_node_list->bitmap != NULL) {
        urpc_bitmap_free(jetty_node_list->bitmap);
    }
    (void)pthread_spin_destroy(&jetty_node_list->lock);
}

void umq_ub_jetty_pool_uninit(void)
{
    if (!g_jetty_pool_inited) {
        return;
    }

    g_jetty_pool_inited = false;

    umq_ub_jetty_node_list_uninit(&g_jetty_pool.jetty_node_list);

    jetty_pool_node_t *iter = NULL;
    jetty_pool_node_t *tmp = NULL;
    URPC_LIST_FOR_EACH_SAFE(iter, tmp, node, &g_jetty_pool.free_q) {
        urpc_list_remove(&iter->node);
        free(iter);
    }
    /* Drain MPMC ring: each remaining node is still IDLE and must be freed. */
    while (urpc_mpmc_try_dequeue(&g_jetty_pool.active_ring, &iter)) {
        free(iter);
    }
    URPC_LIST_FOR_EACH_SAFE(iter, tmp, node, &g_jetty_pool.relay_q) {
        urpc_list_remove(&iter->node);
        free(iter);
    }

    (void)close(g_jetty_pool.event_fd);
    (void)pthread_spin_destroy(&g_jetty_pool.lock);

    memset(&g_jetty_pool, 0, sizeof(g_jetty_pool));
}

jetty_pool_node_t *umq_ub_jetty_pool_get_free_node(void)
{
    jetty_pool_node_t *node = NULL;
    (void)pthread_spin_lock(&g_jetty_pool.lock);
    if (!urpc_list_is_empty(&g_jetty_pool.free_q)) {
        node = (jetty_pool_node_t *)urpc_list_pop_front(&g_jetty_pool.free_q);
        g_jetty_pool.free_count--;
        pthread_spin_unlock(&g_jetty_pool.lock);
        memset(node, 0, sizeof(jetty_pool_node_t));
        return node;
    }
    (void)pthread_spin_unlock(&g_jetty_pool.lock);

    return (jetty_pool_node_t *)calloc(1, sizeof(jetty_pool_node_t));
}

void umq_ub_jetty_pool_put_free_node(jetty_pool_node_t *node)
{
    if (node == NULL) {
        return;
    }
    (void)pthread_spin_lock(&g_jetty_pool.lock);
    urpc_list_push_back(&g_jetty_pool.free_q, &node->node);
    g_jetty_pool.free_count++;
    (void)pthread_spin_unlock(&g_jetty_pool.lock);
}

int umq_ub_jetty_node_add(jetty_pool_node_t *node)
{
    if (!g_jetty_pool_inited) {
        UMQ_VLOG_ERR(VLOG_UMQ, "jetty pool not initialized\n");
        return -UMQ_ERR_EINVAL;
    }

    jetty_pool_t *pool = &g_jetty_pool;
    jetty_pool_node_t *add_node = node;

    (void)pthread_spin_lock(&pool->lock);
    if (pool->node_count >= pool->max_nodes) {
        (void)pthread_spin_unlock(&pool->lock);
        UMQ_VLOG_ERR(VLOG_UMQ, "max jetty nodes %u reached\n", pool->max_nodes);
        return -UMQ_ERR_EMLINK;
    }
    add_node->in_global_pool = true;
    __atomic_store_n(&add_node->state, JETTY_POOL_NODE_IDLE, __ATOMIC_RELEASE);
    pool->node_count++;
    (void)pthread_spin_unlock(&pool->lock);

    /* Lock-free enqueue into MPMC ring (replaces list push + active_count++). */
    urpc_mpmc_enqueue(&pool->active_ring, &add_node);

    /* Notify eventfd consumers that at least one node is available. */
    if (eventfd_write(pool->event_fd, (uint64_t)1) == -1) {
        UMQ_VLOG_WARN(VLOG_UMQ, "eventfd_write failed, errno: %d\n", errno);
    }

    return UMQ_SUCCESS;
}

int umq_ub_jetty_node_remove(jetty_pool_node_t *node)
{
    if (!g_jetty_pool_inited) {
        UMQ_VLOG_ERR(VLOG_UMQ, "jetty pool not initialized\n");
        return -UMQ_ERR_EINVAL;
    }

    if (node == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "pool_node is NULL, already removed\n");
        return -UMQ_ERR_EINVAL;
    }

    jetty_pool_t *pool = &g_jetty_pool;

    // Try CAS IDLE→ERR to atomically claim the node.
    // If the node is still in the ring, the next alloc() will dequeue it,
    // see ERR state, and recycle to free_q. No explicit ring removal needed.
    int expected = JETTY_POOL_NODE_IDLE;
    if (!__atomic_compare_exchange_n(&node->state, &expected, JETTY_POOL_NODE_ERR,
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        UMQ_VLOG_ERR(VLOG_UMQ, "cannot remove sub_umq, node state %d (expected IDLE)\n", expected);
        return -UMQ_ERR_EBUSY;
    }

    (void)pthread_spin_lock(&pool->lock);
    pool->node_count--;
    (void)pthread_spin_unlock(&pool->lock);

    if (node->is_jetty_err) {
        (void)__atomic_sub_fetch(&pool->err_count, 1, __ATOMIC_RELAXED);
    }
    return UMQ_SUCCESS;
}

jetty_pool_node_t *umq_ub_jetty_node_alloc(void)
{
    if (!g_jetty_pool_inited) {
        UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "jetty pool not initialized\n");
        return NULL;
    }

    jetty_pool_t *pool = &g_jetty_pool;

    while (1) {
        jetty_pool_node_t *node = NULL;

        /* Lock-free dequeue from MPMC ring. */
        if (!urpc_mpmc_try_dequeue(&pool->active_ring, &node)) {
            /* Ring is empty — no nodes available. */
            break;
        }

        /* CAS IDLE→IN_USE: atomically claim ownership.
         * A node in the ring is guaranteed to be IDLE (add sets it,
         * free restores it before enqueue). ERR nodes are recycled
         * by remove() via CAS — if we see ERR here, the node was
         * marked bad but still sits in the ring. */
        uint32_t expected = JETTY_POOL_NODE_IDLE;
        if (!__atomic_compare_exchange_n(&node->state, &expected, JETTY_POOL_NODE_IN_USE,
                                         false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            /* Node is ERR or already IN_USE — recycle to free_q. */
            if (expected == JETTY_POOL_NODE_ERR || node->is_jetty_err) {
                (void)pthread_spin_lock(&pool->lock);
                recycle_node_to_free_q(pool, node);
                (void)pthread_spin_unlock(&pool->lock);
            }
            /* Try next node in ring. */
            continue;
        }

        /* Successfully borrowed a healthy IDLE node. */
        node->borrow_count = 0;
        node->borrow_limit = pool->borrow_limit;
        (void)__atomic_add_fetch(&pool->in_use_count, 1, __ATOMIC_RELAXED);
        (void)__atomic_add_fetch(&pool->acc_alloc_count, 1, __ATOMIC_RELAXED);
        return node;
    }

    /* No available jettys in the ring. */
    UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "No available jetty\n");
    errno = UMQ_ERR_EMLINK;
    return NULL;
}

int umq_ub_jetty_node_free(jetty_pool_node_t *node)
{
    if (!g_jetty_pool_inited) {
        UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "jetty pool not initialized\n");
        return -UMQ_ERR_EINVAL;
    }

    jetty_pool_t *pool = &g_jetty_pool;
    (void)__atomic_sub_fetch(&pool->in_use_count, 1, __ATOMIC_RELAXED);
    (void)__atomic_add_fetch(&pool->acc_free_count, 1, __ATOMIC_RELAXED);

    if (node->is_jetty_err) {
        (void)pthread_spin_lock(&pool->lock);
        recycle_node_to_relay_q(pool, node);
        (void)pthread_spin_unlock(&pool->lock);
        return UMQ_SUCCESS;
    }

    /* Restore IDLE state and re-enqueue directly into the MPMC ring.
     * No thread-local cache — the ring is fast enough for single-element
     * operations (~25 ns with one CAS). */
    __atomic_store_n(&node->state, JETTY_POOL_NODE_IDLE, __ATOMIC_RELEASE);
    node->borrow_count = 0;

    urpc_mpmc_enqueue(&pool->active_ring, &node);

    return UMQ_SUCCESS;
}

int umq_ub_jetty_pool_get_eventfd(void)
{
    if (!g_jetty_pool_inited) {
        UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "jetty pool not initialized\n");
        return -UMQ_ERR_EINVAL;
    }
    return g_jetty_pool.event_fd;
}

umq_ub_jetty_node_list_t *umq_ub_jetty_pool_get_jetty_node_list(void)
{
    (void)__atomic_fetch_add(&g_jetty_pool.jetty_node_list.ref_cnt, 1, __ATOMIC_RELAXED);
    return &g_jetty_pool.jetty_node_list;
}

uint32_t umq_ub_jetty_pool_put_jetty_node_list(umq_ub_jetty_node_list_t *jetty_node_list)
{
    return __atomic_sub_fetch(&jetty_node_list->ref_cnt, 1, __ATOMIC_ACQ_REL);
}

void umq_ub_jetty_node_mark_err(jetty_pool_node_t *node)
{
    if (node == NULL) {
        return;
    }
    bool expected = false;
    if (!__atomic_compare_exchange_n(&node->is_jetty_err, &expected, true, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return; // was already true
    }
    (void)__atomic_add_fetch(&g_jetty_pool.err_count, 1, __ATOMIC_RELAXED);
}

int umq_ub_jetty_pool_stats_get(umq_ub_jetty_pool_stats_t *stats)
{
    if (stats == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "stats is NULL\n");
        return -UMQ_ERR_EINVAL;
    }
    if (!g_jetty_pool_inited) {
        memset(stats, 0, sizeof(*stats));
        return UMQ_SUCCESS;
    }
    jetty_pool_t *pool = &g_jetty_pool;
    (void)pthread_spin_lock(&pool->lock);
    stats->total_num = pool->node_count;
    (void)pthread_spin_unlock(&pool->lock);
    stats->global_num = urpc_mpmc_count(&pool->active_ring);
    stats->cache_num = 0;  // Thread-local cache removed with MPMC ring
    stats->in_use_num = __atomic_load_n(&pool->in_use_count, __ATOMIC_RELAXED);
    stats->err_num = __atomic_load_n(&pool->err_count, __ATOMIC_RELAXED);
    stats->acc_alloc_num = __atomic_load_n(&pool->acc_alloc_count, __ATOMIC_RELAXED);
    stats->acc_free_num = __atomic_load_n(&pool->acc_free_count, __ATOMIC_RELAXED);
    return UMQ_SUCCESS;
}

