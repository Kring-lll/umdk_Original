/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: jetty pool implementation for UMQ UB
 * Create: 2025-06-08
 * Note:
 * History: 2025-06-08
 */

#include <sys/eventfd.h>
#include <unistd.h>

#include "umq_vlog.h"
#include "umq_errno.h"
#include "urpc_thread_closure.h"
#include "umq_ub_private.h"
#include "umq_ub_jetty_pool.h"

static __thread thread_local_jetty_cache_t g_thread_jetty_cache = {0};
static jetty_pool_t g_jetty_pool;
static _Atomic bool g_jetty_pool_inited = false;

// Forward declarations
static void release_thread_cache(uint64_t id);

static ALWAYS_INLINE void recycle_node_to_free_q(jetty_pool_t *pool, jetty_pool_node_t *node)
{
    node->sub_umq = NULL;
    __atomic_store_n(&node->state, JETTY_POOL_NODE_IDLE, __ATOMIC_RELEASE);
    node->in_global_pool = true;
    urpc_list_push_back(&pool->free_q, &node->node);
    pool->free_count++;
}

static ALWAYS_INLINE void logic_umq_borrow_jetty(ub_queue_t *queue, jetty_pool_node_t *node)
{
    queue->jetty[UB_QUEUE_JETTY_IO] = node->sub_umq->jetty[UB_QUEUE_JETTY_IO];
    queue->jetty[UB_QUEUE_JETTY_FLOW_CONTROL] = node->sub_umq->jetty[UB_QUEUE_JETTY_FLOW_CONTROL];
    queue->jfs_jfc[UB_QUEUE_JETTY_IO] = node->sub_umq->jfs_jfc[UB_QUEUE_JETTY_IO];
    queue->jfs_jfc[UB_QUEUE_JETTY_FLOW_CONTROL] = node->sub_umq->jfs_jfc[UB_QUEUE_JETTY_FLOW_CONTROL];
    queue->jetty_node = node;
}

static ALWAYS_INLINE thread_local_jetty_cache_t *get_thread_jetty_cache(void)
{
    if (!g_thread_jetty_cache.inited) {
        urpc_list_init(&g_thread_jetty_cache.cache_list);
        urpc_list_init(&g_thread_jetty_cache.registry_node);
        g_thread_jetty_cache.cached_count = 0;
        g_thread_jetty_cache.inited = true;
        urpc_thread_closure_register(THREAD_CLOSURE_JETTY_POOL, 0, release_thread_cache);
        (void)pthread_spin_lock(&g_jetty_pool.lock);
        urpc_list_push_back(&g_jetty_pool.thread_cache_list, &g_thread_jetty_cache.registry_node);
        (void)pthread_spin_unlock(&g_jetty_pool.lock);
    }
    return &g_thread_jetty_cache;
}

// Release all thread cache to global pool. should be called when thread exits
static ALWAYS_INLINE void release_thread_cache(uint64_t id)
{
    (void)id;
    if (!g_thread_jetty_cache.inited) {
        return;
    }

    if (!__atomic_load_n(&g_jetty_pool_inited, __ATOMIC_ACQUIRE)) {
        while (!urpc_list_is_empty(&g_thread_jetty_cache.cache_list)) {
            jetty_pool_node_t *cached = (jetty_pool_node_t *)urpc_list_pop_front(
                &g_thread_jetty_cache.cache_list);
            free(cached);
        }
        g_thread_jetty_cache.cached_count = 0;
        g_thread_jetty_cache.inited = false;
        return;
    }

    (void)pthread_spin_lock(&g_jetty_pool.lock);
    urpc_list_remove(&g_thread_jetty_cache.registry_node);
    if (!urpc_list_is_empty(&g_thread_jetty_cache.cache_list)) {
        while (!urpc_list_is_empty(&g_thread_jetty_cache.cache_list)) {
            jetty_pool_node_t *cached = (jetty_pool_node_t *)urpc_list_pop_front(
                &g_thread_jetty_cache.cache_list);
            cached->in_global_pool = true;
            if (__atomic_load_n(&cached->state, __ATOMIC_ACQUIRE) == JETTY_POOL_NODE_ERR) {
                recycle_node_to_free_q(&g_jetty_pool, cached);
            } else {
                urpc_list_push_back(&g_jetty_pool.active_q, &cached->node);
                g_jetty_pool.active_count++;
            }
        }
        g_thread_jetty_cache.cached_count = 0;
    }
    (void)pthread_spin_unlock(&g_jetty_pool.lock);

    g_thread_jetty_cache.inited = false;
}

