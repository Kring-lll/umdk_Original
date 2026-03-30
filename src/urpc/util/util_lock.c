/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: util lock module
 * Create: 2026-03-30
 */

#include <pthread.h>

#include "util_lock.h"

// UTIL_LOG_INFO("timing wheel init success\n");

static util_external_mutex_lock *util_mutex_lock_create_impl(int attr);
static int util_mutex_lock_destroy_impl(util_external_mutex_lock *m);
static int util_mutex_lock_impl(util_external_mutex_lock *m);
static int util_mutex_unlock_impl(util_external_mutex_lock *m);
static int util_mutex_try_lock_impl(util_external_mutex_lock *m);

static util_external_rw_lock *util_rwlock_create_impl(int attr);
static int util_rwlock_destroy_impl(util_external_rw_lock *m);
static int util_rwlock_rdlock_impl(util_external_rw_lock *m);
static int util_rwlock_wrlock_impl(util_external_rw_lock *m);
static int util_rwlock_unlock_impl(util_external_rw_lock *m);
static int util_rwlock_tryrdlock_impl(util_external_rw_lock *m);
static int util_rwlock_trywrlock_impl(util_external_rw_lock *m);

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

int util_mutex_lock_destroy(util_external_mutex_lock *m)
{
    return g_util_mutex_lock_ops.destroy(m);
}

int util_mutex_lock(util_external_mutex_lock *m)
{
    g_util_mutex_lock_ops.lock(m);
}

int util_mutex_unlock(util_external_mutex_lock *m)
{
    g_util_mutex_lock_ops.unlock(m);
}

int util_mutex_try_lock(util_external_mutex_lock *m)
{
    return g_util_mutex_lock_ops.trylock(m);
}

// read-write mutex api
util_external_rw_lock *util_rwlock_create(int attr)
{
    return g_util_rw_lock_ops.create(attr);
}

int util_rwlock_destroy(util_external_rw_lock *m)
{
    return g_util_rw_lock_ops.destroy(m);
}

int util_rwlock_rdlock(util_external_rw_lock *m)
{
    g_util_rw_lock_ops.read_lock(m);
}

int util_rwlock_wrlock(util_external_rw_lock *m)
{
    g_util_rw_lock_ops.write_lock(m);
}

int util_rwlock_unlock(util_external_rw_lock *m)
{
    g_util_rw_lock_ops.unlock(m);
}

int util_rwlock_tryrdlock(util_external_rw_lock *m)
{
    return g_util_rw_lock_ops.try_read_lock(m);
}

int util_rwlock_trywrlock(util_external_rw_lock *m)
{
    return g_util_rw_lock_ops.try_write_lock(m);
}   
