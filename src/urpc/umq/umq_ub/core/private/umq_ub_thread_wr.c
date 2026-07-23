/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: umq ub thread-local tx work-request pool.
 * Create: 2026-07-17
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "umq_errno.h"
#include "umq_vlog.h"
#include "util_thread_key.h"
#include "umq_ub_thread_wr.h"

typedef struct umq_ub_thread_wr_ctx {
    util_thread_key_t *key;
    bool inited;
} umq_ub_thread_wr_ctx_t;

static umq_ub_thread_wr_ctx_t g_umq_ub_thread_wr_ctx;

static void umq_thread_wr_destructor(void *data)
{
    (void)util_thread_setspecific(g_umq_ub_thread_wr_ctx.key, NULL);

    if (data == NULL) {
        return;
    }
    free(data);
}

int umq_ub_thread_wr_init(void)
{
    if (g_umq_ub_thread_wr_ctx.inited) {
        UMQ_VLOG_ERR(VLOG_UMQ, "umq ub thread wr already initialized\n");
        return -UMQ_ERR_EEXIST;
    }

    memset(&g_umq_ub_thread_wr_ctx, 0, sizeof(g_umq_ub_thread_wr_ctx));
    g_umq_ub_thread_wr_ctx.key = util_thread_key_create(umq_thread_wr_destructor);
    if (g_umq_ub_thread_wr_ctx.key == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "umq thread wr key create failed\n");
        return -UMQ_ERR_ENOMEM;
    }

    g_umq_ub_thread_wr_ctx.inited = true;
    return UMQ_SUCCESS;
}

void umq_ub_thread_wr_uninit(void)
{
    if (!g_umq_ub_thread_wr_ctx.inited) {
        return;
    }

    umq_thread_wr_destructor((void *)umq_ub_thread_wr_get());
    g_umq_ub_thread_wr_ctx.inited = false;
    if (g_umq_ub_thread_wr_ctx.key != NULL) {
        (void)util_thread_key_delete(g_umq_ub_thread_wr_ctx.key);
        g_umq_ub_thread_wr_ctx.key = NULL;
    }
}

umq_ub_thread_wr_t *umq_ub_thread_wr_get(void)
{
    if (!g_umq_ub_thread_wr_ctx.inited) {
        return NULL;
    }

    umq_ub_thread_wr_t *wr = util_thread_getspecific(g_umq_ub_thread_wr_ctx.key);
    if (wr != NULL) {
        return wr;
    }

    wr = (umq_ub_thread_wr_t *)calloc(1, sizeof(umq_ub_thread_wr_t));
    if (wr == NULL) {
        UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "umq ub thread wr alloc failed\n");
        return NULL;
    }

    if (util_thread_setspecific(g_umq_ub_thread_wr_ctx.key, (void *)wr) != 0) {
        UMQ_VLOG_ERR(VLOG_UMQ, "umq thread wr setspecific failed\n");
        free(wr);
        return NULL;
    }
    return wr;
}