// Validate jetty pool configuration parameters (0 means use default)
static int validate_jetty_pool_config(jetty_pool_config_t *config)
{
    uint32_t cache_size = (config->cache_size > 0) ? config->cache_size : THREAD_LOCAL_JETTY_CACHE_SIZE;
    // cache_size cannot exceed max_nodes, otherwise one thread could hold all nodes
    uint32_t max_nodes = (config->max_nodes > 0) ? config->max_nodes : JETTY_POOL_MAX_NODES;
    if (cache_size > max_nodes) {
        UMQ_VLOG_ERR(VLOG_UMQ, "cache_size %u cannot exceed max_nodes %u\n", cache_size, max_nodes);
        return -UMQ_ERR_EINVAL;
    }
    // notify_threshold cannot exceed max_nodes, otherwise eventfd will never fire
    uint32_t notify_threshold = (config->notify_threshold > 0) ?
        config->notify_threshold : JETTY_POOL_NOTIFY_THRESHOLD;
    if (notify_threshold > max_nodes) {
         UMQ_VLOG_ERR(VLOG_UMQ, "notify_threshold %u cannot exceed max_nodes %u\n", notify_threshold, max_nodes);
        return -UMQ_ERR_EINVAL;
    }
    return UMQ_SUCCESS;
}

int umq_ub_jetty_pool_init(jetty_pool_config_t *config)
{
    if (__atomic_load_n(&g_jetty_pool_inited, __ATOMIC_ACQUIRE)) {
        UMQ_VLOG_ERR(VLOG_UMQ, "jetty pool already exists, only one pool allowed per process\n");
        return -UMQ_ERR_EEXIST;
    }

    int ret = validate_jetty_pool_config(config);
    if (ret != UMQ_SUCCESS) {
        return ret;
    }

    memset(&g_jetty_pool, 0, sizeof(g_jetty_pool));

    urpc_list_init(&g_jetty_pool.free_q);
    urpc_list_init(&g_jetty_pool.active_q);
    urpc_list_init(&g_jetty_pool.thread_cache_list);

    int event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd < 0) {
        UMQ_VLOG_ERR(VLOG_UMQ, "create jetty pool eventfd failed, errno: %d\n", errno);
        return -UMQ_ERR_EINVAL;
    }

    (void)pthread_spin_init(&g_jetty_pool.lock, PTHREAD_PROCESS_PRIVATE);

    g_jetty_pool.event_fd = event_fd;
    g_jetty_pool.max_nodes = (config->max_nodes > 0) ? config->max_nodes : JETTY_POOL_MAX_NODES;
    g_jetty_pool.batch_size = (config->batch_size > 0) ? config->batch_size : THREAD_LOCAL_JETTY_BATCH_SIZE;
    g_jetty_pool.cache_size = (config->cache_size > 0) ? config->cache_size : THREAD_LOCAL_JETTY_CACHE_SIZE;
    g_jetty_pool.notify_threshold = (config->notify_threshold > 0) ?
        config->notify_threshold : JETTY_POOL_NOTIFY_THRESHOLD;
    g_jetty_pool.return_batch_size = (config->return_batch_size > 0) ?
        config->return_batch_size : THREAD_LOCAL_JETTY_RETURN_BATCH_SIZE;
    g_jetty_pool.borrow_limit = (config->borrow_limit > 0) ? config->borrow_limit : JETTY_POOL_BORROW_LIMIT;

    __atomic_store_n(&g_jetty_pool_inited, true, __ATOMIC_RELEASE);

    UMQ_VLOG_INFO(VLOG_UMQ, "init jetty pool success, eventfd %d, cache_size %u, max_nodes %u, batch_size %u, "
        "notify_threshold %u, return_batch_size %u, borrow_limit %u\n",
        event_fd, g_jetty_pool.cache_size, g_jetty_pool.max_nodes, g_jetty_pool.batch_size,
        g_jetty_pool.notify_threshold, g_jetty_pool.return_batch_size, g_jetty_pool.borrow_limit);
    return UMQ_SUCCESS;
}

