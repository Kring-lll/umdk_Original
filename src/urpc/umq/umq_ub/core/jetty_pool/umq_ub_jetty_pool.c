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
#include "umq_ub_jetty_pool.h"

static __thread thread_local_jetty_cache_t g_thread_jetty_cache = {0};
static jetty_pool_t *g_jetty_pool = NULL;  // Global singleton jetty pool

// Forward declarations
static void release_thread_cache(uint64_t id);

// Helper: reset an ERR node's fields and push it back to free_q for reassignment.
// node_count was already decremented by pool_remove, so we only bump free_count.
static ALWAYS_INLINE void recycle_node_to_free_q(jetty_pool_t *pool, jetty_pool_node_t *node)
{
    node->sub_umq = NULL;
    node->umq_ref = 0;
    __atomic_store_n((int *)&node->state, JETTY_POOL_NODE_IDLE, __ATOMIC_RELEASE);
    node->in_global_pool = true;
    urpc_list_push_back(&pool->free_q, &node->node);
    pool->free_count++;
}

// Helper: borrow sub_umq's jettys and jfs_jfc for a Logic UMQ
static ALWAYS_INLINE void logic_umq_borrow_jetty(ub_queue_t *queue, jetty_pool_node_t *node)
{
    queue->jetty[UB_QUEUE_JETTY_IO] = node->sub_umq->jetty[UB_QUEUE_JETTY_IO];
    queue->jetty[UB_QUEUE_JETTY_FLOW_CONTROL] = node->sub_umq->jetty[UB_QUEUE_JETTY_FLOW_CONTROL];
    queue->jfs_jfc[UB_QUEUE_JETTY_IO] = node->sub_umq->jfs_jfc[UB_QUEUE_JETTY_IO];
    queue->jfs_jfc[UB_QUEUE_JETTY_FLOW_CONTROL] = node->sub_umq->jfs_jfc[UB_QUEUE_JETTY_FLOW_CONTROL];
    queue->pool_node = node;
}

