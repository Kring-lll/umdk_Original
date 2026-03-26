/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: realize qbuf pool function
 * Create: 2025-7-26
 * Note:
 * History: 2025-7-26
 */

#include <malloc.h>

#include "umq_errno.h"
#include "umq_vlog.h"
#include "umq_qbuf_pool.h"

#include "urpc_thread_closure.h"

#define QBUF_POOL_TLS_MAX (2048)     // max count of thread local buffer storage
#define QBUF_POOL_BATCH_CNT (512)    // batch size when fetch from global or return to global

#define QBUF_POOL_DEFAULT_EXPANSION_COUNT 8192
#define QBUF_POOL_SEG_ARRAY_INIT_CAP 4

typedef struct qbuf_expansion_pool {
    uint32_t dynamic_mempool_id;
    void *data_buffer;
    void *header_buffer;
    void *ext_header_buffer;
    uint64_t seg_size;

    uint64_t total_blk_with_data;
    uint64_t free_blk_with_data;
    umq_buf_list_t free_list_with_data;

    uint64_t total_blk_without_data;
    uint64_t free_blk_without_data;
    umq_buf_list_t free_list_without_data;
} qbuf_expansion_pool_t;

typedef struct local_qbuf_pool {
    bool inited;
    local_block_pool_t block_pool;
} local_qbuf_pool_t;

typedef struct qbuf_pool {
    bool inited;
    void *data_buffer;          // 数据区起始地址，COMBINE模式为所有的数据起始位置，SPLIT模式为所有的数据起始位置+头部区大小，需要8K对齐
    void *header_buffer;        // 头部区起始地址，COMBINE模式为NULL，SPLIT模式为所有数据的起始位置
    void *ext_header_buffer;    // ext头部区起始地址，数据区指针为空，仅有头部，数量为分片数*16。combine模式为NULL
    uint64_t total_size;        // 内存池管理的内存总大小

    uint32_t block_size;        // headroom size + data size以8K为大小向上取整，如果是combine模式还包括umq_qbuf_t结构体大小
    uint32_t headroom_size;     // 预留的头部空间大小
    uint32_t data_size;

    uint64_t total_block_num;
    umq_buf_mode_t mode;

    global_block_pool_t block_pool;

    qbuf_expansion_pool_t **expansion_pool;
    urpc_id_generator_t dynamic_id_gen; // number of blocks per expansion
    register_seg_callback_t register_seg_func;
    unregister_seg_callback_t unregister_seg_func;
    uint32_t expansion_pool_list_capacity;
    uint32_t expansion_pool_id_min;
    uint32_t expansion_pool_cnt_max;
    uint32_t expansion_count;
    uint32_t expansion_block_count;
} qbuf_pool_t;

static qbuf_pool_t g_qbuf_pool = {0};
static __thread local_qbuf_pool_t g_thread_cache = {0};
static uint8_t g_umq_qbuf_size_pow_samll = UMQ_QBUF_SIZE_POW_8K;

static void *g_buffer_addr = NULL;
static uint64_t g_total_len = 0;

static void free_expansion_pool_slot(qbuf_expansion_pool_t *slot)
{
    uint32_t id = slot->dynamic_mempool_id;
    g_qbuf_pool.expansion_pool[id] = NULL;
    g_qbuf_pool.expansion_count--;
    urpc_id_generator_free(&g_qbuf_pool.dynamic_id_gen, id);

    if (slot->data_buffer != NULL) {
        free(slot->data_buffer);
    }
    free(slot);
}

static ALWAYS_INLINE void return_batch_to_expansion_pool(qbuf_expansion_pool_t *exp_pool, umq_buf_t *batch_head,
        umq_buf_t *batch_tail, uint32_t batch_cnt, uint64_t *local_buf_cnt)
{
    if (batch_head->buf_data != NULL) {
        QBUF_LIST_NEXT(batch_tail) = QBUF_LIST_FIRST(&exp_pool->free_list_with_data);
        QBUF_LIST_FIRST(&exp_pool->free_list_with_data) = batch_head;
        exp_pool->free_blk_with_data += batch_cnt;
    } else {
        QBUF_LIST_NEXT(batch_tail) = QBUF_LIST_FIRST(&exp_pool->free_list_without_data);
        QBUF_LIST_FIRST(&exp_pool->free_list_without_data) = batch_head;
        exp_pool->free_blk_without_data += batch_cnt;
    }
    *local_buf_cnt -= batch_cnt;

    if (exp_pool->free_blk_with_data == exp_pool->total_blk_with_data && (exp_pool->total_blk_without_data == 0 ||
        exp_pool->free_blk_without_data == exp_pool->total_blk_without_data)) {
        if (g_qbuf_pool.unregister_seg_func != NULL) {
            uint16_t mempool_id = (uint16_t)(exp_pool->dynamic_mempool_id + g_qbuf_pool.expansion_pool_id_min);
            g_qbuf_pool.unregister_seg_func(NULL, mempool_id);
        }
        free_expansion_pool_slot(exp_pool);
    }
}

static ALWAYS_INLINE void flush_batch_to_pool( umq_buf_list_t *global_head, uint64_t *global_buf_cnt,
    umq_buf_t *batch_head, umq_buf_t *batch_tail, uint32_t batch_cnt, uint32_t batch_exp_id, uint64_t *local_buf_cnt)
{
    if (batch_exp_id != UINT32_MAX) {
        qbuf_expansion_pool_t *exp_pool = g_qbuf_pool.expansion_pool[batch_exp_id];
        if (exp_pool != NULL) {
            return_batch_to_expansion_pool(exp_pool, batch_head, batch_tail, batch_cnt, local_buf_cnt);
        }
    } else {
        return_batch_to_global_pool(global_head, global_buf_cnt, batch_head, batch_tail, batch_cnt, local_buf_cnt);
    }
}

void return_list_to_pools(umq_buf_t *local_head, uint64_t *local_buf_cnt,
    umq_buf_list_t *global_head, uint64_t *global_buf_cnt)
{
    umq_buf_t *cur = local_head;
    umq_buf_t *batch_head = NULL;
    umq_buf_t *batch_tail = NULL;
    uint32_t batch_cnt = 0;
    uint32_t batch_exp_id = UINT32_MAX;

    while (cur != NULL) {
        umq_buf_t *next = QBUF_LIST_NEXT(cur);
        uint32_t node_exp_id = (cur->mempool_id >= g_qbuf_pool.expansion_pool_id_min) ?
            (cur->mempool_id - g_qbuf_pool.expansion_pool_id_min) : UINT32_MAX;
        if (batch_head == NULL) {
            batch_head = cur;
            batch_tail = cur;
            batch_cnt = 1;
            batch_exp_id = node_exp_id;
        } else if (batch_exp_id == node_exp_id) {
            batch_tail = cur;
            batch_cnt++;
        } else {
            QBUF_LIST_NEXT(batch_tail) = NULL;
            flush_batch_to_pool(global_head, global_buf_cnt, batch_head,
                batch_tail, batch_cnt, batch_exp_id, local_buf_cnt);
            batch_head = cur;
            batch_tail = cur;
            batch_cnt = 1;
            batch_exp_id = node_exp_id;
        }
        cur = next;
    }

    if (batch_head != NULL) {
        QBUF_LIST_NEXT(batch_tail) = NULL;
        flush_batch_to_pool(global_head, global_buf_cnt, batch_head,
            batch_tail, batch_cnt, batch_exp_id, local_buf_cnt);
    }
}

void *umq_io_buf_malloc(umq_buf_mode_t buf_mode, uint64_t size)
{
    if (g_buffer_addr != NULL) {
        return g_buffer_addr;
    }

    uint64_t min_size = umq_buf_size_small();
    if (buf_mode == UMQ_BUF_SPLIT) {
        min_size = (UMQ_EMPTY_HEADER_COEFFICIENT + 1) * (uint32_t)sizeof(umq_buf_t) + umq_buf_size_small();
    }

    if (size > 0) {
        if (size < min_size) {
            UMQ_VLOG_ERR(VLOG_UMQ, "memory size %lu invalid, expect at least %lu\n", size, min_size);
            return NULL;
        }

        g_total_len = size;
    } else {
        g_total_len = UMQ_BUF_DEFAULT_TOTAL_SIZE;
    }

    g_buffer_addr = (void *)memalign(umq_buf_size_small(), g_total_len);
    if (g_buffer_addr == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "memalign for qbuf pool failed, errno: %d\n", errno);
        return NULL;
    }

    UMQ_VLOG_INFO(VLOG_UMQ, "malloc umq io buf %lu bytes\n", g_total_len);

    return g_buffer_addr;
}

void umq_io_buf_free(void)
{
    if (g_buffer_addr != NULL) {
        free(g_buffer_addr);
        g_buffer_addr = NULL;
    }

    g_total_len = 0;
}

void *umq_io_buf_addr(void)
{
    return g_buffer_addr;
}

uint64_t umq_io_buf_size(void)
{
    return g_total_len;
}

int umq_buf_size_pow_small_set(umq_buf_block_size_t block_size)
{
    if (block_size < BLOCK_SIZE_8K || block_size >= BLOCK_SIZE_MAX) {
        UMQ_VLOG_ERR(VLOG_UMQ, "block size %d is invalid\n", block_size);
        return -UMQ_ERR_EINVAL;
    }

    if (block_size == BLOCK_SIZE_8K) {
        g_umq_qbuf_size_pow_samll = UMQ_QBUF_SIZE_POW_8K;
    } else if (block_size == BLOCK_SIZE_16K) {
        g_umq_qbuf_size_pow_samll = UMQ_QBUF_SIZE_POW_16K;
    } else if (block_size == BLOCK_SIZE_32K) {
        g_umq_qbuf_size_pow_samll = UMQ_QBUF_SIZE_POW_32K;
    } else {
        g_umq_qbuf_size_pow_samll = UMQ_QBUF_SIZE_POW_64K;
    }

    return UMQ_SUCCESS;
}

uint8_t umq_buf_size_pow_small(void)
{
    return g_umq_qbuf_size_pow_samll;
}

uint64_t umq_buf_to_id(char *buf, bool shm, bool with_data)
{
    // not support shm buf to id and name
    if (shm) {
        return 0;
    }

    if (umq_qbuf_mode_get() != UMQ_BUF_COMBINE) {
        return with_data ?
            buf_to_id_with_data_split((char *)g_qbuf_pool.header_buffer, buf) :
            buf_to_id_without_data_split((char *)g_qbuf_pool.ext_header_buffer, buf);
    }

    return buf_to_id_combine((char *)g_qbuf_pool.data_buffer, buf, g_qbuf_pool.block_size);
}

uint64_t umq_buf_to_id_with_header(umq_buf_list_t *header, char *buf, bool shm, bool *with_data)
{
    // not support shm buf to id and name
    if (shm) {
        return 0;
    }

    *with_data = true;

    if (umq_qbuf_mode_get() == UMQ_BUF_SPLIT && (void *)QBUF_LIST_FIRST(header) >= g_qbuf_pool.ext_header_buffer) {
        *with_data = false;
    }

    return umq_buf_to_id(buf, shm, *with_data);
}