void umq_ub_jetty_pool_uninit(void)
{
    if (!__atomic_load_n(&g_jetty_pool_inited, __ATOMIC_ACQUIRE)) {
        return;
    }

    __atomic_store_n(&g_jetty_pool_inited, false, __ATOMIC_RELEASE);

    (void)pthread_spin_lock(&g_jetty_pool.lock);
    thread_local_jetty_cache_t *cache = NULL;
    thread_local_jetty_cache_t *cache_tmp = NULL;
    URPC_LIST_FOR_EACH_SAFE(cache, cache_tmp, registry_node, &g_jetty_pool.thread_cache_list) {
        while (!urpc_list_is_empty(&cache->cache_list)) {
            jetty_pool_node_t *cached = (jetty_pool_node_t *)urpc_list_pop_front(&cache->cache_list);
            free(cached);
        }
        cache->cached_count = 0;
        cache->inited = false;
        urpc_list_remove(&cache->registry_node);
    }

    jetty_pool_node_t *iter = NULL;
    jetty_pool_node_t *tmp = NULL;
    URPC_LIST_FOR_EACH_SAFE(iter, tmp, node, &g_jetty_pool.free_q) {
        urpc_list_remove(&iter->node);
        free(iter);
    }
    URPC_LIST_FOR_EACH_SAFE(iter, tmp, node, &g_jetty_pool.active_q) {
        urpc_list_remove(&iter->node);
        free(iter);
    }
    (void)pthread_spin_unlock(&g_jetty_pool.lock);

    (void)close(g_jetty_pool.event_fd);
    (void)pthread_spin_destroy(&g_jetty_pool.lock);

    memset(&g_jetty_pool, 0, sizeof(g_jetty_pool));
}

static void set_reference_config(jetty_pool_t *pool, ub_queue_t *sub_umq)
{
    pool->ref_sub_cfg.rx_depth = sub_umq->rx_depth;
    pool->ref_sub_cfg.tx_depth = sub_umq->tx_depth;
    pool->ref_sub_cfg.rx_buf_size = sub_umq->rx_buf_size;
    pool->ref_sub_cfg.tx_buf_size = sub_umq->tx_buf_size;
    pool->ref_sub_cfg.order_type = sub_umq->order_type;
    pool->ref_sub_cfg.tp_mode = sub_umq->tp_mode;
    pool->ref_sub_cfg.tp_type = sub_umq->tp_type;
    pool->ref_sub_cfg.trans_mode = (uint32_t)sub_umq->umq_trans_mode;
    pool->ref_sub_cfg.create_flag = sub_umq->create_flag;
    pool->ref_sub_cfg.share_rq_umqh = sub_umq->share_rq_umqh;
    pool->ref_sub_cfg.mode = (uint32_t)sub_umq->mode;
    pool->ref_sub_cfg.priority = (uint32_t)sub_umq->priority;
    pool->ref_config_set = true;
}

