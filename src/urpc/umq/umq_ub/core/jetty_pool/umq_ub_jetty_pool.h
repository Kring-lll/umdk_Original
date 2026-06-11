/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: jetty pool for UMQ UB
 * Create: 2025-06-08
 * Note:
 * History: 2025-06-08
 */

#ifndef UMQ_UB_JETTY_POOL_H
#define UMQ_UB_JETTY_POOL_H

#include <stdbool.h>
#include <pthread.h>
#include "urpc_list.h"
#include "urma_api.h"
#include "umq_ub_private.h"

#ifdef __cplusplus
extern "C" {
#endif

#define THREAD_LOCAL_JETTY_CACHE_SIZE 0
#define THREAD_LOCAL_JETTY_BATCH_SIZE 1  // Default batch size for allocating from global pool to cache
#define THREAD_LOCAL_JETTY_RETURN_BATCH_SIZE 1 // Default batch size for returning from cache to global pool
#define JETTY_POOL_NOTIFY_THRESHOLD 1
#define JETTY_POOL_MAX_NODES 1024
#define JETTY_POOL_BORROW_LIMIT 1024  // Max WRs per borrow (prevents jetty hogging)

// Jetty pool configuration
typedef struct jetty_pool_config {
    uint32_t cache_size;           // Thread-local cache size (0 means use default 16)
    uint32_t max_nodes;            // Max nodes in pool (0 means use default 1024)
    uint32_t batch_size;           // Batch size for allocating from global pool to cache (0 means use default 1)
    uint32_t notify_threshold;     // Notify via eventfd when idle_count >= threshold (0 means use default 16)
    uint32_t return_batch_size;    // Batch size for returning from cache to global pool (0 means use default 1)
    uint32_t borrow_limit;         // Max WRs per borrow (0 means use default 1024)
} jetty_pool_config_t;

// Forward declarations
typedef struct ub_queue ub_queue_t;
typedef struct jetty_pool jetty_pool_t;
typedef struct jetty_pool_node jetty_pool_node_t;

typedef enum jetty_pool_node_state {
    JETTY_POOL_NODE_IDLE,             // node is in free_q/active_q or thread-local cache
    JETTY_POOL_NODE_IN_USE,           // node is borrowed by a Logic UMQ
    JETTY_POOL_NODE_ERR,              // node has error, should not be allocated
} jetty_pool_node_state_t;

typedef struct jetty_pool_node {
    urpc_list_t node;                   // For list linkage (cache / free_q / active_q / err_q)
    ub_queue_t *sub_umq;                // Current owner Sub UMQ's queue
    volatile uint32_t state;            // Track node state (atomic CAS operations)
    volatile uint64_t umq_ref;          // Logic UMQ handle (atomic, used for poll/post coordination)
    bool in_global_pool;                // Whether node is in a global pool list (free_q/active_q/err_q)
    uint32_t borrow_count;              // WRs posted in current borrow cycle
    uint32_t borrow_limit;              // Max WRs per borrow (0 = unlimited)
} jetty_pool_node_t;

typedef struct jetty_pool_sub_umq_template {
    uint32_t rx_depth;
    uint32_t tx_depth;
    uint32_t rx_buf_size;
    uint32_t tx_buf_size;
    uint32_t order_type;
    uint32_t tp_mode;
    uint32_t tp_type;
    uint32_t trans_mode;       // umq_trans_mode: UMQ_TRANS_MODE_UB / UMQ_TRANS_MODE_UB_PLUS
    uint32_t create_flag;      // Creation flags (only SUB_UMQ + SHARE_TRANSPORT bits)
    uint64_t share_rq_umqh;   // Handle of the shared RQ
    uint32_t mode;             // Queue mode: UMQ_MODE_POLLING / UMQ_MODE_INTERRUPT
    uint32_t priority;         // Priority of the queue
} jetty_pool_sub_umq_template_t;

typedef struct jetty_pool {
    urpc_list_t free_q;             // Pre-allocated nodes NOT assigned to any Sub UMQ
    urpc_list_t active_q;           // Nodes assigned to Sub UMQ, available for Logic UMQ
    pthread_spinlock_t lock;        // Pool-level lock (minimal lock usage)
    int event_fd;                   // Eventfd for idle jetty notification

    uint32_t free_count;            // Current count in free_q (protected by lock)
    uint32_t active_count;          // Current count in active_q (protected by lock)
    uint32_t node_count;            // Total allocated nodes
    uint32_t max_nodes;             // Max nodes allowed in pool (0 means use default JETTY_POOL_MAX_NODES)

    uint32_t batch_size;            // Batch size for allocating from active_q to cache (default 1)
    uint32_t cache_size;            // Thread-local cache size (default 16)
    uint32_t notify_threshold;      // Notify via eventfd when active_count >= threshold (default 16)
    uint32_t return_batch_size;     // Batch size for returning from cache to active_q (default 1)
    uint32_t borrow_limit;          // Max WRs per borrow (default JETTY_POOL_BORROW_LIMIT)

    // Reference Sub UMQ config for consistency check (all sub_umqs in pool must have identical params)
    jetty_pool_sub_umq_template_t ref_sub_cfg;
    bool ref_config_set;
} jetty_pool_t;

// Thread-local jetty cache (linked list based on user config)
typedef struct thread_local_jetty_cache {
    urpc_list_t cache_list;     // Linked list of cached jetty nodes
    uint32_t cached_count;       // Current cached jetty count
    bool inited;                 // Whether this thread cache is initialized
} thread_local_jetty_cache_t;

int umq_ub_jetty_pool_init(jetty_pool_config_t *config);
void umq_ub_jetty_pool_uninit(void);
int umq_ub_jetty_pool_add(ub_queue_t *sub_umq);
int umq_ub_jetty_pool_remove(ub_queue_t *sub_umq);
jetty_pool_node_t *umq_ub_jetty_pool_alloc(ub_queue_t *queue);
int umq_ub_jetty_pool_free(ub_queue_t *queue);
int umq_ub_jetty_pool_get_eventfd(void);
bool umq_ub_jetty_pool_is_inited(void);

#ifdef __cplusplus
}
#endif

#endif