void umq_qbuf_config_get(qbuf_pool_cfg_t *cfg)
{
    cfg->buf_addr = g_qbuf_pool.data_buffer;
    cfg->total_size = g_qbuf_pool.total_size;
    cfg->data_size = g_qbuf_pool.data_size;
    cfg->headroom_size = g_qbuf_pool.headroom_size;
    cfg->mode = g_qbuf_pool.mode;
}

static void release_thread_cache(uint64_t id);

static ALWAYS_INLINE local_block_pool_t *get_thread_cache(void)
{
    if (!g_thread_cache.inited) {
        QBUF_LIST_INIT(&g_thread_cache.block_pool.head_with_data);
        g_thread_cache.block_pool.buf_cnt_with_data = 0;
        QBUF_LIST_INIT(&g_thread_cache.block_pool.head_without_data);
        g_thread_cache.block_pool.buf_cnt_without_data = 0;
        g_thread_cache.inited = true;
        urpc_thread_closure_register(THREAD_CLOSURE_QBUF, 0, release_thread_cache);
    }

    return &g_thread_cache.block_pool;
}

// release all thread cache to global pool. should be called when thread exits
static ALWAYS_INLINE void release_thread_cache(uint64_t id)
{
    if (!g_thread_cache.inited || !g_qbuf_pool.inited) {
        return;
    }

    local_block_pool_t *local_pool = get_thread_cache();
    (void)pthread_mutex_lock(&g_qbuf_pool.block_pool.global_mutex);
    if (local_pool->head_with_data.first != NULL) {
        return_list_to_pools(QBUF_LIST_FIRST(&local_pool->head_with_data), &local_pool->buf_cnt_with_data,
            &g_qbuf_pool.block_pool.head_with_data, &g_qbuf_pool.block_pool.buf_cnt_with_data);
    }

    if (local_pool->head_without_data.first != NULL) {
        return_list_to_pools(QBUF_LIST_FIRST(&local_pool->head_without_data), &local_pool->buf_cnt_without_data,
            &g_qbuf_pool.block_pool.head_without_data, &g_qbuf_pool.block_pool.buf_cnt_without_data);
    }
    (void)pthread_mutex_unlock(&g_qbuf_pool.block_pool.global_mutex);
    g_thread_cache.inited = false;
}

static int umq_qbuf_expansion_pool_init(const qbuf_pool_cfg_t *cfg)
{
    g_qbuf_pool.expansion_pool = NULL;
    g_qbuf_pool.expansion_pool_list_capacity = 0;
    g_qbuf_pool.expansion_count = 0;
    g_qbuf_pool.expansion_block_count = (cfg->expansion_block_count == 0) ?
        QBUF_POOL_DEFAULT_EXPANSION_COUNT : cfg->expansion_block_count;
    g_qbuf_pool.expansion_pool_id_min = cfg->expansion_pool_id_min;
    g_qbuf_pool.expansion_pool_cnt_max = cfg->expansion_pool_cnt_max;
    g_qbuf_pool.register_seg_func = cfg->register_seg_func;
    g_qbuf_pool.unregister_seg_func = cfg->unregister_seg_func;

    if (g_qbuf_pool.expansion_block_count > 0 && g_qbuf_pool.register_seg_func != NULL) {
        int ret = urpc_id_generator_init(&g_qbuf_pool.dynamic_id_gen, URPC_ID_GENERATOR_TYPE_BITMAP,
            g_qbuf_pool.expansion_pool_cnt_max);
        if (ret != UMQ_SUCCESS) {
            UMQ_VLOG_ERR(VLOG_UMQ, "failed to init dynamic_id_gen, ret: %d\n", ret);
            return ret;
        }

        g_qbuf_pool.expansion_pool = (qbuf_expansion_pool_t **)calloc(QBUF_POOL_SEG_ARRAY_INIT_CAP,
            sizeof(qbuf_expansion_pool_t *));
        if (g_qbuf_pool.expansion_pool == NULL) {
            UMQ_VLOG_ERR(VLOG_UMQ, "failed to alloc expansion_pool array\n");
            urpc_id_generator_uninit(&g_qbuf_pool.dynamic_id_gen);
            return -UMQ_ERR_ENOMEM;
        }
        g_qbuf_pool.expansion_pool_list_capacity = QBUF_POOL_SEG_ARRAY_INIT_CAP;
    }
    return UMQ_SUCCESS;
}