// Validate that sub_umq config matches the reference (all sub_umqs in pool must be identical)
static int validate_sub_umq_config_match(jetty_pool_t *pool, ub_queue_t *sub_umq)
{
    if (!pool->ref_config_set) {
        set_reference_config(pool, sub_umq);
        return UMQ_SUCCESS; // first sub_umq, no reference yet
    }
    if (sub_umq->tp_mode != pool->ref_sub_cfg.tp_mode) {
        UMQ_VLOG_ERR(VLOG_UMQ, "sub_umq tp_mode %u mismatch with pool reference %u\n",
            sub_umq->tp_mode, pool->ref_sub_cfg.tp_mode);
        return -UMQ_ERR_EINVAL;
    }
    if (sub_umq->tp_type != pool->ref_sub_cfg.tp_type) {
        UMQ_VLOG_ERR(VLOG_UMQ, "sub_umq tp_type %u mismatch with pool reference %u\n",
            sub_umq->tp_type, pool->ref_sub_cfg.tp_type);
        return -UMQ_ERR_EINVAL;
    }
    if (sub_umq->order_type != pool->ref_sub_cfg.order_type) {
        UMQ_VLOG_ERR(VLOG_UMQ, "sub_umq order_type %u mismatch with pool reference %u\n",
            sub_umq->order_type, pool->ref_sub_cfg.order_type);
        return -UMQ_ERR_EINVAL;
    }
    if (sub_umq->rx_depth != pool->ref_sub_cfg.rx_depth) {
        UMQ_VLOG_ERR(VLOG_UMQ, "sub_umq rx_depth %u mismatch with pool reference %u\n",
            sub_umq->rx_depth, pool->ref_sub_cfg.rx_depth);
        return -UMQ_ERR_EINVAL;
    }
    if (sub_umq->tx_depth != pool->ref_sub_cfg.tx_depth) {
        UMQ_VLOG_ERR(VLOG_UMQ, "sub_umq tx_depth %u mismatch with pool reference %u\n",
            sub_umq->tx_depth, pool->ref_sub_cfg.tx_depth);
        return -UMQ_ERR_EINVAL;
    }
    if (sub_umq->rx_buf_size != pool->ref_sub_cfg.rx_buf_size) {
        UMQ_VLOG_ERR(VLOG_UMQ, "sub_umq rx_buf_size %u mismatch with pool reference %u\n",
            sub_umq->rx_buf_size, pool->ref_sub_cfg.rx_buf_size);
        return -UMQ_ERR_EINVAL;
    }
    if (sub_umq->tx_buf_size != pool->ref_sub_cfg.tx_buf_size) {
        UMQ_VLOG_ERR(VLOG_UMQ, "sub_umq tx_buf_size %u mismatch with pool reference %u\n",
            sub_umq->tx_buf_size, pool->ref_sub_cfg.tx_buf_size);
        return -UMQ_ERR_EINVAL;
    }
    if ((uint32_t)sub_umq->umq_trans_mode != pool->ref_sub_cfg.trans_mode) {
        UMQ_VLOG_ERR(VLOG_UMQ, "sub_umq umq_trans_mode %u mismatch with pool reference %u\n",
            (uint32_t)sub_umq->umq_trans_mode, pool->ref_sub_cfg.trans_mode);
        return -UMQ_ERR_EINVAL;
    }
    if (sub_umq->create_flag != pool->ref_sub_cfg.create_flag) {
        UMQ_VLOG_ERR(VLOG_UMQ, "sub_umq create_flag 0x%x mismatch with pool reference 0x%x\n",
            sub_umq->create_flag, pool->ref_sub_cfg.create_flag);
        return -UMQ_ERR_EINVAL;
    }
    if (sub_umq->share_rq_umqh != pool->ref_sub_cfg.share_rq_umqh) {
        UMQ_VLOG_ERR(VLOG_UMQ, "sub_umq share_rq_umqh 0x%lx mismatch with pool reference 0x%lx\n",
            sub_umq->share_rq_umqh, pool->ref_sub_cfg.share_rq_umqh);
        return -UMQ_ERR_EINVAL;
    }
    if ((uint32_t)sub_umq->mode != pool->ref_sub_cfg.mode) {
        UMQ_VLOG_ERR(VLOG_UMQ, "sub_umq mode %u mismatch with pool reference %u\n",
            (uint32_t)sub_umq->mode, pool->ref_sub_cfg.mode);
        return -UMQ_ERR_EINVAL;
    }
    if ((uint32_t)sub_umq->priority != pool->ref_sub_cfg.priority) {
        UMQ_VLOG_ERR(VLOG_UMQ, "sub_umq priority %u mismatch with pool reference %u\n",
            (uint32_t)sub_umq->priority, pool->ref_sub_cfg.priority);
        return -UMQ_ERR_EINVAL;
    }
    return UMQ_SUCCESS;
}

