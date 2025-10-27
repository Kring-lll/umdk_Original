/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2025. All rights reserved.
 * Description: uvs private api
 * Author: Li Hai
 * Create: 2024-2-18
 * Note:
 * History:
 */

#include <pthread.h>

#include "tpsa_log.h"

static pthread_mutex_t g_uvs_ops_lock;
static pthread_rwlock_t g_uvs_api_rwlock = PTHREAD_RWLOCK_INITIALIZER;

int uvs_ops_lock_init(void)
{
    if (pthread_mutex_init(&g_uvs_ops_lock, NULL) != 0) {
        return -1;
    }
    return 0;
}

void uvs_ops_lock_uninit(void)
{
    (void)pthread_mutex_destroy(&g_uvs_ops_lock);
}

int uvs_ops_mutex_lock(void)
{
    int ret;

    ret = pthread_mutex_lock(&g_uvs_ops_lock);
    if (ret != 0) {
        TPSA_LOG_ERR("pthread_mutex_lock g_uvs_ops_lock error, ret: %d\n", ret);
        return ret;
    }
    return 0;
}

int uvs_ops_mutex_unlock(void)
{
    int ret;

    ret = pthread_mutex_unlock(&g_uvs_ops_lock);
    if (ret != 0) {
        TPSA_LOG_ERR("pthread_mutex_unlock g_uvs_ops_lock error, ret: %d\n", ret);
        return ret;
    }
    return 0;
}

void uvs_get_api_rdlock(void)
{
    pthread_rwlock_rdlock(&g_uvs_api_rwlock);
}

void uvs_get_api_wrlock(void)
{
    pthread_rwlock_wrlock(&g_uvs_api_rwlock);
}

void put_uvs_lock(void)
{
    pthread_rwlock_unlock(&g_uvs_api_rwlock);
}