int umq_qbuf_pool_init(qbuf_pool_cfg_t *cfg)
{
    if (g_qbuf_pool.inited) {
        UMQ_VLOG_INFO(VLOG_UMQ, "qbuf pool has already been inited\n");
        return -UMQ_ERR_EEXIST;
    }

    umq_qbuf_block_pool_init(&g_qbuf_pool.block_pool);
    g_qbuf_pool.mode = cfg->mode;
    g_qbuf_pool.total_size = cfg->total_size;
    g_qbuf_pool.headroom_size = cfg->headroom_size;
    g_qbuf_pool.data_size = cfg->data_size;

    int ret = umq_qbuf_expansion_pool_init(cfg);
    if (ret != UMQ_SUCCESS) {
        umq_qbuf_block_pool_uninit(&g_qbuf_pool.block_pool);
        return ret;
    }

    if (cfg->mode == UMQ_BUF_SPLIT) {
        uint32_t blk_size = umq_buf_size_small();
        uint64_t blk_num = cfg->total_size /
            ((UMQ_EMPTY_HEADER_COEFFICIENT + 1) * (uint32_t)sizeof(umq_buf_t) + blk_size);

        g_qbuf_pool.block_size = blk_size;
        g_qbuf_pool.total_block_num = blk_num;

        g_qbuf_pool.data_buffer = cfg->buf_addr;
        g_qbuf_pool.header_buffer = cfg->buf_addr + blk_num * blk_size;
        g_qbuf_pool.ext_header_buffer = g_qbuf_pool.header_buffer + blk_num * sizeof(umq_buf_t);

        for (uint64_t i = 0; i < blk_num; i++) {
            umq_buf_t *buf = id_to_buf_with_data_split((char *)g_qbuf_pool.header_buffer, i);
            buf->umqh = UMQ_INVALID_HANDLE;
            buf->buf_size = blk_size + (uint32_t)sizeof(umq_buf_t);
            buf->data_size = blk_size;
            buf->total_data_size = buf->data_size;
            buf->headroom_size = 0;
            buf->buf_data = g_qbuf_pool.data_buffer + i * blk_size;
            buf->mempool_id = 0;
            buf->alloc_state = QBUF_ALLOC_STATE_FREE;
            (void)memset(buf->qbuf_ext, 0, sizeof(buf->qbuf_ext));
            QBUF_LIST_INSERT_HEAD(&g_qbuf_pool.block_pool.head_with_data, buf);
        }

        uint64_t head_without_data_count = blk_num * UMQ_EMPTY_HEADER_COEFFICIENT;
        for (uint64_t i = 0; i < head_without_data_count; i++) {
            umq_buf_t *head_buf = id_to_buf_without_data_split((char *)g_qbuf_pool.ext_header_buffer, i);
            head_buf->umqh = UMQ_INVALID_HANDLE;
            head_buf->buf_size = (uint32_t)sizeof(umq_buf_t);
            head_buf->data_size = 0;
            head_buf->total_data_size = 0;
            head_buf->headroom_size = 0;
            head_buf->buf_data = NULL;
            head_buf->mempool_id = 0;
            head_buf->alloc_state = QBUF_ALLOC_STATE_FREE;
            (void)memset(head_buf->qbuf_ext, 0, sizeof(head_buf->qbuf_ext));
            QBUF_LIST_INSERT_HEAD(&g_qbuf_pool.block_pool.head_without_data, head_buf);
        }
        g_qbuf_pool.block_pool.buf_cnt_with_data = blk_num;
        g_qbuf_pool.block_pool.buf_cnt_without_data = head_without_data_count;
    } else if (cfg->mode == UMQ_BUF_COMBINE) {
        uint32_t blk_size = umq_buf_size_small();
        uint64_t blk_num = cfg->total_size / blk_size;

        g_qbuf_pool.data_buffer = cfg->buf_addr;
        g_qbuf_pool.header_buffer = NULL;
        g_qbuf_pool.ext_header_buffer = NULL;

        g_qbuf_pool.block_size = blk_size;
        g_qbuf_pool.total_block_num = blk_num;

        for (uint64_t i = 0; i < blk_num; i++) {
            umq_buf_t *buf = id_to_buf_combine((char *)g_qbuf_pool.data_buffer, i, g_qbuf_pool.block_size);
            buf->umqh = UMQ_INVALID_HANDLE;
            buf->buf_size = blk_size;
            buf->data_size = blk_size - (uint32_t)sizeof(umq_buf_t);
            buf->total_data_size = buf->data_size;
            buf->headroom_size = 0;
            buf->buf_data = (char *)buf + sizeof(umq_buf_t);
            buf->mempool_id = 0;
            buf->alloc_state = QBUF_ALLOC_STATE_FREE;
            (void)memset(buf->qbuf_ext, 0, sizeof(buf->qbuf_ext));
            QBUF_LIST_INSERT_HEAD(&g_qbuf_pool.block_pool.head_with_data, buf);
        }
        g_qbuf_pool.block_pool.buf_cnt_with_data = blk_num;
        g_qbuf_pool.block_pool.buf_cnt_without_data = 0;
    } else {
        umq_qbuf_block_pool_uninit(&g_qbuf_pool.block_pool);
        UMQ_VLOG_ERR(VLOG_UMQ, "buf mode: %d is invalid\n", cfg->mode);
        return -UMQ_ERR_EINVAL;
    }

    g_qbuf_pool.inited = true;
    return UMQ_SUCCESS;
}

static void umq_qbuf_expansion_pool_uninit(void)
{
    (void)pthread_mutex_lock(&g_qbuf_pool.block_pool.global_mutex);
    if (g_qbuf_pool.expansion_pool != NULL) {
        for (uint32_t i = 0; i < g_qbuf_pool.expansion_pool_list_capacity; i++) {
            qbuf_expansion_pool_t *exp_pool = g_qbuf_pool.expansion_pool[i];
            if (exp_pool == NULL) {
                continue;
            }
            if (g_qbuf_pool.unregister_seg_func != NULL && exp_pool->data_buffer != NULL) {
                uint16_t mempool_id = (uint16_t)(exp_pool->dynamic_mempool_id + g_qbuf_pool.expansion_pool_id_min);
                g_qbuf_pool.unregister_seg_func(NULL, mempool_id);
            }
            if (exp_pool->data_buffer != NULL) {
                free(exp_pool->data_buffer);
            }
            free(exp_pool);
        }
        free(g_qbuf_pool.expansion_pool);
    }

    if (g_qbuf_pool.dynamic_id_gen.private_data != NULL) {
        urpc_id_generator_uninit(&g_qbuf_pool.dynamic_id_gen);
    }
    (void)pthread_mutex_unlock(&g_qbuf_pool.block_pool.global_mutex);
}

