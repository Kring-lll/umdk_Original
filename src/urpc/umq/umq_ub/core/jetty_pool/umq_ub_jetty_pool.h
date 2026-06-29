/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: jetty pool for UMQ UB
 * Create: 2026-06-08
 * Note:
 * History: 2026-06-08
 */

#ifndef UMQ_UB_JETTY_POOL_H
#define UMQ_UB_JETTY_POOL_H

#include <stdbool.h>
#include <pthread.h>
#include "urpc_list.h"
#include "urpc_bitmap.h"
#include "urpc_mpmc_ring.h"
#include "urma_api.h"

/* Jetty pool ring capacity — must be a power of 2.
 * The ring replaces active_q for lock-free alloc/free of jetty nodes. */
#define JETTY_POOL_RING_CAPACITY 65536

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jetty_pool_node jetty_pool_node_t;

// MPMC ring declaration for the active node pool.
// Stores jetty_pool_node_t pointers (8 bytes each).
URPC_MPMC_RING_DECLARE(jetty_active_ring, jetty_pool_node_t *, JETTY_POOL_RING_CAPACITY);

typedef struct umq_ub_jetty_node_list {
    jetty_pool_node_t **node_list;
    uint32_t list_len;
    urpc_bitmap_t bitmap;
    volatile uint32_t ref_cnt;
    pthread_spinlock_t lock;       // serializes bitmap + node_list slot mutation (create/destroy)
} umq_ub_jetty_node_list_t;

// Jetty pool configuration
typedef struct jetty_pool_config {
    uint32_t notify_threshold;     // Notify via eventfd when idle_count >= threshold (0 means use default 16)
    uint32_t borrow_limit;         // Max WRs per borrow (0 means use default 1024)
} jetty_pool_config_t;

int umq_ub_jetty_pool_init(jetty_pool_config_t *config);
void umq_ub_jetty_pool_uninit(void);
jetty_pool_node_t *umq_ub_jetty_pool_get_free_node(void);
void umq_ub_jetty_pool_put_free_node(jetty_pool_node_t *node);
int umq_ub_jetty_node_add(jetty_pool_node_t *node);
int umq_ub_jetty_node_remove(jetty_pool_node_t *node);
jetty_pool_node_t *umq_ub_jetty_node_alloc(void);
int umq_ub_jetty_node_free(jetty_pool_node_t *node);
int umq_ub_jetty_pool_get_eventfd(void);
umq_ub_jetty_node_list_t *umq_ub_jetty_pool_get_jetty_node_list(void);
uint32_t umq_ub_jetty_pool_put_jetty_node_list(umq_ub_jetty_node_list_t *jetty_node_list);
void umq_ub_jetty_node_mark_err(jetty_pool_node_t *node);

// DFX statistics
typedef struct umq_ub_jetty_pool_stats {
    uint64_t total_num;         // Total nodes currently tracked (node_count)
    uint64_t global_num;        // IDLE nodes in global active_ring
    uint64_t in_use_num;        // Nodes currently borrowed by a Logic UMQ (state == IN_USE)
    uint64_t err_num;           // Nodes marked is_jetty_err == true
    uint64_t acc_alloc_num;     // Cumulative allocs (nodes borrowed by Logic UMQ)
    uint64_t acc_free_num;      // Cumulative frees (nodes returned to pool)
} umq_ub_jetty_pool_stats_t;

int umq_ub_jetty_pool_stats_get(umq_ub_jetty_pool_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif