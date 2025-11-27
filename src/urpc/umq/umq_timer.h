/**
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: umq timer
 * Create: 2025-11-03
 */

#ifndef UMQ_TIMER_H
#define UMQ_TIMER_H

#include <stdbool.h>
#include <stdalign.h>
#include <assert.h>

#include "urpc_list.h"
#include "urpc_util.h"

#include <pthread.h>

#define UMQ_TIMER_TLS_MAX_COUNT 2048
#define UMQ_TIMER_BATCH_COUNT 512
#define UMQ_TIMER_PAYLOAD_SIZE 32     // The max payload size behind umq_timer
#define UMQ_TIMER_TOTAL_COUNT 1048576 // UMQ has 1M timers at all

#ifdef __cplusplus
extern "C" {
#endif

typedef struct umq_timer_pool_cfg umq_timer_pool_cfg_t;
typedef struct umq_timer_pool umq_timer_pool_t;

typedef struct umq_timer {
    urpc_list_t node;       // The intrusive list node
    umq_timer_pool_t *pool; // The timer pool
    uint64_t id;            // The timer id
    uint64_t expiration;    // The absolute expiration timestamp
    void (*func)(void *);   // The callback function
    void *arg;              // The function arg
} umq_timer_t;

static_assert(_Alignof(umq_timer_t) == 8, "The payload can be put at the end of the timer");
static_assert((sizeof(umq_timer_t) + UMQ_TIMER_PAYLOAD_SIZE) % 8 == 0, "The payload can be put at the end of the timer");

/**
 * @brief Create a timer. @p arg will be deep-copied into timer.
 * @param[in] pool    The timer pool used to reduce malloc latency during data plane
 * @param[in] func    The callback
 * @param[in] arg     The pointer to arg
 * @param[in] sz      The size of arg
 * @param[in] timeout The timeout in milliseconds
 * @return umq_timer_t*, or NULL if malloc failed
 * @note The id field is filled with the address of @c umq_timer_t itself.
 * @seealso umq_timer_create_with_id
 */
umq_timer_t *umq_timer_create(umq_timer_pool_t *pool, void (*func)(void *), void *arg, size_t sz, uint64_t timeout);

/**
 * @brief Create a timer with user specified @p id. @p arg will be deep-copied into timer.
 * @param[in] pool    The timer pool used to reduce malloc latency during data plane
 * @param[in] func    The callback
 * @param[in] arg     The pointer to arg
 * @param[in] sz      The size of arg
 * @param[in] timeout The timeout in milliseconds
 * @param[in] id      The user given timer id
 * @return umq_timer_t*, or NULL if malloc failed
 * @note Users should guarantee the @p id is unique.
 * @seealso umq_timer_create
 */
umq_timer_t *umq_timer_create_with_id(umq_timer_pool_t *pool, void (*func)(void *), void *arg, size_t sz,
                                      uint64_t timeout, uint64_t id);

/**
 * @brief Destroy the timer which is created by umq_timer_create and umq_timer_create_with_id.
 * @param[in] timer The timer will be freed
 */
void umq_timer_destroy(umq_timer_t *timer);

/**
 * @brief Check whether a timer is expired or not
 * @param[in] timer The timer
 * @return true if the timer is expired, otherwise false
 */
bool umq_timer_expired(const umq_timer_t *timer, uint64_t now);

/**
 * @brief Invoke the timer callback with stored arg.
 * @param[in] timer The timer
 */
static inline void umq_timer_invoke(const umq_timer_t *timer) {
    if (URPC_LIKELY(timer != NULL)) {
        timer->func(timer->arg);
    }
}

typedef struct umq_timer_list {
    urpc_list_t head;
} umq_timer_list_t;

void umq_timer_list_init(umq_timer_list_t *list);
void umq_timer_list_uninit(umq_timer_list_t *list);

/**
 * @brief Get the earliest timer
 * @param[in] list The timer list
 * @return The timer, or NULL if @p list is empty
 */
umq_timer_t *umq_timer_list_top(umq_timer_list_t *list);

/**
 * @brief Drop the earliest timer
 * @param[in] list The timer list
 * @pre The @p list is not empty.
 * @seealso umq_timer_list_add
 * @seealso umq_timer_list_top
 */
void umq_timer_list_pop(umq_timer_list_t *list);

/**
 * @brief Add a new timer to list, all timers are in ascending order by the expiration field.
 * @param[in] list  The timer list
 * @param[in] timer The new timer
 */
void umq_timer_list_add(umq_timer_list_t *list, umq_timer_t *timer);

/**
 * @brief Add a new timer to the end of the list with no ordering count.
 * @param[in] list  The timer list
 * @param[in] timer The new timer
 */
static inline void umq_timer_list_push_back(umq_timer_list_t *list, umq_timer_t *timer)
{
    urpc_list_push_back(&list->head, &timer->node);
}

/**
 * @brief Remove the timer with user given id.
 * @param[in] list The timer list
 * @param[in] id   The user given timer id
 * @return The removed timer, or NULL if not found
 */
umq_timer_t *umq_timer_list_remove_by_id(umq_timer_list_t *list, uint64_t id);

/**
 * @brief Get the expired timers into @p out.
 * @param[in]  list The timer list
 * @param[out] out  The out timer list
 * @param[in]  now  The current timestamp in milliseconds
 * @return The list of expired timers
 * @pre The @p out timer list is initialized by umq_timer_list_init.
 */
void umq_timer_list_get_expired(umq_timer_list_t *list, umq_timer_list_t *out, uint64_t now);

/**
 * @brief Do expire the timers.
 * @param[in] list The timer list
 * @return The count of expired timers
 */
int umq_timer_list_do_expire(umq_timer_list_t *list);

struct umq_timer_pool_cfg {
    void *buf_addr;      // The start address of pool
    uint64_t num_block;  // The number of blocks
    uint64_t block_size; // The size of a block
};

/**
 * @c umq_timer_pool can be chained if the parent is non-null. In this codebase,
 * we use two levels of pool: level 1 thread-local timer pool, and level 2 the
 * global timer pool.
 */
struct umq_timer_pool {
    bool inited;

    umq_timer_pool_cfg_t cfg;  // The configuration of timer pool. Only meaningful for memory owner
    umq_timer_pool_t *parent;  // The parent timer pool. The pool is the memory owner if it's NULL
    umq_timer_list_t freelist; // The available timers in this pool
    pthread_mutex_t mutex;     // To protect freelist when concurrent allocation happens
    size_t count;              // The count of available timers

    // ops
    umq_timer_t *(*alloc)(umq_timer_pool_t *);
    int (*alloc_batch)(umq_timer_pool_t *, umq_timer_list_t *);
    void (*free)(umq_timer_pool_t *, umq_timer_t *);
    void (*free_batch)(umq_timer_pool_t *, umq_timer_list_t *);
    void (*destroy)(umq_timer_pool_t *);
};

int umq_timer_pool_init(const umq_timer_pool_cfg_t *config);
int umq_timer_pool_uninit(void);

// ops for non-thread-cache timer pool

/**
 * @brief Allocate one timer from head with thread-safety
 * @param[in] pool The timer pool
 * @return umq_timer_t*, or NULL if there is no space
 */
umq_timer_t *umq_timer_pool_alloc_op(umq_timer_pool_t *pool);

/**
 * @brief Allocate @c UMQ_TIMER_BATCH_COUNT timers from head with thread-safety
 * @param[in]  pool The timer pool
 * @param[out] list The out list containing allocated timers
 * @return The count of allocated timers. It can be less than @c UMQ_TIMER_BATCH_COUNT.
 */
int umq_timer_pool_alloc_batch_op(umq_timer_pool_t *pool, umq_timer_list_t *list);

/**
 * @brief Free the @p timer to the pool with thread-safety
 * @param[in] pool  The timer pool
 * @param[in] timer The timer will be freed
 */
void umq_timer_pool_free_op(umq_timer_pool_t *pool, umq_timer_t *timer);

/**
 * @brief Free all timers in @p list to the pool with thread-safety
 * @param[in] pool  The timer pool
 * @param[out] list The timer list will be freed
 */
void umq_timer_pool_free_batch_op(umq_timer_pool_t *pool, umq_timer_list_t *list);

/**
 * @brief Release all timers to OS with thread-safety
 * @param[in] pool The timer pool
 */
void umq_timer_pool_destroy_op(umq_timer_pool_t *pool);

// ops for thread-cache timer pool

/**
 * @brief Allocate one timers from head without thread-safety
 * @param[in] pool The timer pool
 * @return umq_timer_t*, or NULL if there is no space
 */
umq_timer_t *umq_timer_pool_cache_alloc_op(umq_timer_pool_t *pool);

/**
 * @brief Allocate one timers from head without thread-safety
 * @param[in]  pool The timer pool
 * @param[out] list The timer list contains one timer
 * @note Even if it's named alloc_batch, it always allocates one timer.
 */
int umq_timer_pool_cache_alloc_batch_op(umq_timer_pool_t *pool, umq_timer_list_t *list);

/**
 * @brief Free the @p timer to the pool
 * @param[in] pool  The timer pool
 * @param[in] timer The timer will be recycled
 */
void umq_timer_pool_cache_free_op(umq_timer_pool_t *pool, umq_timer_t *timer);

/**
 * @brief Free all timers in @p list to the pool
 * @param[in] pool The timer pool
 * @param[in] list The timer list will be recycled
 */
void umq_timer_pool_cache_free_batch_op(umq_timer_pool_t *pool, umq_timer_list_t *list);

/**
 * @brief Free all timers to parent timer pool
 * @param[in] pool
 */
void umq_timer_pool_cache_destroy_op(umq_timer_pool_t *pool);

umq_timer_pool_t *umq_timer_pool_thread_cache(void);
umq_timer_pool_t *umq_timer_pool_root(void);

#ifdef __cplusplus
}
#endif

#endif // UMQ_TIMER_H