void umq_qbuf_pool_uninit(void)
{
    if (!g_qbuf_pool.inited) {
        return;
    }
    release_thread_cache(0);

    umq_qbuf_expansion_pool_uninit();

    umq_qbuf_block_pool_uninit(&g_qbuf_pool.block_pool);
    memset(&g_qbuf_pool, 0, sizeof(qbuf_pool_t));
}

static int alloc_expansion_pool_slot(qbuf_expansion_pool_t **slot)
{
    uint32_t id = 0;
    int ret = urpc_id_generator_alloc(&g_qbuf_pool.dynamic_id_gen, 0, &id);
    if (ret != UMQ_SUCCESS) {
        UMQ_VLOG_ERR(VLOG_UMQ, "failed to alloc dynamic id, ret: %d\n", ret);
        return ret;
    }

    if (id >= g_qbuf_pool.expansion_pool_list_capacity) {
        uint32_t new_capacity = (id + 1) * 2;
        qbuf_expansion_pool_t **new_array = (qbuf_expansion_pool_t **)realloc(
            g_qbuf_pool.expansion_pool, new_capacity * sizeof(qbuf_expansion_pool_t *));
        if (new_array == NULL) {
            UMQ_VLOG_ERR(VLOG_UMQ, "failed to realloc expansion_pool array\n");
            urpc_id_generator_free(&g_qbuf_pool.dynamic_id_gen, id);
            return -UMQ_ERR_ENOMEM;
        }
        for (uint32_t i = g_qbuf_pool.expansion_pool_list_capacity; i < new_capacity; i++) {
            new_array[i] = NULL;
        }
        g_qbuf_pool.expansion_pool = new_array;
        g_qbuf_pool.expansion_pool_list_capacity = new_capacity;
    }

    qbuf_expansion_pool_t *exp_pool = (qbuf_expansion_pool_t *)calloc(1, sizeof(qbuf_expansion_pool_t));
    if (exp_pool == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "failed to alloc qbuf_expansion_pool_t\n");
        urpc_id_generator_free(&g_qbuf_pool.dynamic_id_gen, id);
        return -UMQ_ERR_ENOMEM;
    }

    exp_pool->dynamic_mempool_id = id;
    QBUF_LIST_INIT(&exp_pool->free_list_with_data);
    QBUF_LIST_INIT(&exp_pool->free_list_without_data);

    g_qbuf_pool.expansion_pool[id] = exp_pool;
    g_qbuf_pool.expansion_count++;
    *slot = exp_pool;
    return UMQ_SUCCESS;
}

