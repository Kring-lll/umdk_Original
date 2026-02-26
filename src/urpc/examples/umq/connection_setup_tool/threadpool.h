/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: umq example
 * Create: 2026-1-27
 * Note:
 * History: 2026-1-27
 */

#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct threadpool_task {
    void (*function)(void *);
    void *arg;
} threadpool_task_t;

typedef enum threadpool_status {
    THRADPOOL_STATUS_READY = 0,
    THRADPOOL_STATUS_SHUTDOWN,
} threadpool_status_t;

typedef struct task_queue {
    threadpool_task_t *queue;
    uint32_t queue_size;
    uint32_t queue_head;
    uint32_t queue_tail;
    uint32_t task_count;
} task_queue_t;

typedef struct threadpool{
    pthread_mutex_t lock;
    pthread_cond_t notify;
    pthread_t *threads;
    task_queue_t task_queue;
    int thread_count;
    int started;
    threadpool_status_t status;
} threadpool_t;

threadpool_t *threadpool_create(int thread_count, int queue_size);
int threadpool_add(threadpool_t *pool, void (*function)(void *), void *arg, uint32_t arg_len);
int threadpool_destroy(threadpool_t *pool);
int threadpool_free(threadpool_t *pool);

#ifdef __cplusplus
}
#endif

#endif