int umq_ub_jetty_pool_add(ub_queue_t *sub_umq)
{
    if (!__atomic_load_n(&g_jetty_pool_inited, __ATOMIC_ACQUIRE)) {
        UMQ_VLOG_ERR(VLOG_UMQ, "jetty pool not initialized\n");
        return -UMQ_ERR_EINVAL;
    }

    jetty_pool_t *pool = &g_jetty_pool;

    (void)pthread_spin_lock(&pool->lock);
    int ret = validate_sub_umq_config_match(pool, sub_umq);
    if (ret != UMQ_SUCCESS) {
        (void)pthread_spin_unlock(&pool->lock);
        return ret;
    }

    jetty_pool_node_t *node = NULL;
    if (pool->node_count >= pool->max_nodes) {
        (void)pthread_spin_unlock(&pool->lock);
        UMQ_VLOG_ERR(VLOG_UMQ, "max jetty nodes %u reached\n", pool->max_nodes);
        return -UMQ_ERR_EMLINK;
    } else if (!urpc_list_is_empty(&pool->free_q)) {
        node = (jetty_pool_node_t *)urpc_list_pop_front(&pool->free_q);
        pool->free_count--;
    } else {
        // Allocate under lock — small calloc, acceptable inside a spinlock.
        node = (jetty_pool_node_t *)calloc(1, sizeof(jetty_pool_node_t));
        if (node == NULL) {
            (void)pthread_spin_unlock(&pool->lock);
            UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "allocate jetty pool node failed, errno: %d\n", errno);
            return -UMQ_ERR_ENOMEM;
        }
        urpc_list_init(&node->node);
    }

    node->sub_umq = sub_umq;
    node->in_global_pool = true;
    __atomic_store_n(&node->state, JETTY_POOL_NODE_IDLE, __ATOMIC_RELEASE);
    sub_umq->jetty_node = node;

    urpc_list_push_back(&pool->active_q, &node->node);
    pool->active_count++;
    pool->node_count++;
    uint32_t active_after_add = pool->active_count;
    (void)pthread_spin_unlock(&pool->lock);

    // Notify via eventfd when active_count reaches every notify_threshold
    if (active_after_add % pool->notify_threshold == 0) {
        if (eventfd_write(pool->event_fd, (uint64_t)active_after_add) == -1) {
            UMQ_VLOG_WARN(VLOG_UMQ, "eventfd_write failed, errno: %d\n", errno);
        }
    }
    return UMQ_SUCCESS;
}