int expand_global_pool(bool with_data)
{
    if (g_qbuf_pool.expansion_count >= g_qbuf_pool.expansion_pool_cnt_max) {
        UMQ_VLOG_ERR(VLOG_UMQ, "expansion pool count %u reached max %u\n",
            g_qbuf_pool.expansion_count, g_qbuf_pool.expansion_pool_cnt_max);
        return -UMQ_ERR_ENOMEM;
    }

    if (g_qbuf_pool.register_seg_func == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "register_seg_func is NULL, expansion not supported\n");
        return -UMQ_ERR_ENOMEM;
    }

    qbuf_expansion_pool_t *exp_pool = NULL;
    int ret = alloc_expansion_pool_slot(&exp_pool);
    if (ret != UMQ_SUCCESS) {
        return ret;
    }

    uint64_t blk_size = umq_buf_size_small();
    uint64_t blk_count = g_qbuf_pool.expansion_block_count;
    uint64_t total_size;

    if (g_qbuf_pool.mode == UMQ_BUF_SPLIT) {
        uint64_t header_size = blk_count * sizeof(umq_buf_t);
        uint64_t ext_header_size = blk_count * UMQ_EMPTY_HEADER_COEFFICIENT * sizeof(umq_buf_t);
        total_size = blk_count * blk_size + header_size + ext_header_size;

        exp_pool->data_buffer = (void *)memalign(blk_size, total_size);
        if (exp_pool->data_buffer == NULL) {
            UMQ_VLOG_ERR(VLOG_UMQ, "failed to alloc expansion pool memory\n");
            goto FREE_SLOT;
        }

        exp_pool->header_buffer = (char *)exp_pool->data_buffer + blk_count * blk_size;
        exp_pool->ext_header_buffer = (char *)exp_pool->header_buffer + header_size;
        exp_pool->seg_size = total_size;
        exp_pool->total_blk_with_data = blk_count;
        exp_pool->free_blk_with_data = blk_count;
        exp_pool->total_blk_without_data = blk_count * UMQ_EMPTY_HEADER_COEFFICIENT;
        exp_pool->free_blk_without_data = exp_pool->total_blk_without_data;

        for (uint64_t i = 0; i < blk_count; i++) {
            umq_buf_t *buf = (umq_buf_t *)((char *)exp_pool->header_buffer + i * sizeof(umq_buf_t));
            buf->umqh = UMQ_INVALID_HANDLE;
            buf->buf_size = blk_size + (uint32_t)sizeof(umq_buf_t);
            buf->data_size = blk_size;
            buf->total_data_size = buf->data_size;
            buf->headroom_size = 0;
            buf->buf_data = (char *)exp_pool->data_buffer + i * blk_size;
            buf->mempool_id = (uint16_t)(exp_pool->dynamic_mempool_id + g_qbuf_pool.expansion_pool_id_min);
            buf->alloc_state = QBUF_ALLOC_STATE_FREE;
            (void)memset(buf->qbuf_ext, 0, sizeof(buf->qbuf_ext));
            QBUF_LIST_INSERT_HEAD(&exp_pool->free_list_with_data, buf);
        }

        for (uint64_t i = 0; i < exp_pool->total_blk_without_data; i++) {
            umq_buf_t *head_buf = (umq_buf_t *)((char *)exp_pool->ext_header_buffer + i * sizeof(umq_buf_t));
            head_buf->umqh = UMQ_INVALID_HANDLE;
            head_buf->buf_size = (uint32_t)sizeof(umq_buf_t);
            head_buf->data_size = 0;
            head_buf->total_data_size = 0;
            head_buf->headroom_size = 0;
            head_buf->buf_data = NULL;
            head_buf->mempool_id = (uint16_t)(exp_pool->dynamic_mempool_id + g_qbuf_pool.expansion_pool_id_min);
            head_buf->alloc_state = QBUF_ALLOC_STATE_FREE;
            (void)memset(head_buf->qbuf_ext, 0, sizeof(head_buf->qbuf_ext));
            QBUF_LIST_INSERT_HEAD(&exp_pool->free_list_without_data, head_buf);
        }
    } else {
        total_size = blk_count * blk_size;

        exp_pool->data_buffer = (void *)memalign(blk_size, total_size);
        if (exp_pool->data_buffer == NULL) {
            UMQ_VLOG_ERR(VLOG_UMQ, "failed to alloc expansion pool memory\n");
            goto FREE_SLOT;
        }

        exp_pool->header_buffer = NULL;
        exp_pool->ext_header_buffer = NULL;
        exp_pool->seg_size = total_size;
        exp_pool->total_blk_with_data = blk_count;
        exp_pool->free_blk_with_data = blk_count;
        exp_pool->total_blk_without_data = 0;
        exp_pool->free_blk_without_data = 0;

        for (uint64_t i = 0; i < blk_count; i++) {
            umq_buf_t *buf = (umq_buf_t *)((char *)exp_pool->data_buffer + i * blk_size);
            buf->umqh = UMQ_INVALID_HANDLE;
            buf->buf_size = blk_size;
            buf->data_size = blk_size - (uint32_t)sizeof(umq_buf_t);
            buf->total_data_size = buf->data_size;
            buf->headroom_size = 0;
            buf->buf_data = (char *)buf + sizeof(umq_buf_t);
            buf->mempool_id = (uint16_t)(exp_pool->dynamic_mempool_id + g_qbuf_pool.expansion_pool_id_min);
            buf->alloc_state = QBUF_ALLOC_STATE_FREE;
            (void)memset(buf->qbuf_ext, 0, sizeof(buf->qbuf_ext));
            QBUF_LIST_INSERT_HEAD(&exp_pool->free_list_with_data, buf);
        }
    }

    uint16_t mempool_id = (uint16_t)(exp_pool->dynamic_mempool_id + g_qbuf_pool.expansion_pool_id_min);
    ret = g_qbuf_pool.register_seg_func(NULL, mempool_id, exp_pool->data_buffer, exp_pool->seg_size);
    if (ret != UMQ_SUCCESS) {
        UMQ_VLOG_ERR(VLOG_UMQ, "failed to register expansion pool seg, ret: %d\n", ret);
        goto FREE_DATA_BUFFER;
    }

    return UMQ_SUCCESS;

FREE_DATA_BUFFER:
    free(exp_pool->data_buffer);
    exp_pool->data_buffer = NULL;

FREE_SLOT:
    free_expansion_pool_slot(exp_pool);
    return -UMQ_ERR_ENOMEM;
}

uint32_t fetch_from_expansion_pools(bool with_data, uint32_t need,
    umq_buf_list_t *local_head, uint64_t *local_buf_cnt)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < g_qbuf_pool.expansion_count && need > 0; i++) {
        qbuf_expansion_pool_t *exp_pool = g_qbuf_pool.expansion_pool[i];
        if (exp_pool == NULL) {
            continue;
        }

        umq_buf_list_t *exp_free_list;
        uint64_t *exp_free_cnt;
        if (with_data) {
            exp_free_list = &exp_pool->free_list_with_data;
            exp_free_cnt = &exp_pool->free_blk_with_data;
        } else {
            exp_free_list = &exp_pool->free_list_without_data;
            exp_free_cnt = &exp_pool->free_blk_without_data;
        }

        uint32_t take_cnt = (need < (uint32_t)*exp_free_cnt) ? need : (uint32_t)*exp_free_cnt;

        if (take_cnt > 0) {
            uint32_t cnt = 0;
            umq_buf_t *cur_node;
            QBUF_LIST_FOR_EACH(cur_node, exp_free_list) {
                if (++cnt == take_cnt || QBUF_LIST_NEXT(cur_node) == NULL) {
                    break;
                }
            }

            umq_buf_t *input_head = QBUF_LIST_FIRST(exp_free_list);
            umq_buf_t *output_head = QBUF_LIST_FIRST(local_head);
            QBUF_LIST_FIRST(exp_free_list) = QBUF_LIST_NEXT(cur_node);
            QBUF_LIST_FIRST(local_head) = input_head;
            // set output
            QBUF_LIST_NEXT(cur_node) = output_head;
            *local_buf_cnt += take_cnt;
            *exp_free_cnt -= take_cnt;
            count += take_cnt;
            need -= take_cnt;
        }
    }
    return count;
}

