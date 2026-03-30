/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: util lock module
 * Create: 2026-03-30
 */

#ifndef UTIL_LOCK_H
#define UTIL_LOCK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* util_external_mutex_lock;
typedef void* util_external_rw_lock;

typedef struct util_external_mutex_lock_ops {
    util_external_mutex_lock (*create)(int attr);
    int (*destroy)(util_external_mutex_lock *m);

    int (*lock)(util_external_mutex_lock *m);
    int (*unlock)(util_external_mutex_lock *m);
    int (*trylock)(util_external_mutex_lock *m);
} util_external_mutex_lock_ops_t;

typedef struct util_external_rw_lock_ops {
    util_external_rw_lock (*create)(void);
    int (*destroy)(util_external_rw_lock *m);

    int (*read_lock)(util_external_rw_lock *m);
    int (*write_lock)(util_external_rw_lock *m);
    int (*unlock)(util_external_rw_lock *m);
    int (*try_read_lock)(util_external_rw_lock *m);
    int (*try_write_lock)(util_external_rw_lock *m);
} util_external_rw_lock_ops_t;

// if ops is NULL, reset global ops
void util_external_mutex_lock_ops_register(util_external_mutex_lock_ops_t *ops);
void util_external_rw_lock_ops_register(util_external_rw_lock_ops_t *ops);

// mutex api
util_external_mutex_lock *util_mutex_lock_create(int attr);
int util_mutex_lock_destroy(util_external_mutex_lock *m);
int util_mutex_lock(util_external_mutex_lock *m);
int util_mutex_unlock(util_external_mutex_lock *m);
int util_mutex_try_lock(util_external_mutex_lock *m);

// read-write mutex api
util_external_rw_lock *util_rwlock_create(int attr);
int util_rwlock_destroy(util_external_rw_lock *m);
int util_rwlock_rdlock(util_external_rw_lock *m);
int util_rwlock_wrlock(util_external_rw_lock *m);
int util_rwlock_unlock(util_external_rw_lock *m);
int util_rwlock_tryrdlock(util_external_rw_lock *m);
int util_rwlock_trywrlock(util_external_rw_lock *m);

#ifdef __cplusplus
}
#endif

#endif