static ALWAYS_INLINE thread_local_jetty_cache_t *get_thread_jetty_cache(void)
{
    if (!g_thread_jetty_cache.inited) {
        urpc_list_init(&g_thread_jetty_cache.cache_list);
        g_thread_jetty_cache.cached_count = 0;
        g_thread_jetty_cache.inited = true;
        urpc_thread_closure_register(THREAD_CLOSURE_JETTY_POOL, 0, release_thread_cache);
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

    // If pool is already uninitialized, we can't return nodes to it.
    // Free cached nodes directly to avoid leaking heap memory.
    if (g_jetty_pool == NULL) {
        while (!urpc_list_is_empty(&g_thread_jetty_cache.cache_list)) {
            jetty_pool_node_t *cached = (jetty_pool_node_t *)urpc_list_pop_front(
                &g_thread_jetty_cache.cache_list);
            free(cached);
        }
        g_thread_jetty_cache.cached_count = 0;
        g_thread_jetty_cache.inited = false;
        return;
    }

    if (!urpc_list_is_empty(&g_thread_jetty_cache.cache_list)) {
        (void)pthread_spin_lock(&g_jetty_pool->lock);
        while (!urpc_list_is_empty(&g_thread_jetty_cache.cache_list)) {
            jetty_pool_node_t *cached = (jetty_pool_node_t *)urpc_list_pop_front(
                &g_thread_jetty_cache.cache_list);
            cached->in_global_pool = true;
            if (__atomic_load_n(&cached->state, __ATOMIC_ACQUIRE) == JETTY_POOL_NODE_ERR) {
                // ERR node: recycle to free_q (node_count already decremented).
                recycle_node_to_free_q(g_jetty_pool, cached);
            } else {
                // IDLE: return to active_q (node still assigned to Sub UMQ)
                urpc_list_push_back(&g_jetty_pool->active_q, &cached->node);
                g_jetty_pool->active_count++;
            }
        }
        g_thread_jetty_cache.cached_count = 0;
        (void)pthread_spin_unlock(&g_jetty_pool->lock);
    }

    g_thread_jetty_cache.inited = false;
}

// Validate jetty pool configuration parameters (0 means use default)
static int validate_jetty_pool_config(jetty_pool_config_t *config)
{
    // batch_size cannot exceed cache_size
    uint32_t cache_size = (config->cache_size > 0) ? config->cache_size : THREAD_LOCAL_JETTY_CACHE_SIZE;
    // uint32_t batch_size = (config->batch_size > 0) ? config->batch_size : THREAD_LOCAL_JETTY_BATCH_SIZE;
    // if (batch_size > cache_size) {
    //     UMQ_VLOG_ERR(VLOG_UMQ, "batch_size %u cannot exceed cache_size %u\n", batch_size, cache_size);
    //     return -UMQ_ERR_EINVAL;
    // }
    // return_batch_size cannot exceed cache_size
    // uint32_t return_batch_size = (config->return_batch_size > 0) ? config->return_batch_size : THREAD_LOCAL_JETTY_RETURN_BATCH_SIZE;
    // if (return_batch_size > cache_size) {
    //     UMQ_VLOG_ERR(VLOG_UMQ, "return_batch_size %u cannot exceed cache_size %u\n", return_batch_size, cache_size);
    //     return -UMQ_ERR_EINVAL;
    // }
    // cache_size cannot exceed max_nodes, otherwise one thread could hold all nodes
    uint32_t max_nodes = (config->max_nodes > 0) ? config->max_nodes : JETTY_POOL_MAX_NODES;
    if (cache_size > max_nodes) {
        UMQ_VLOG_ERR(VLOG_UMQ, "cache_size %u cannot exceed max_nodes %u\n", cache_size, max_nodes);
        return -UMQ_ERR_EINVAL;
    }
    // notify_threshold cannot exceed max_nodes, otherwise eventfd will never fire
    uint32_t notify_threshold = (config->notify_threshold > 0) ? config->notify_threshold : THREAD_LOCAL_JETTY_CACHE_SIZE;
    if (notify_threshold > max_nodes) {
        notify_threshold = max_nodes;
    }
    return UMQ_SUCCESS;
}

int umq_ub_jetty_pool_init(jetty_pool_config_t *config)
{
    // Singleton: only one jetty pool allowed per process
    if (g_jetty_pool != NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "jetty pool already exists, only one pool allowed per process\n");
        return -UMQ_ERR_EEXIST;
    }

    // Validate configuration parameters before allocating resources
    int ret = validate_jetty_pool_config(config);
    if (ret != UMQ_SUCCESS) {
        return ret;
    }

    jetty_pool_t *pool = calloc(1, sizeof(jetty_pool_t));
    if (pool == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "allocate jetty_pool failed, errno: %d\n", errno);
        return -UMQ_ERR_ENOMEM;
    }

    urpc_list_init(&pool->free_q);
    urpc_list_init(&pool->active_q);

    int event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd < 0) {
        UMQ_VLOG_ERR(VLOG_UMQ, "create jetty pool eventfd failed, errno: %d\n", errno);
        free(pool);
        return -UMQ_ERR_EINVAL;
    }

    (void)pthread_spin_init(&pool->lock, PTHREAD_PROCESS_PRIVATE);

    pool->event_fd = event_fd;
    pool->max_nodes = (config->max_nodes > 0) ? config->max_nodes : JETTY_POOL_MAX_NODES; // default 1024
    pool->batch_size = (config->batch_size > 0) ? config->batch_size : THREAD_LOCAL_JETTY_BATCH_SIZE; // default 1
    pool->cache_size = (config->cache_size > 0) ? config->cache_size : THREAD_LOCAL_JETTY_CACHE_SIZE; // default 16
    pool->notify_threshold = (config->notify_threshold > 0) ? config->notify_threshold : THREAD_LOCAL_JETTY_CACHE_SIZE; // default 16
    if (pool->notify_threshold > pool->max_nodes) {
        pool->notify_threshold = pool->max_nodes;
    }
    pool->return_batch_size = (config->return_batch_size > 0) ? config->return_batch_size : THREAD_LOCAL_JETTY_RETURN_BATCH_SIZE; // default 1
    pool->borrow_limit = (config->borrow_limit > 0) ? config->borrow_limit : JETTY_POOL_BORROW_LIMIT; // default 1024

    g_jetty_pool = pool;

    UMQ_VLOG_INFO(VLOG_UMQ, "init jetty pool success, eventfd %d, cache_size %u, max_nodes %u, batch_size %u, "
        "notify_threshold %u, return_batch_size %u, borrow_limit %u\n",
        event_fd, pool->cache_size, pool->max_nodes, pool->batch_size,
        pool->notify_threshold, pool->return_batch_size, pool->borrow_limit);
    return UMQ_SUCCESS;
}

