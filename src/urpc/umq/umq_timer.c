/**
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: umq timer
 * Create: 2025-11-03
 */

#include "umq_timer.h"

#include <stdlib.h>
#include <string.h>

#include "urpc_util.h"
#include "urpc_thread_closure.h"
#include "umq_errno.h"

static umq_timer_pool_t g_umq_timer_pool_root = {
    .inited = false,

    .parent = NULL,
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .count = 0,

    // ops
    .alloc = umq_timer_pool_alloc_op,
    .alloc_batch = umq_timer_pool_alloc_batch_op,
    .free = umq_timer_pool_free_op,
    .free_batch = umq_timer_pool_free_batch_op,
    .destroy = umq_timer_pool_destroy_op,
};

static __thread umq_timer_pool_t g_umq_timer_pool_thread_cache = {
    .inited = false,

    .parent = &g_umq_timer_pool_root,
    .mutex = PTHREAD_MUTEX_INITIALIZER, // The mutex is unused as we are thread-local
    .count = 0,

    // ops
    .alloc = umq_timer_pool_cache_alloc_op,
    .alloc_batch = umq_timer_pool_cache_alloc_batch_op,
    .free = umq_timer_pool_cache_free_op,
    .free_batch = umq_timer_pool_cache_free_batch_op,
    .destroy = umq_timer_pool_cache_destroy_op,
};

umq_timer_t *umq_timer_create(umq_timer_pool_t *pool, void (*func)(void *), void *arg, size_t sz, uint64_t timeout)
{
    if (URPC_UNLIKELY(sz > UMQ_TIMER_PAYLOAD_SIZE)) {
        return NULL;
    }

    umq_timer_t *timer = pool->alloc(pool);
    if (URPC_UNLIKELY(timer == NULL)) {
        return NULL;
    }

    // Users should guarantee sz is not oversize.
    // See umq_timer_pool_cfg_t
    (void)memcpy(timer + 1, arg, sz);

    urpc_list_init(&timer->node);
    timer->pool = pool;
    timer->id = (uint64_t)(uintptr_t)timer;
    timer->expiration = get_timestamp_ms() + timeout;
    timer->func = func;
    timer->arg = (void *)(timer + 1);
    return timer;
}

umq_timer_t *umq_timer_create_with_id(umq_timer_pool_t *pool, void (*func)(void *), void *arg, size_t sz,
                                      uint64_t timeout, uint64_t id)
{
    umq_timer_t *timer = umq_timer_create(pool, func, arg, sz, timeout);
    if (URPC_UNLIKELY(timer == NULL)) {
        return NULL;
    }

    timer->id = id;
    return timer;
}

void umq_timer_destroy(umq_timer_t *timer)
{
    if (URPC_LIKELY(timer != NULL)) {
        umq_timer_pool_t *pool = timer->pool;
        pool->free(pool, timer);
    }
}

bool umq_timer_expired(const umq_timer_t *timer, uint64_t now)
{
    if (URPC_LIKELY(timer != NULL)) {
        return now >= timer->expiration;
    }
    return false;
}

void umq_timer_list_init(umq_timer_list_t *list)
{
    urpc_list_init(&list->head);
}

void umq_timer_list_uninit(umq_timer_list_t *list)
{
    urpc_list_init(&list->head);
}

umq_timer_t *umq_timer_list_top(umq_timer_list_t *list)
{
    umq_timer_t *timer;
    URPC_LIST_FIRST_NODE(timer, node, &list->head);
    return timer;
}

void umq_timer_list_pop(umq_timer_list_t *list)
{
    umq_timer_t *timer;
    URPC_LIST_FIRST_NODE(timer, node, &list->head);
    urpc_list_remove(&timer->node);
}

void umq_timer_list_add(umq_timer_list_t *list, umq_timer_t *timer)
{
    const uint64_t expiration = timer->expiration;

    umq_timer_t *it;
    URPC_LIST_FOR_EACH(it, node, &list->head) {
        if (expiration < it->expiration) {
            urpc_list_insert_before(&it->node, &timer->node);
            return;
        }
    }

    urpc_list_insert_before(&it->node, &timer->node);
}