int umq_ub_jetty_pool_remove(ub_queue_t *sub_umq)
{
    if (!__atomic_load_n(&g_jetty_pool_inited, __ATOMIC_ACQUIRE)) {
        UMQ_VLOG_ERR(VLOG_UMQ, "jetty pool not initialized\n");
        return -UMQ_ERR_EINVAL;
    }
    if (sub_umq == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "sub_umq is NULL\n");
        return -UMQ_ERR_EINVAL;
    }

    jetty_pool_t *pool = &g_jetty_pool;

    jetty_pool_node_t *node = sub_umq->jetty_node;
    if (node == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "pool_node is NULL, already removed\n");
        return -UMQ_ERR_EINVAL;
    }

    // Check tx_outstanding before removal: if WRs are still in flight, refuse.
    if (__atomic_load_n(&node->sub_umq->tx_outstanding, __ATOMIC_ACQUIRE) != 0) {
        UMQ_VLOG_ERR(VLOG_UMQ, "cannot remove sub_umq, tx_outstanding %u != 0\n",
            node->sub_umq->tx_outstanding);
        return -UMQ_ERR_EBUSY;
    }

    // Try CAS IDLE→ERR to atomically claim the node.
    int expected = JETTY_POOL_NODE_IDLE;
    if (!__atomic_compare_exchange_n(&node->state, &expected, JETTY_POOL_NODE_ERR,
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        // CAS failed — node is IN_USE (borrowed by Logic UMQ) or already ERR.
        // With the new design, IN_USE nodes cannot be removed.
        UMQ_VLOG_ERR(VLOG_UMQ, "cannot remove sub_umq, node state %d (expected IDLE)\n", expected);
        return -UMQ_ERR_EBUSY;
    }

    // IDLE → ERR: node is in active_q or in some thread-local cache.
    // Transport is being destroyed, so node_count must reflect that.
    (void)pthread_spin_lock(&pool->lock);
    if (node->in_global_pool) {
        // Node in active_q: return to free_q for reassignment.
        urpc_list_remove(&node->node);
        pool->active_count--;
        recycle_node_to_free_q(pool, node);
    }
    pool->node_count--;
    (void)pthread_spin_unlock(&pool->lock);
    sub_umq->jetty_node = NULL;
    return UMQ_SUCCESS;
}

jetty_pool_node_t *umq_ub_jetty_pool_alloc(ub_queue_t *queue)
{
    if (!__atomic_load_n(&g_jetty_pool_inited, __ATOMIC_ACQUIRE)) {
        UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "jetty pool not initialized\n");
        return NULL;
    }

    jetty_pool_t *pool = &g_jetty_pool;

    thread_local_jetty_cache_t *cache = get_thread_jetty_cache();

    while (pool->active_count > 0) {
        // 1. Batch fetch from active_q to fill thread-local cache.
        //    active_q never contains ERR nodes (remove() moves them to free_q).
        if (cache->cached_count == 0) {
            (void)pthread_spin_lock(&pool->lock);
            uint32_t moved = urpc_list_move_n(&pool->active_q, &cache->cache_list, pool->batch_size);
            pool->active_count -= moved;
            // Set in_global_pool under lock: pool_remove checks this flag to decide
            // whether the node is in active_q. A TOCTOU race would corrupt the list.
            jetty_pool_node_t *iter = NULL;
            URPC_LIST_FOR_EACH(iter, node, &cache->cache_list) {
                iter->in_global_pool = false;
            }
            (void)pthread_spin_unlock(&pool->lock);
            if (moved == 0) {
                // active_q is empty — no sub_umqs in pool
                break;
            }

            cache->cached_count += moved;
        }

        // 2. Try each cached node — remove() may have marked some as ERR while in cache.
        while (cache->cached_count > 0) {
            jetty_pool_node_t *node = (jetty_pool_node_t *)urpc_list_pop_front(&cache->cache_list);
            cache->cached_count--;

            uint32_t expected = JETTY_POOL_NODE_IDLE;
            if (!__atomic_compare_exchange_n(&node->state, &expected, JETTY_POOL_NODE_IN_USE,
                                             false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                // CAS failed — node's state changed (ERR from remove())
                if (expected == JETTY_POOL_NODE_ERR) {
                    // Sub UMQ was destroyed. Reclaim to free_q for reassignment.
                    // node_count was already decremented by pool_remove.
                    (void)pthread_spin_lock(&pool->lock);
                    recycle_node_to_free_q(pool, node);
                    (void)pthread_spin_unlock(&pool->lock);
                }
                // ERR is the only non-IDLE state possible in cache (from remove()).
                continue;
            }

            // Node is now IN_USE and owned by this Logic UMQ
            logic_umq_borrow_jetty(queue, node);
            node->borrow_count = 0;
            node->borrow_limit = pool->borrow_limit;
            __atomic_store_n(&node->umq_ref, (uint64_t)(uintptr_t)queue, __ATOMIC_RELEASE);
            return node;
        }
    }

    // 3. No available jettys.
    // If pool has reached max_nodes capacity → EMLINK, else temporarily exhausted → ETOOMANYREFS
    errno = (pool->node_count == pool->max_nodes) ? UMQ_ERR_EMLINK : UMQ_ERR_ETOOMANYREFS;
    return NULL;
}