int umq_qbuf_alloc(uint32_t request_size, uint32_t num, umq_alloc_option_t *option, umq_buf_list_t *list)
{
    if (!g_qbuf_pool.inited) {
        UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "qbuf pool has not been inited\n");
        return -UMQ_ERR_ENOMEM;
    }

    local_block_pool_t *local_pool = get_thread_cache();
    bool flag = (option != NULL && (option->flag & UMQ_ALLOC_FLAG_HEAD_ROOM_SIZE) != 0);
    qbuf_alloc_param_t param;
    param.shm = false;
    param.headroom_size = flag ? option->headroom_size : g_qbuf_pool.headroom_size;
    int ret = UMQ_SUCCESS;

    if (g_qbuf_pool.mode == UMQ_BUF_SPLIT) {
        param.actual_buf_count =
            num * ((request_size + param.headroom_size + umq_buf_size_small() - 1) >> umq_buf_size_pow_small());
    } else {
        uint32_t align_size = umq_buf_size_small() - sizeof(umq_buf_t);
        param.actual_buf_count = num * ((request_size + param.headroom_size + align_size - 1) / align_size);
    }

    if (request_size == 0) {
        if (flag && param.headroom_size > 0) {
            UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "headroom_size not supported when request_size is 0\n");
            return -UMQ_ERR_EINVAL;
        }

        if (g_qbuf_pool.mode != UMQ_BUF_SPLIT) {
            UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "cannot alloc memory size 0 in combine mode\n");
            return -UMQ_ERR_ENOMEM;
        }

        while (local_pool->buf_cnt_without_data < num) {
            ret = fetch_from_global(&g_qbuf_pool.block_pool, local_pool, false, QBUF_POOL_BATCH_CNT);
            if (ret <= 0) {
                UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "fetch from global failed, current size: %u, alloc num: %u, status: %d\n",
                    local_pool->buf_cnt_without_data, num, ret);
                return ret;
            }
        }

        umq_qbuf_alloc_nodata(local_pool, num, list, param.shm);

        return 0;
    }

    while (local_pool->buf_cnt_with_data < param.actual_buf_count) {
        ret = fetch_from_global(&g_qbuf_pool.block_pool, local_pool, true, QBUF_POOL_BATCH_CNT);
        if (ret <= 0) {
            UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "fetch from global failed, current size: %u, alloc num: %u, status: %d\n",
                local_pool->buf_cnt_with_data, param.actual_buf_count, ret);
            return ret;
        }
    }

    if (g_qbuf_pool.mode == UMQ_BUF_SPLIT) {
        umq_qbuf_alloc_data_with_split(local_pool, request_size, &param, list);
    } else {
        umq_qbuf_alloc_data_with_combine(local_pool, request_size, &param, list);
    }
    return UMQ_SUCCESS;
}

void umq_qbuf_free(umq_buf_list_t *list)
{
    if (!g_qbuf_pool.inited) {
        UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "qbuf pool has not been inited\n");
        return;
    }

    local_block_pool_t *local_pool = get_thread_cache();
    // split mode and buf is in head no data zone
    if (g_qbuf_pool.mode == UMQ_BUF_SPLIT && (void *)QBUF_LIST_FIRST(list) >= g_qbuf_pool.ext_header_buffer) {
        // put buf list before head of head_without_data
        uint32_t cnt = release_batch(list, &local_pool->head_without_data, false);
        local_pool->buf_cnt_without_data += cnt;

        // if local list node count reaches QBUF_POOL_TLS_MAX + QBUF_POOL_BATCH_CNT, return some nodes to global
        if (local_pool->buf_cnt_without_data >= QBUF_POOL_TLS_MAX + QBUF_POOL_BATCH_CNT) {
            return_to_global(&g_qbuf_pool.block_pool, local_pool, false, QBUF_POOL_TLS_MAX);
        }

        return;
    }

    uint32_t cnt = release_batch(list, &local_pool->head_with_data, false);
    local_pool->buf_cnt_with_data += cnt;

    // if local list node count reaches QBUF_POOL_TLS_MAX + QBUF_POOL_BATCH_CNT, return some nodes to global
    if (local_pool->buf_cnt_with_data > QBUF_POOL_TLS_MAX + QBUF_POOL_BATCH_CNT) {
        return_to_global(&g_qbuf_pool.block_pool, local_pool, true, QBUF_POOL_TLS_MAX);
    }
}

int umq_qbuf_headroom_reset(umq_buf_t *qbuf, uint16_t headroom_size)
{
    if (!g_qbuf_pool.inited) {
        UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "qbuf pool has not been inited\n");
        return -UMQ_ERR_ENOMEM;
    }
    return headroom_reset(qbuf, headroom_size, g_qbuf_pool.mode, g_qbuf_pool.block_size);
}

umq_buf_t *umq_qbuf_data_to_head(void *data)
{
    if (!g_qbuf_pool.inited) {
        UMQ_VLOG_ERR(VLOG_UMQ, "qbuf pool has not been inited\n");
        return NULL;
    }

    if (g_qbuf_pool.mode == UMQ_BUF_SPLIT) {
        if (data >= g_qbuf_pool.data_buffer && data < g_qbuf_pool.header_buffer) {
            uint64_t id =
                ((uint64_t)(uintptr_t)data - (uint64_t)(uintptr_t)g_qbuf_pool.data_buffer) / g_qbuf_pool.block_size;
            return (umq_buf_t *)(g_qbuf_pool.header_buffer + id * sizeof(umq_buf_t));
        }
    } else {
        if (data >= g_qbuf_pool.data_buffer && data < g_qbuf_pool.data_buffer + g_qbuf_pool.total_size) {
            uint64_t id =
                ((uint64_t)(uintptr_t)data - (uint64_t)(uintptr_t)g_qbuf_pool.data_buffer) / g_qbuf_pool.block_size;
            return (umq_buf_t *)(g_qbuf_pool.data_buffer + id * g_qbuf_pool.block_size);
        }
    }

    return NULL;
}

