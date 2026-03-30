/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: util lock module
 * Create: 2026-03-30
 */

#include <pthread.h>

#include "util_lock.h"

static util_external_mutex_lock *util_mutex_lock_create_impl(util_externel_mutex_attr_t type)
{
    util_external_mutex_lock *mutex_lock = (util_external_mutex_lock)calloc(sizeof(util_external_mutex_lock));
    if (mutex_lock == NULL) {
        return NULL;
    }

    pthread_mutexattr_t attr;
    int ret = pthread_mutexattr_init(&attr);
    if (ret != 0) {
        goto FREE_LOCK;
    }
    if (type == UTIL_MUTEX_ATTR_RECURSIVE) {
        ret = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        if (ret != 0) {
            pthread_mutexattr_destroy(&attr);
            goto FREE_LOCK;
        }
    }
 
    ret = pthread_mutex_init(mutex_lock, &attr);
    pthread_mutexattr_destroy(&attr);
    if (ret != 0) {
        goto FREE_LOCK;
    }
    return mutex_lock;

FREE_LOCK:
    free(mutex_lock);
    return NULL;
}

static int util_mutex_lock_destroy_impl(util_external_mutex_lock *lock)
{
    if (lock == NULL) {
        return -1;
    }
    pthread_mutex_destroy(lock);
    free(lock);
    return 0;
}

static int util_mutex_lock_impl(util_external_mutex_lock *lock)
{
    if (lock == NULL) {
        return -1;
    }
    pthread_mutex_lock(lock);
    return 0;
}

static int util_mutex_unlock_impl(util_external_mutex_lock *lock)
{
    if (lock == NULL) {
        return -1;
    }
    pthread_mutex_unlock(lock);
    return 0;
}

static int util_mutex_try_lock_impl(util_external_mutex_lock *lock)
{
    if (lock == NULL) {
        return -1;
    }
    pthread_mutex_trylock(lock);
    return 0;
}

static util_external_rw_lock *util_rwlock_create_impl(int attr)
{
    util_external_rw_lock *rw_lock = (util_external_rw_lock)calloc(sizeof(util_external_rw_lock));
    if (rw_lock == NULL) {
        return NULL;
    }

    int ret = pthread_rwlock_init(rw_lock, NULL);
    if (ret != 0) {
        goto FREE_LOCK;
    }
    return rw_lock;

FREE_LOCK:
    free(rw_lock);
    return NULL;
}
static int util_rwlock_destroy_impl(util_external_rw_lock *lock){
    if (lock == NULL) {
        return -1;
    }
    pthread_rwlock_destroy(lock);
    free(lock);
    return 0;
}
static int util_rwlock_rdlock_impl(util_external_rw_lock *lock)
{
    if (lock == NULL) {
        return -1;
    }
    pthread_rwlock_rdlock(lock);
    return 0;
}
static int util_rwlock_wrlock_impl(util_external_rw_lock *lock)
{
    if (lock == NULL) {
        return -1;
    }
    pthread_rwlock_wrlock(lock);
    return 0;
}
static int util_rwlock_unlock_impl(util_external_rw_lock *lock)
{
    if (lock == NULL) {
        return -1;
    }
    pthread_rwlock_unlock(lock);
    return 0;
}
static int util_rwlock_tryrdlock_impl(util_external_rw_lock *lock)
{
    if (lock == NULL) {
        return -1;
    }
    pthread_rwlock_tryrdlock(lock);
    return 0;
}
static int util_rwlock_trywrlock_impl(util_external_rw_lock *lock)
{
    if (lock == NULL) {
        return -1;
    }
    pthread_rwlock_trywrlock(lock);
    return 0;
}

static util_external_mutex_lock_ops_t g_util_default_mutex_lock_ops = {
    .create = util_mutex_lock_create_impl,
    .destroy = util_mutex_lock_destroy_impl,

    .lock = util_mutex_lock_impl,
    .unlock = util_mutex_unlock_impl,
    .trylock = util_mutex_try_lock_impl,
};

static util_external_rw_lock_ops_t g_util_default_rw_lock_ops = {
    .create = util_rwlock_create_impl,
    .destroy = util_rwlock_destroy_impl,

    .read_lock = util_rwlock_rdlock_impl,
    .write_lock = util_rwlock_wrlock_impl,
    .unlock = util_rwlock_unlock_impl,
    .try_read_lock = util_rwlock_tryrdlock_impl,
    .try_write_lock = util_rwlock_trywrlock_impl,
};

static util_external_mutex_lock_ops_t g_util_mutex_lock_ops = g_util_default_mutex_lock_ops;
static util_external_rw_lock_ops_t g_util_rw_lock_ops = g_util_default_rw_lock_ops;

// if ops is NULL, reset global ops
void util_external_mutex_lock_ops_register(util_external_mutex_lock_ops_t *ops)
{
    if (ops == NULL) {
        g_util_mutex_lock_ops = g_util_default_mutex_lock_ops;
        return;
    }

    g_util_mutex_lock_ops = *ops;
}

void util_external_rw_lock_ops_register(util_external_rw_lock_ops_t *ops)
{
    if (ops == NULL) {
        g_util_mutex_lock_ops = g_util_default_rw_lock_ops;
        return;
    }

    g_util_rw_lock_ops = *ops;
}

// mutex api
util_external_mutex_lock *util_mutex_lock_create(int attr)
{
    return g_util_mutex_lock_ops.create(attr);
}

int util_mutex_lock_destroy(util_external_mutex_lock *lock)
{
    return g_util_mutex_lock_ops.destroy(lock);
}

int util_mutex_lock(util_external_mutex_lock *lock)
{
    g_util_mutex_lock_ops.lock(lock);
}

int util_mutex_unlock(util_external_mutex_lock *lock)
{
    g_util_mutex_lock_ops.unlock(lock);
}

int util_mutex_try_lock(util_external_mutex_lock *lock)
{
    return g_util_mutex_lock_ops.trylock(lock);
}

// read-write mutex api
util_external_rw_lock *util_rwlock_create(int attr)
{
    return g_util_rw_lock_ops.create(attr);
}

int util_rwlock_destroy(util_external_rw_lock *lock)
{
    return g_util_rw_lock_ops.destroy(lock);
}

int util_rwlock_rdlock(util_external_rw_lock *lock)
{
    g_util_rw_lock_ops.read_lock(lock);
}

int util_rwlock_wrlock(util_external_rw_lock *lock)
{
    g_util_rw_lock_ops.write_lock(lock);
}

int util_rwlock_unlock(util_external_rw_lock *lock)
{
    g_util_rw_lock_ops.unlock(lock);
}

int util_rwlock_tryrdlock(util_external_rw_lock *lock)
{
    return g_util_rw_lock_ops.try_read_lock(lock);
}

int util_rwlock_trywrlock(util_external_rw_lock *lock)
{
    return g_util_rw_lock_ops.try_write_lock(lock);
}   