// Reset current thread's cache (discard any cached nodes)
static void reset_thread_cache(void)
{
    g_thread_jetty_cache.cached_count = 0;
    g_thread_jetty_cache.inited = false;
}

void umq_ub_jetty_pool_uninit(void)
{
    jetty_pool_t *pool = g_jetty_pool;
    if (pool == NULL) {
        reset_thread_cache();
        return;
    }

    // Flush current thread's cache then free all remaining nodes
    if (g_thread_jetty_cache.inited && !urpc_list_is_empty(&g_thread_jetty_cache.cache_list)) {
        while (!urpc_list_is_empty(&g_thread_jetty_cache.cache_list)) {
            jetty_pool_node_t *cached = (jetty_pool_node_t *)urpc_list_pop_front(
                &g_thread_jetty_cache.cache_list);
            free(cached);
        }
        g_thread_jetty_cache.cached_count = 0;
    }
    reset_thread_cache();

    // Free nodes in all pool lists
    jetty_pool_node_t *iter = NULL;
    jetty_pool_node_t *tmp = NULL;
    URPC_LIST_FOR_EACH_SAFE(iter, tmp, node, &pool->free_q) {
        urpc_list_remove(&iter->node);
        free(iter);
    }
    URPC_LIST_FOR_EACH_SAFE(iter, tmp, node, &pool->active_q) {
        urpc_list_remove(&iter->node);
        free(iter);
    }

    (void)close(pool->event_fd);
    (void)pthread_spin_destroy(&pool->lock);

    g_jetty_pool = NULL;
    free(pool);
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
    jetty_pool_t *pool = g_jetty_pool;
    if (pool == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "jetty pool not initialized\n");
        return -UMQ_ERR_EINVAL;
    }

    // Single lock region: config check + node provision + active_q update
    // kept atomic to prevent TOCTOU races between free_q pop and node_count increment.
    (void)pthread_spin_lock(&pool->lock);
    int ret = validate_sub_umq_config_match(pool, sub_umq);
    if (ret != UMQ_SUCCESS) {
        (void)pthread_spin_unlock(&pool->lock);
        return ret;
    }

    jetty_pool_node_t *node = NULL;
    if (!urpc_list_is_empty(&pool->free_q)) {
        node = (jetty_pool_node_t *)urpc_list_pop_front(&pool->free_q);
        pool->free_count--;
    } else if (pool->node_count >= pool->max_nodes) {
        (void)pthread_spin_unlock(&pool->lock);
        UMQ_VLOG_ERR(VLOG_UMQ, "max jetty nodes %u reached\n", pool->max_nodes);
        return -UMQ_ERR_EMLINK;
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

    if (node != NULL) {
        urpc_list_push_back(&pool->active_q, &node->node);
        pool->active_count++;
        pool->node_count++;
        node->in_global_pool = true;
    }
    uint32_t active_after_add = pool->active_count;
    (void)pthread_spin_unlock(&pool->lock);

    if (node == NULL) {
        return -UMQ_ERR_EMLINK; // should not reach here, defensive
    }

    // Init node for this Sub UMQ (fields not protected by pool lock)
    node->sub_umq = sub_umq;
    node->umq_ref = 0;
    __atomic_store_n((int *)&node->state, JETTY_POOL_NODE_IDLE, __ATOMIC_RELEASE);

    // Sub UMQ saves pool_node for cleanup on destroy
    sub_umq->pool_node = node;

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
    jetty_pool_t *pool = g_jetty_pool;
    if (pool == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "jetty pool not initialized\n");
        return -UMQ_ERR_EINVAL;
    }
    if (sub_umq == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "sub_umq is NULL\n");
        return -UMQ_ERR_EINVAL;
    }

    jetty_pool_node_t *node = sub_umq->pool_node;
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
    if (!__atomic_compare_exchange_n((int *)&node->state, &expected, JETTY_POOL_NODE_ERR,
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
    sub_umq->pool_node = NULL;
    return UMQ_SUCCESS;
}

jetty_pool_node_t *umq_ub_jetty_pool_alloc(ub_queue_t *queue)
{
    jetty_pool_t *pool = g_jetty_pool;
    if (pool == NULL) {
        UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "jetty pool not initialized\n");
        return NULL;
    }

    thread_local_jetty_cache_t *cache = get_thread_jetty_cache();

    // Outer loop: retry batch-fetch if cache emptied by ERR discards
    while (1) {
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

            int expected = JETTY_POOL_NODE_IDLE;
            if (!__atomic_compare_exchange_n((int *)&node->state, &expected, JETTY_POOL_NODE_IN_USE,
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
            node->umq_ref = (uint64_t)(uintptr_t)queue;   // Mark ownership for poll/post coordination
            return node;
        }
    }

    // 3. No available jettys.
    // If pool has reached max_nodes capacity → EMLINK, else temporarily exhausted → ETOOMANYREFS
    errno = umq_ub_jetty_pool_is_full() ? UMQ_ERR_EMLINK : UMQ_ERR_ETOOMANYREFS;
    return NULL;
}

static inline int return_node_to_cache(jetty_pool_t *pool, jetty_pool_node_t *node)
{
    node->umq_ref = 0;
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
    jetty_pool_t *pool = g_jetty_pool;
    if (pool == NULL) {
        UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "jetty pool not initialized\n");
        return -UMQ_ERR_EINVAL;
    }

    jetty_pool_node_t *node = queue->pool_node;
    if (node == NULL) {
        UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "node is NULL\n");
        return -UMQ_ERR_EINVAL;
    }

    if (__atomic_load_n(&node->sub_umq->tx_outstanding, __ATOMIC_ACQUIRE) != 0) {
        UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "cannot return jetty node, tx_outstanding on sub_umq %u\n",
            node->sub_umq->tx_outstanding);
        return -UMQ_ERR_EBUSY;
    }

    // Node must be IN_USE here (guaranteed by caller protocol).
    // CAS is defensive; failure would indicate a protocol violation.
    int expected = JETTY_POOL_NODE_IN_USE;
    if (!__atomic_compare_exchange_n((int *)&node->state, &expected, JETTY_POOL_NODE_IDLE,
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "pool_free: unexpected node state %d\n", expected);
        return -UMQ_ERR_EBUSY;
    }

    return return_node_to_cache(pool, node);
}

int umq_ub_jetty_pool_get_eventfd(void)
{
    jetty_pool_t *pool = g_jetty_pool;
    if (pool == NULL) {
        UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "jetty pool not initialized\n");
        return -UMQ_ERR_EINVAL;
    }
    return pool->event_fd;
}

bool umq_ub_jetty_pool_is_inited(void)
{
    return g_jetty_pool != NULL;
}

bool umq_ub_jetty_pool_is_full(void)
{
    jetty_pool_t *pool = g_jetty_pool;
    if (pool == NULL) {
        return false;
    }
    return __atomic_load_n(&pool->node_count, __ATOMIC_RELAXED) >= pool->max_nodes;
}