static ALWAYS_INLINE int return_node_to_cache(jetty_pool_t *pool, jetty_pool_node_t *node)
{
    node->borrow_count = 0;  // Reset for next borrow cycle
    thread_local_jetty_cache_t *cache = get_thread_jetty_cache();
    urpc_list_push_back(&cache->cache_list, &node->node);
    cache->cached_count++;

    // Check if cache exceeds limit, batch return excess to active_q
    if (cache->cached_count > pool->cache_size) {
        (void)pthread_spin_lock(&pool->lock);
        uint32_t to_mark = (pool->return_batch_size < cache->cached_count) ?
                            pool->return_batch_size : cache->cached_count;
        if (to_mark > 0) {
            jetty_pool_node_t *iter = NULL;
            uint32_t marked = 0;
            URPC_LIST_FOR_EACH(iter, node, &cache->cache_list) {
                if (marked >= to_mark) break;
                iter->in_global_pool = true;
                marked++;
            }
        }
        uint32_t cnt = urpc_list_move_n(&cache->cache_list, &pool->active_q, pool->return_batch_size);
        pool->active_count += cnt;
        cache->cached_count -= cnt;
        uint64_t value = (uint64_t)pool->active_count;
        (void)pthread_spin_unlock(&pool->lock);

        if ((value % pool->notify_threshold) < cnt) {
            if (eventfd_write(pool->event_fd, value) != 0) {
                UMQ_VLOG_WARN(VLOG_UMQ, "eventfd_write failed, errno: %d\n", errno);
            }
        }
    }
    return UMQ_SUCCESS;
}

int umq_ub_jetty_pool_free(ub_queue_t *queue)
{
    if (!__atomic_load_n(&g_jetty_pool_inited, __ATOMIC_ACQUIRE)) {
        UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "jetty pool not initialized\n");
        return -UMQ_ERR_EINVAL;
    }

    jetty_pool_t *pool = &g_jetty_pool;

    jetty_pool_node_t *node = queue->jetty_node;
    if (node == NULL) {
        UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "node is NULL\n");
        return -UMQ_ERR_EINVAL;
    }

    // Node must be IN_USE here (guaranteed by caller protocol).
    // CAS is defensive; failure would indicate a protocol violation.
    uint32_t expected = JETTY_POOL_NODE_IN_USE;
    if (!__atomic_compare_exchange_n(&node->state, &expected, JETTY_POOL_NODE_IDLE,
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "pool_free: unexpected node state %d\n", expected);
        return -UMQ_ERR_EBUSY;
    }

    return return_node_to_cache(pool, node);
}

int umq_ub_jetty_pool_get_eventfd(void)
{
    if (!__atomic_load_n(&g_jetty_pool_inited, __ATOMIC_ACQUIRE)) {
        UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "jetty pool not initialized\n");
        return -UMQ_ERR_EINVAL;
    }
    return g_jetty_pool.event_fd;
}

bool umq_ub_jetty_pool_is_inited(void)
{
    return __atomic_load_n(&g_jetty_pool_inited, __ATOMIC_ACQUIRE);
}
