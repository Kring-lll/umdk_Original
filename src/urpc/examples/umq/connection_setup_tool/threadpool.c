/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: umq example
 * Create: 2026-1-27
 * Note:
 * History: 2026-1-27
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "threadpool.h"

#define MAX_THREAD_NUM 100

static void *threadpool_worker(void *threadpool)
{
    threadpool_t *pool = (threadpool_t *)threadpool;
    threadpool_task_t task;

    for (;;) {
        pthread_mutex_lock(&(pool->lock));

        while ((pool->task_queue.task_count == 0) && (pool->status == THRADPOOL_STATUS_READY)) {
            pthread_cond_wait(&(pool->notify), &(pool->lock));
        }

        if (pool->status == THRADPOOL_STATUS_SHUTDOWN) {
            break;
        }

        task.function = pool->task_queue.queue[pool->task_queue.queue_head].function;
        task.arg = pool->task_queue.queue[pool->task_queue.queue_head].arg;
        pool->task_queue.queue_head = (pool->task_queue.queue_head + 1) % pool->task_queue.queue_size;
        pool->task_queue.task_count--;
        pthread_mutex_unlock(&(pool->lock));
        (*(task.function))(task.arg);

        free(task.arg);
    }
    
    pool->started--;
    pthread_mutex_unlock(&(pool->lock));
    return NULL;
}

threadpool_t *threadpool_create(int thread_count, int queue_size)
{
    threadpool_t *pool = NULL;
    int i;
    
    if (thread_count <= 0 || thread_count > MAX_THREAD_NUM || queue_size <= 0) {
        return NULL;
    }

    if ((pool = (threadpool_t *)malloc(sizeof(threadpool_t))) == NULL) {
        goto err;
    }

    pool->thread_count = 0;
    pool->task_queue.queue_size = queue_size;
    pool->task_queue.queue_head = 0;
    pool->task_queue.queue_tail = 0;
    pool->task_queue.task_count = 0;
    pool->started = 0;

    pool->threads = (pthread_t *)malloc(sizeof(pthread_t) * thread_count);
    if (pool->threads == NULL) {
        goto err;
    }

    pool->task_queue.queue = (threadpool_task_t *)malloc(sizeof(threadpool_task_t) * queue_size);
    if (pool->task_queue.queue == NULL) {
        goto err;
    }

    if ((pthread_mutex_init(&(pool->lock), NULL) != 0) ||
        (pthread_cond_init(&(pool->notify), NULL) != 0)) {
        goto err;
    }
    
    for (i = 0; i < thread_count; i++) {
        if (pthread_create(&(pool->threads[i]), NULL, threadpool_worker, (void *)pool) != 0) {
            goto err;
        }
        pool->started++;
    }
    pool->thread_count = thread_count;
    pool->status = THRADPOOL_STATUS_READY;
    return pool;

err:
    if (pool) {
        threadpool_free(pool);
        pool = NULL;
    }
    return pool;
}

int threadpool_add(threadpool_t *pool, void (*function)(void *), void *arg, uint32_t arg_len)
{
    int err = 0;
    uint32_t next;
    
    void *work_arg = malloc(arg_len);
    if (work_arg == NULL) {
        return -1;
    }

    (void)memcpy(work_arg, arg, arg_len);
    if (pool == NULL || function == NULL) {
        goto FREE_ARG;
    }

    pthread_mutex_lock(&(pool->lock));
    next = (pool->task_queue.queue_tail + 1) % pool->task_queue.queue_size;
    if (pool->task_queue.task_count == pool->task_queue.queue_size || pool->status != THRADPOOL_STATUS_READY) {
        pthread_mutex_unlock(&pool->lock);
        goto FREE_ARG;
    } else {
        pool->task_queue.queue[pool->task_queue.queue_tail].function = function;
        pool->task_queue.queue[pool->task_queue.queue_tail].arg = work_arg;
        pool->task_queue.queue_tail = next;
        pool->task_queue.task_count += 1;
        err = pthread_cond_signal(&(pool->notify));
    }
    pthread_mutex_unlock(&(pool->lock));
    return err;

FREE_ARG:
    free(work_arg);
    return -1;
}

int threadpool_destroy(threadpool_t *pool)
{
    int i, err = 0;
    if (pool == NULL) {
        return -1;
    }

    pthread_mutex_lock(&(pool->lock));
    if (pool->status != THRADPOOL_STATUS_READY) {
        err = -1;
    } else {
        pool->status = THRADPOOL_STATUS_SHUTDOWN;
        if ((pthread_cond_broadcast(&(pool->notify)) != 0)) {
            err = -1;
        }

        for (i = 0; i < pool->thread_count; i++) {
            if (pthread_join(pool->threads[i], NULL) != 0) {
                err = -1;
            }
        }
    }
    pthread_mutex_unlock(&(pool->lock));

    if (err == 0) {
        threadpool_free(pool);
    }
    return err;
}

int threadpool_free(threadpool_t *pool)
{
    if (pool == NULL || pool->started > 0) {
        return -1;
    }
    
    if (pool->threads) {
        free(pool->threads);
        pool->threads = NULL;
        free(pool->task_queue.queue);
        pool->task_queue.queue = NULL;
        pthread_mutex_destroy(&(pool->lock));
        pthread_cond_destroy(&(pool->notify));
    }
    free(pool);
    return 0;
}