umq_timer_t *umq_timer_list_remove_by_id(umq_timer_list_t *list, uint64_t id)
{
    umq_timer_t *it;
    umq_timer_t *next;
    URPC_LIST_FOR_EACH_SAFE(it, next, node, &list->head) {
        if (it->id == id) {
            urpc_list_remove(&it->node);
            return it;
        }
    }
    return NULL;
}

void umq_timer_list_get_expired(umq_timer_list_t *list, umq_timer_list_t *out, uint64_t now)
{
    umq_timer_t *it;
    umq_timer_t *next;
    URPC_LIST_FOR_EACH_SAFE(it, next, node, &list->head) {
        if (!umq_timer_expired(it, now)) {
            break;
        }

        urpc_list_remove(&it->node);
        urpc_list_push_back(&out->head, &it->node);
    }
}

int umq_timer_list_do_expire(umq_timer_list_t *list)
{
    int cnt = 0;
    umq_timer_t *it;
    umq_timer_t *next;
    URPC_LIST_FOR_EACH_SAFE(it, next, node, &list->head) {
        ++cnt;
        umq_timer_invoke(it);
        umq_timer_destroy(it);
    }

    return cnt;
}

int umq_timer_pool_init(const umq_timer_pool_cfg_t *config)
{
    umq_timer_pool_t *root = umq_timer_pool_root();

    root->inited = true;
    root->cfg = *config;
    root->parent = NULL;
    umq_timer_list_init(&root->freelist);
    pthread_mutex_init(&root->mutex, NULL);
    root->count = config->num_block;

    for (uint64_t i = 0; i < config->num_block; ++i) {
        umq_timer_t *timer = (umq_timer_t *)((char *)config->buf_addr + i * config->block_size);
        umq_timer_list_push_back(&root->freelist, timer);
    }

    return UMQ_SUCCESS;
}

int umq_timer_pool_uninit(void)
{
    umq_timer_pool_t *root = umq_timer_pool_root();

    root->inited = false;
    umq_timer_list_uninit(&root->freelist);
    pthread_mutex_destroy(&root->mutex);
    root->count = 0;

    free(root->cfg.buf_addr);

    return UMQ_SUCCESS;
}

static void umq_timer_release_thread_cache(uint64_t id)
{
    umq_timer_pool_t *pool = &g_umq_timer_pool_thread_cache;
    pool->destroy(pool);
}

umq_timer_pool_t *umq_timer_pool_thread_cache()
{
    umq_timer_pool_t *pool = &g_umq_timer_pool_thread_cache;
    if (URPC_UNLIKELY(!pool->inited)) {
        umq_timer_list_init(&pool->freelist);
        pool->count = 0;
        pool->inited = true;
        urpc_thread_closure_register(THREAD_CLOSURE_UMQ_TIMER_POOL, 0, umq_timer_release_thread_cache);
    }
    return pool;
}

umq_timer_pool_t *umq_timer_pool_root()
{
    return &g_umq_timer_pool_root;
}

umq_timer_t *umq_timer_pool_alloc_op(umq_timer_pool_t *pool)
{
    if (URPC_UNLIKELY(!pool->inited)) {
        return NULL;
    }

    umq_timer_t *timer = NULL;
    URPC_WITH_LOCK(pool->mutex) {
        if (pool->count > 0) {
            timer = umq_timer_list_top(&pool->freelist);
            umq_timer_list_pop(&pool->freelist);
            --pool->count;
        }
    }
    return timer;
}

int umq_timer_pool_alloc_batch_op(umq_timer_pool_t *pool, umq_timer_list_t *list)
{
    if (URPC_UNLIKELY(!pool->inited)) {
        return 0;
    }

    size_t cnt = 0;
    URPC_WITH_LOCK(pool->mutex) {
        cnt = MIN(pool->count, UMQ_TIMER_BATCH_COUNT);
        for (size_t i = 0; i < cnt; ++i) {
            umq_timer_list_push_back(list, umq_timer_list_top(&pool->freelist));
        }

        pool->count -= cnt;
    }
    return cnt;
}