uint32_t umq_qbuf_headroom_get(void)
{
    return g_qbuf_pool.headroom_size;
}

umq_buf_mode_t umq_qbuf_mode_get(void)
{
    return g_qbuf_pool.mode;
}

int umq_qbuf_pool_info_get(umq_qbuf_pool_stats_t *qbuf_pool_stats)
{
    if (!g_qbuf_pool.inited) {
        UMQ_VLOG_ERR(VLOG_UMQ, "qbuf pool has not been inited\n");
        return -UMQ_ERR_ENOMEM;
    }

    if (qbuf_pool_stats->num >= UMQ_STATS_QBUF_POOL_TYPE_MAX) {
        UMQ_VLOG_ERR(VLOG_UMQ, "count of qbuf pool info exceeds maximum %u\n", UMQ_STATS_QBUF_POOL_TYPE_MAX);
        return -UMQ_ERR_EINVAL;
    }

    umq_qbuf_pool_info_t *qbuf_pool_info = &qbuf_pool_stats->qbuf_pool_info[qbuf_pool_stats->num];
    uint32_t block_size = g_qbuf_pool.block_size;
    uint32_t umq_buf_t_size = (uint32_t)sizeof(umq_buf_t);
    umq_buf_mode_t mode = g_qbuf_pool.mode;
    qbuf_pool_info->mode = mode;
    qbuf_pool_info->total_size = g_qbuf_pool.total_size;
    qbuf_pool_info->headroom_size = g_qbuf_pool.headroom_size;
    qbuf_pool_info->block_size = block_size;
    qbuf_pool_info->total_block_num = g_qbuf_pool.total_block_num;
    qbuf_pool_info->umq_buf_t_size = umq_buf_t_size;
    if (mode == UMQ_BUF_SPLIT) {
        qbuf_pool_info->data_size = block_size;
        qbuf_pool_info->buf_size = block_size + umq_buf_t_size;
        qbuf_pool_info->available_mem.split.block_num_with_data = g_qbuf_pool.block_pool.buf_cnt_with_data;
        qbuf_pool_info->available_mem.split.size_with_data = g_qbuf_pool.block_pool.buf_cnt_with_data *
            (block_size + umq_buf_t_size);
        qbuf_pool_info->available_mem.split.block_num_without_data =
            g_qbuf_pool.block_pool.buf_cnt_without_data;
        qbuf_pool_info->available_mem.split.size_without_data =
            g_qbuf_pool.block_pool.buf_cnt_without_data * umq_buf_t_size;
    } else {
        qbuf_pool_info->data_size = block_size - umq_buf_t_size;
        qbuf_pool_info->buf_size = block_size;
        qbuf_pool_info->available_mem.combine.block_num_with_data =
            g_qbuf_pool.block_pool.buf_cnt_with_data;
        qbuf_pool_info->available_mem.combine.size_with_data = g_qbuf_pool.block_pool.buf_cnt_with_data * block_size;
    }
    qbuf_pool_stats->num++;
    return UMQ_SUCCESS;
}

int umq_qbuf_register_seg(uint8_t *ctx, register_seg_callback_t register_seg_func)
{
    int ret;

    ret = register_seg_func(ctx, UMQ_QBUF_DEFAULT_MEMPOOL_ID,
                           g_qbuf_pool.data_buffer, g_qbuf_pool.total_size);
    if (ret != UMQ_SUCCESS) {
        return ret;
    }

    (void)pthread_mutex_lock(&g_qbuf_pool.block_pool.global_mutex);
    for (uint32_t i = 0; i < g_qbuf_pool.expansion_pool_list_capacity; i++) {
        qbuf_expansion_pool_t *pool = g_qbuf_pool.expansion_pool[i];
        if (pool == NULL) {
            continue;
        }

        uint16_t mempool_id = (uint16_t)(pool->dynamic_mempool_id + g_qbuf_pool.expansion_pool_id_min);
        ret = register_seg_func(ctx, mempool_id, pool->data_buffer, pool->seg_size);
        if (ret != UMQ_SUCCESS) {
            (void)pthread_mutex_unlock(&g_qbuf_pool.block_pool.global_mutex);
            UMQ_VLOG_ERR(VLOG_UMQ, "failed to register expansion pool seg, ret: %d\n", ret);
            return ret;
        }
    }
    (void)pthread_mutex_unlock(&g_qbuf_pool.block_pool.global_mutex);
    return UMQ_SUCCESS;
}

void umq_qbuf_unregister_seg(uint8_t *ctx, unregister_seg_callback_t unregister_seg_func)
{
    unregister_seg_func(ctx, UMQ_QBUF_DEFAULT_MEMPOOL_ID);
    (void)pthread_mutex_lock(&g_qbuf_pool.block_pool.global_mutex);
    for (uint32_t i = 0; i < g_qbuf_pool.expansion_pool_list_capacity; i++) {
        qbuf_expansion_pool_t *pool = g_qbuf_pool.expansion_pool[i];
        if (pool == NULL) {
            continue;
        }

        uint16_t mempool_id = (uint16_t)(pool->dynamic_mempool_id + g_qbuf_pool.expansion_pool_id_min);
        unregister_seg_func(ctx, mempool_id);
    }
    (void)pthread_mutex_unlock(&g_qbuf_pool.block_pool.global_mutex);
}