void umq_timer_pool_free_op(umq_timer_pool_t *pool, umq_timer_t *timer)
{
    if (URPC_UNLIKELY(!pool->inited)) {
        return;
    }

    URPC_WITH_LOCK(pool->mutex) {
        umq_timer_list_push_back(&pool->freelist, timer);
    }
}

void umq_timer_pool_free_batch_op(umq_timer_pool_t *pool, umq_timer_list_t *list)
{
    if (URPC_UNLIKELY(!pool->inited)) {
        return;
    }

    URPC_WITH_LOCK(pool->mutex) {
        urpc_list_splice_tail(&list->head, &pool->freelist.head);
    }
}

void umq_timer_pool_destroy_op(umq_timer_pool_t *pool)
{
    if (URPC_UNLIKELY(!pool->inited)) {
        return;
    }

    URPC_WITH_LOCK(pool->mutex) {
        umq_timer_pool_t *parent = pool->parent;
        if (parent == NULL) {
            // The memory owner
            free(pool->cfg.buf_addr);
            umq_timer_list_init(&pool->freelist);
        } else {
            parent->free_batch(parent, &pool->freelist);
        }

        pool->count = 0;
        pool->inited = false;
    }
}

umq_timer_t *umq_timer_pool_cache_alloc_op(umq_timer_pool_t *pool)
{
    if (URPC_UNLIKELY(!pool->inited)) {
        return NULL;
    }

    // If there is no timer, beg some timers from parent.
    if (pool->count == 0) {
        umq_timer_pool_t *parent = pool->parent;
        if (URPC_UNLIKELY(parent == NULL)) {
            return NULL;
        }

        umq_timer_list_t list2;
        umq_timer_list_init(&list2);

        int n = parent->alloc_batch(parent, &list2);
        if (URPC_UNLIKELY(n == 0)) {
            return NULL;
        }

        urpc_list_splice(&list2.head, &pool->freelist.head);
        pool->count += n;
    }

    umq_timer_t *timer = umq_timer_list_top(&pool->freelist);
    umq_timer_list_pop(&pool->freelist);
    --pool->count;
    return timer;
}


int umq_timer_pool_cache_alloc_batch_op(umq_timer_pool_t *pool, umq_timer_list_t *list)
{
    if (URPC_UNLIKELY(!pool->inited)) {
        return 0;
    }

    umq_timer_t *timer = umq_timer_pool_cache_alloc_op(pool);
    if (URPC_UNLIKELY(timer == NULL)) {
        return 0;
    }

    umq_timer_list_push_back(list, timer);
    return 1;
}

void umq_timer_pool_cache_free_op(umq_timer_pool_t *pool, umq_timer_t *timer)
{
    if (URPC_UNLIKELY(!pool->inited)) {
        return;
    }

    umq_timer_list_push_back(&pool->freelist, timer);
    ++pool->count;
}

void umq_timer_pool_cache_free_batch_op(umq_timer_pool_t *pool, umq_timer_list_t *list)
{
    if (URPC_UNLIKELY(!pool->inited)) {
        return;
    }

    umq_timer_t *it;
    umq_timer_t *next;
    URPC_LIST_FOR_EACH_SAFE(it, next, node, &list->head) {
        umq_timer_list_push_back(&pool->freelist, it);
        ++pool->count;
    }
}

void umq_timer_pool_cache_destroy_op(umq_timer_pool_t *pool)
{
    if (URPC_UNLIKELY(!pool->inited)) {
        return;
    }

    umq_timer_pool_t *parent = pool->parent;
    if (URPC_UNLIKELY(parent == NULL)) {
        return;
    }

    parent->free_batch(parent, &pool->freelist);
    pool->count = 0;
    pool->inited = false;
}
