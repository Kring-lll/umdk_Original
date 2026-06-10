/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: realize tiny qbuf pool function
 * Create: 2026-5-28
 */

#include <malloc.h>
#include <unistd.h>
#include <sys/mman.h>

#include "umq_errno.h"
#include "umq_vlog.h"
#include "urpc_list.h"
#include "urpc_thread_closure.h"
#include "umq_tiny_qbuf_pool.h"

#define TINY_QBUF_POOL_TLS_MAX (2048)
#define TINY_QBUF_POOL_BATCH_CNT (64)
#define TINY_QBUF_MEMALIGN_SIZE (2ULL * 1024ULL * 1024ULL)
#define TINY_QBUF_POOL_SHRINK_THRESHOLD (64)
#define TINY_QBUF_POOL_SELF_SHRINK_RATIO (4)

typedef struct local_tiny_qbuf_pool {
    urpc_list_t tls_node;
    bool inited;
    local_block_pool_t block_pool;
    umq_local_qbuf_pool_stats_t stats;
} local_tiny_qbuf_pool_t;

typedef struct tiny_qbuf_pool {
    bool inited;
    void *data_buffer;
    void *header_buffer;
    uint64_t total_size;
    uint32_t block_size;
    uint32_t headroom_size;
    uint64_t total_block_num;
    uint64_t tls_qbuf_pool_depth;
    uint64_t tls_expand_qbuf_pool_depth;
    umq_buf_mode_t mode;
    global_block_pool_t block_pool;
} tiny_qbuf_pool_t;

static tiny_qbuf_pool_t g_tiny_qbuf_pool = {0};
static __thread local_tiny_qbuf_pool_t g_thread_tiny_cache = {0};
static urpc_list_t g_tiny_tls_register_head;
static pthread_spinlock_t g_tiny_tls_stats_lock;
static void *g_tiny_buffer_addr = NULL;
static uint64_t g_tiny_total_len = 0;
static volatile uint64_t g_tiny_total_local_cap_with_data = 0;

static void release_tiny_thread_cache(uint64_t id);

static inline uint64_t umq_tiny_qbuf_pool_expand_max(uint64_t total_size) {
    return ((total_size) - (total_size) / 8);
}

uint32_t umq_tiny_buf_block_size_bytes(umq_tiny_buf_block_size_t size_enum)
{
    switch (size_enum) {
        case TINY_BLOCK_SIZE_512:
            return 512U;
        case TINY_BLOCK_SIZE_1K:
            return 1024U;
        case TINY_BLOCK_SIZE_2K:
            return 2U * 1024U;
        case TINY_BLOCK_SIZE_4K:
            return 4U * 1024U;
        case TINY_BLOCK_SIZE_8K:
            return 8U * 1024U;
        default:
            return 0;
    }
}

void *umq_tiny_io_buf_malloc(umq_buf_mode_t buf_mode, uint64_t size)
{
    if (g_tiny_buffer_addr != NULL) {
        return g_tiny_buffer_addr;
    }

    uint64_t min_size = UMQ_TINY_QBUF_BLOCK_SIZE;
    if (buf_mode == UMQ_BUF_SPLIT) {
        min_size = UMQ_TINY_QBUF_BLOCK_SIZE + sizeof(umq_buf_t);
    }

    uint64_t alloc_size = size == 0 ? UMQ_TINY_QBUF_POOL_MAX_SIZE : size;
    if (alloc_size > UMQ_TINY_QBUF_POOL_MAX_SIZE) {
        alloc_size = UMQ_TINY_QBUF_POOL_MAX_SIZE;
    }
    if (alloc_size < min_size) {
        UMQ_VLOG_ERR(VLOG_UMQ, "tiny qbuf memory size %lu invalid, expect at least %lu\n", alloc_size, min_size);
        return NULL;
    }

    g_tiny_total_len = alloc_size;
    g_tiny_buffer_addr = (void *)memalign(TINY_QBUF_MEMALIGN_SIZE, g_tiny_total_len);
    if (g_tiny_buffer_addr == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "memalign for tiny qbuf pool failed, errno: %d\n", errno);
        g_tiny_total_len = 0;
        return NULL;
    }
    madvise(g_tiny_buffer_addr, g_tiny_total_len, MADV_HUGEPAGE);
    UMQ_VLOG_INFO(VLOG_UMQ, "malloc tiny qbuf io buf %lu bytes\n", g_tiny_total_len);
    return g_tiny_buffer_addr;
}

void umq_tiny_io_buf_free(void)
{
    if (g_tiny_buffer_addr != NULL) {
        free(g_tiny_buffer_addr);
        g_tiny_buffer_addr = NULL;
    }
    g_tiny_total_len = 0;
}

void *umq_tiny_io_buf_addr(void)
{
    return g_tiny_buffer_addr;
}

uint64_t umq_tiny_io_buf_size(void)
{
    return g_tiny_total_len;
}

static ALWAYS_INLINE local_block_pool_t *get_tiny_thread_cache(void)
{
    if (!g_thread_tiny_cache.inited) {
        QBUF_LIST_INIT(&g_thread_tiny_cache.block_pool.head_with_data);
        g_thread_tiny_cache.block_pool.buf_cnt_with_data = 0;
        g_thread_tiny_cache.block_pool.capacity_with_data = 0;
        QBUF_LIST_INIT(&g_thread_tiny_cache.block_pool.head_without_data);
        g_thread_tiny_cache.block_pool.buf_cnt_without_data = 0;
        g_thread_tiny_cache.block_pool.capacity_without_data = 0;
        (void)memset(&g_thread_tiny_cache.stats, 0, sizeof(g_thread_tiny_cache.stats));
        g_thread_tiny_cache.stats.tid = (uint64_t)pthread_self();
        g_thread_tiny_cache.inited = true;
        urpc_thread_closure_register(THREAD_CLOSURE_TINY_QBUF, 0, release_tiny_thread_cache);
        (void)pthread_spin_lock(&g_tiny_tls_stats_lock);
        urpc_list_push_back(&g_tiny_tls_register_head, &g_thread_tiny_cache.tls_node);
        (void)pthread_spin_unlock(&g_tiny_tls_stats_lock);
    }

    return &g_thread_tiny_cache.block_pool;
}

static uint32_t tiny_release_batch(umq_buf_list_t *input, umq_buf_list_t *output)
{
    uint32_t cnt = 0;
    umq_buf_t *cur_node;
    umq_buf_t *last_node = NULL;
    QBUF_LIST_FOR_EACH(cur_node, input) {
        ++cnt;
        last_node = cur_node;
        if (cur_node->alloc_state == QBUF_ALLOC_STATE_FREE) {
            UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "tiny qbuf detect double free\n");
        }
        cur_node->alloc_state = QBUF_ALLOC_STATE_FREE;
    }

    umq_buf_t *output_head = QBUF_LIST_FIRST(output);
    QBUF_LIST_FIRST(output) = QBUF_LIST_FIRST(input);
    QBUF_LIST_NEXT(last_node) = output_head;
    return cnt;
}

static int32_t tiny_fetch_from_global(local_block_pool_t *cache_pool, uint32_t batch_count)
{
    (void)pthread_spin_lock(&g_tiny_qbuf_pool.block_pool.global_mutex);
    if (g_tiny_qbuf_pool.block_pool.buf_cnt_with_data == 0) {
        (void)pthread_spin_unlock(&g_tiny_qbuf_pool.block_pool.global_mutex);
        return -UMQ_ERR_ENOMEM;
    }

    uint32_t take = batch_count;
    if (g_tiny_qbuf_pool.block_pool.buf_cnt_with_data < take) {
        take = (uint32_t)g_tiny_qbuf_pool.block_pool.buf_cnt_with_data;
    }
    uint32_t count = allocate_batch(&g_tiny_qbuf_pool.block_pool.head_with_data, take, &cache_pool->head_with_data);
    g_tiny_qbuf_pool.block_pool.buf_cnt_with_data -= count;
    cache_pool->buf_cnt_with_data += count;
    (void)pthread_spin_unlock(&g_tiny_qbuf_pool.block_pool.global_mutex);
    return (int32_t)count;
}

static uint64_t tiny_return_list_to_global(umq_buf_t *head, umq_buf_list_t *global_head, uint64_t *global_buf_cnt)
{
    if (head == NULL) {
        return 0;
    }

    uint64_t cnt = 1;
    umq_buf_t *tail = head;
    while (QBUF_LIST_NEXT(tail) != NULL) {
        cnt++;
        tail = QBUF_LIST_NEXT(tail);
    }

    QBUF_LIST_NEXT(tail) = QBUF_LIST_FIRST(global_head);
    QBUF_LIST_FIRST(global_head) = head;
    *global_buf_cnt += cnt;
    return cnt;
}

static void tiny_return_to_global(local_block_pool_t *local_pool, uint32_t threshold)
{
    (void)pthread_spin_lock(&g_tiny_qbuf_pool.block_pool.global_mutex);
    umq_buf_list_t *local_head = &local_pool->head_with_data;
    if (threshold == 0) {
        umq_buf_t *head = QBUF_LIST_FIRST(local_head);
        QBUF_LIST_FIRST(local_head) = NULL;
        uint64_t return_buf_cnt = tiny_return_list_to_global(head, &g_tiny_qbuf_pool.block_pool.head_with_data,
            &g_tiny_qbuf_pool.block_pool.buf_cnt_with_data);
        local_pool->buf_cnt_with_data -= return_buf_cnt;
        g_thread_tiny_cache.stats.tls_return_buf_cnt_with_data += return_buf_cnt;
        (void)pthread_spin_unlock(&g_tiny_qbuf_pool.block_pool.global_mutex);
        return;
    }

    umq_buf_t *switch_node = NULL;
    uint32_t cnt = 0;
    QBUF_LIST_FOR_EACH(switch_node, local_head) {
        if (++cnt == threshold) {
            break;
        }
    }

    if (switch_node != NULL && QBUF_LIST_NEXT(switch_node) != NULL) {
        umq_buf_t *head = QBUF_LIST_NEXT(switch_node);
        QBUF_LIST_NEXT(switch_node) = NULL;
        uint64_t return_buf_cnt = tiny_return_list_to_global(head, &g_tiny_qbuf_pool.block_pool.head_with_data,
            &g_tiny_qbuf_pool.block_pool.buf_cnt_with_data);
        local_pool->buf_cnt_with_data -= return_buf_cnt;
        g_thread_tiny_cache.stats.tls_return_buf_cnt_with_data += return_buf_cnt;
    }
    (void)pthread_spin_unlock(&g_tiny_qbuf_pool.block_pool.global_mutex);
}

static void tiny_thread_cache_self_shrink(local_block_pool_t *local_pool)
{
    uint64_t remaining = local_pool->buf_cnt_with_data;
    uint64_t shrink = remaining / TINY_QBUF_POOL_SELF_SHRINK_RATIO;
    if (shrink < TINY_QBUF_POOL_SHRINK_THRESHOLD || local_pool->capacity_with_data == 0) {
        return;
    }

    qbuf_tls_capacity_sub(&local_pool->capacity_with_data, &g_tiny_total_local_cap_with_data, shrink);
    if (local_pool->capacity_with_data >= remaining) {
        return;
    }

    tiny_return_to_global(local_pool, (uint32_t)local_pool->capacity_with_data);
    g_thread_tiny_cache.stats.tls_return_cnt_with_data++;
}

static int tiny_local_pool_fetch(uint32_t needed, local_block_pool_t *local_pool)
{
    uint32_t fetch_count = 0;
    uint32_t batch_count = qbuf_tls_round_batch(needed, TINY_QBUF_POOL_BATCH_CNT);
    uint64_t grow = qbuf_tls_capacity_grow(local_pool->capacity_with_data, &g_tiny_total_local_cap_with_data,
        g_tiny_qbuf_pool.tls_qbuf_pool_depth, g_tiny_qbuf_pool.tls_expand_qbuf_pool_depth, batch_count);

    while (fetch_count < batch_count) {
        int32_t ret = tiny_fetch_from_global(local_pool, batch_count - fetch_count);
        if (ret <= 0) {
            UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "tiny qbuf pool not enough, suggestion: increase tiny qbuf total_size\n");
            return ret;
        }
        fetch_count += (uint32_t)ret;
    }
    qbuf_tls_capacity_add(&local_pool->capacity_with_data, &g_tiny_total_local_cap_with_data, grow);
    g_thread_tiny_cache.stats.tls_fetch_cnt_with_data++;
    g_thread_tiny_cache.stats.tls_fetch_buf_cnt_with_data += fetch_count;
    return UMQ_SUCCESS;
}

static void release_tiny_thread_cache(uint64_t id)
{
    (void)id;
    if (!g_thread_tiny_cache.inited || !g_tiny_qbuf_pool.inited) {
        return;
    }

    (void)pthread_spin_lock(&g_tiny_tls_stats_lock);
    urpc_list_remove(&g_thread_tiny_cache.tls_node);
    (void)pthread_spin_unlock(&g_tiny_tls_stats_lock);

    local_block_pool_t *local_pool = get_tiny_thread_cache();
    if (local_pool->head_with_data.first != NULL) {
        tiny_return_to_global(local_pool, 0);
    }
    qbuf_tls_capacity_sub(&local_pool->capacity_with_data, &g_tiny_total_local_cap_with_data,
        local_pool->capacity_with_data);
    g_thread_tiny_cache.inited = false;
}

static void tiny_buf_init_split(char *data_buffer, char *header_buffer, uint64_t blk_num, uint32_t block_size)
{
    for (uint64_t i = 0; i < blk_num; i++) {
        umq_buf_t *buf = id_to_buf_with_data_split(header_buffer, i);
        buf->umqh = UMQ_INVALID_HANDLE;
        buf->buf_size = block_size + (uint32_t)sizeof(umq_buf_t);
        buf->data_size = block_size;
        buf->total_data_size = buf->data_size;
        buf->headroom_size = 0;
        buf->buf_data = data_buffer + i * block_size;
        buf->mempool_without_data = 0;
        buf->mempool_id = TINY_QBUF_POOL_MEMPOOL_ID;
        buf->alloc_state = QBUF_ALLOC_STATE_FREE;
        (void)memset(buf->qbuf_ext, 0, sizeof(buf->qbuf_ext));
        QBUF_LIST_INSERT_HEAD(&g_tiny_qbuf_pool.block_pool.head_with_data, buf);
    }
}

static void tiny_buf_init_combine(char *data_buffer, uint64_t blk_num, uint32_t block_size)
{
    for (uint64_t i = 0; i < blk_num; i++) {
        umq_buf_t *buf = id_to_buf_combine(data_buffer, i, block_size);
        buf->umqh = UMQ_INVALID_HANDLE;
        buf->buf_size = block_size;
        buf->data_size = block_size - (uint32_t)sizeof(umq_buf_t);
        buf->total_data_size = buf->data_size;
        buf->headroom_size = 0;
        buf->buf_data = (char *)buf + sizeof(umq_buf_t);
        buf->mempool_without_data = 0;
        buf->mempool_id = TINY_QBUF_POOL_MEMPOOL_ID;
        buf->alloc_state = QBUF_ALLOC_STATE_FREE;
        (void)memset(buf->qbuf_ext, 0, sizeof(buf->qbuf_ext));
        QBUF_LIST_INSERT_HEAD(&g_tiny_qbuf_pool.block_pool.head_with_data, buf);
    }
}

int umq_tiny_qbuf_pool_init(qbuf_pool_cfg_t *cfg)
{
    if (g_tiny_qbuf_pool.inited) {
        UMQ_VLOG_INFO(VLOG_UMQ, "tiny qbuf pool has already been inited\n");
        return -UMQ_ERR_EEXIST;
    }
    if (cfg == NULL || cfg->buf_addr == NULL || cfg->total_size == 0) {
        UMQ_VLOG_ERR(VLOG_UMQ, "tiny qbuf pool cfg invalid\n");
        return -UMQ_ERR_EINVAL;
    }

    int ret = umq_qbuf_block_pool_init(&g_tiny_qbuf_pool.block_pool);
    if (ret != UMQ_SUCCESS) {
        return ret;
    }

    g_tiny_qbuf_pool.data_buffer = cfg->buf_addr;
    g_tiny_qbuf_pool.total_size = cfg->total_size;
    g_tiny_qbuf_pool.block_size = cfg->data_size == 0 ? UMQ_TINY_QBUF_BLOCK_SIZE : cfg->data_size;
    if (g_tiny_qbuf_pool.block_size <= sizeof(umq_buf_t)) {
        UMQ_VLOG_ERR(VLOG_UMQ, "tiny qbuf block size %u invalid\n", g_tiny_qbuf_pool.block_size);
        ret = -UMQ_ERR_EINVAL;
        goto BLOCK_POOL_UNINIT;
    }
    g_tiny_qbuf_pool.headroom_size = cfg->headroom_size;
    g_tiny_qbuf_pool.tls_qbuf_pool_depth =
        cfg->tls_qbuf_pool_depth == 0 ? TINY_QBUF_POOL_TLS_MAX : cfg->tls_qbuf_pool_depth;
    g_tiny_qbuf_pool.tls_expand_qbuf_pool_depth = cfg->tls_expand_qbuf_pool_depth == 0 ?
        umq_tiny_qbuf_pool_expand_max(g_tiny_qbuf_pool.tls_qbuf_pool_depth) : cfg->tls_expand_qbuf_pool_depth;
    g_tiny_qbuf_pool.mode = cfg->mode;

    uint64_t blk_num;
    if (cfg->mode == UMQ_BUF_SPLIT) {
        blk_num = cfg->total_size / (g_tiny_qbuf_pool.block_size + sizeof(umq_buf_t));
        g_tiny_qbuf_pool.header_buffer = (char *)cfg->buf_addr + blk_num * g_tiny_qbuf_pool.block_size;
        tiny_buf_init_split((char *)g_tiny_qbuf_pool.data_buffer, (char *)g_tiny_qbuf_pool.header_buffer, blk_num,
            g_tiny_qbuf_pool.block_size);
    } else if (cfg->mode == UMQ_BUF_COMBINE) {
        blk_num = cfg->total_size / g_tiny_qbuf_pool.block_size;
        g_tiny_qbuf_pool.header_buffer = NULL;
        tiny_buf_init_combine((char *)g_tiny_qbuf_pool.data_buffer, blk_num, g_tiny_qbuf_pool.block_size);
    } else {
        ret = -UMQ_ERR_EINVAL;
        goto BLOCK_POOL_UNINIT;
    }
    g_tiny_qbuf_pool.total_block_num = blk_num;
    g_tiny_qbuf_pool.block_pool.buf_cnt_with_data = blk_num;
    g_tiny_qbuf_pool.block_pool.buf_cnt_without_data = 0;
    (void)pthread_spin_init(&g_tiny_tls_stats_lock, PTHREAD_PROCESS_PRIVATE);
    urpc_list_init(&g_tiny_tls_register_head);
    g_tiny_qbuf_pool.inited = true;
    return UMQ_SUCCESS;

BLOCK_POOL_UNINIT:
    umq_qbuf_block_pool_uninit(&g_tiny_qbuf_pool.block_pool);
    return ret;
}

void umq_tiny_qbuf_pool_uninit(void)
{
    if (!g_tiny_qbuf_pool.inited) {
        return;
    }

    release_tiny_thread_cache(0);
    (void)pthread_spin_destroy(&g_tiny_tls_stats_lock);
    umq_qbuf_block_pool_uninit(&g_tiny_qbuf_pool.block_pool);
    memset(&g_tiny_qbuf_pool, 0, sizeof(g_tiny_qbuf_pool));
}

int umq_tiny_qbuf_alloc(uint32_t request_size, uint32_t num, umq_alloc_option_t *option, umq_buf_list_t *list)
{
    if (!g_tiny_qbuf_pool.inited) {
        UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "tiny qbuf pool has not been inited\n");
        return -UMQ_ERR_ENOMEM;
    }
    if (request_size == 0 || num == 0) {
        return -UMQ_ERR_EINVAL;
    }

    local_block_pool_t *local_pool = get_tiny_thread_cache();
    bool has_headroom = (option != NULL && (option->flag & UMQ_ALLOC_FLAG_HEAD_ROOM_SIZE) != 0);
    qbuf_alloc_param_t param = {
        .headroom_size = has_headroom ? option->headroom_size : g_tiny_qbuf_pool.headroom_size,
        .shm = false,
    };

    if (g_tiny_qbuf_pool.mode == UMQ_BUF_SPLIT) {
        param.actual_buf_count =
            num * ((request_size + param.headroom_size + g_tiny_qbuf_pool.block_size - 1) /
            g_tiny_qbuf_pool.block_size);
    } else {
        uint32_t align_size = g_tiny_qbuf_pool.block_size - sizeof(umq_buf_t);
        param.actual_buf_count = num * ((request_size + param.headroom_size + align_size - 1) / align_size);
    }

    if (local_pool->buf_cnt_with_data < param.actual_buf_count) {
        int ret = tiny_local_pool_fetch(param.actual_buf_count - (uint32_t)local_pool->buf_cnt_with_data, local_pool);
        if (ret != UMQ_SUCCESS) {
            return ret;
        }
    }

    if (g_tiny_qbuf_pool.mode == UMQ_BUF_SPLIT) {
        umq_qbuf_alloc_data_with_split(local_pool, request_size, &param, list, g_tiny_qbuf_pool.block_size);
    } else {
        umq_qbuf_alloc_data_with_combine(local_pool, request_size, &param, list, g_tiny_qbuf_pool.block_size);
    }
    tiny_thread_cache_self_shrink(local_pool);
    g_thread_tiny_cache.stats.alloc_cnt_with_data += param.actual_buf_count;
    return UMQ_SUCCESS;
}

void umq_tiny_qbuf_free(umq_buf_list_t *list)
{
    if (!g_tiny_qbuf_pool.inited || list == NULL || QBUF_LIST_FIRST(list) == NULL) {
        return;
    }

    local_block_pool_t *local_pool = get_tiny_thread_cache();
    uint32_t cnt = tiny_release_batch(list, &local_pool->head_with_data);
    local_pool->buf_cnt_with_data += cnt;
    uint32_t cap = (uint32_t)local_pool->capacity_with_data;
    if (local_pool->buf_cnt_with_data > cap) {
        uint32_t threshold = cap > TINY_QBUF_POOL_BATCH_CNT ? cap - TINY_QBUF_POOL_BATCH_CNT : 0;
        tiny_return_to_global(local_pool, threshold);
        g_thread_tiny_cache.stats.tls_return_cnt_with_data++;
    }
    g_thread_tiny_cache.stats.free_cnt_with_data += cnt;
}

int umq_tiny_qbuf_headroom_reset(umq_buf_t *qbuf, uint16_t headroom_size)
{
    if (!g_tiny_qbuf_pool.inited) {
        return -UMQ_ERR_ENOMEM;
    }
    return headroom_reset(qbuf, headroom_size, g_tiny_qbuf_pool.mode, g_tiny_qbuf_pool.block_size);
}

umq_buf_t *umq_tiny_qbuf_data_to_head(void *data)
{
    if (!g_tiny_qbuf_pool.inited || data == NULL) {
        return NULL;
    }

    if (g_tiny_qbuf_pool.mode == UMQ_BUF_SPLIT) {
        if (data < g_tiny_qbuf_pool.data_buffer || data >= g_tiny_qbuf_pool.header_buffer) {
            return NULL;
        }
        uint64_t id = ((uint64_t)(uintptr_t)data - (uint64_t)(uintptr_t)g_tiny_qbuf_pool.data_buffer) /
            g_tiny_qbuf_pool.block_size;
        return (umq_buf_t *)((char *)g_tiny_qbuf_pool.header_buffer + id * sizeof(umq_buf_t));
    }

    if (data < g_tiny_qbuf_pool.data_buffer ||
        data >= (void *)((char *)g_tiny_qbuf_pool.data_buffer + g_tiny_qbuf_pool.total_size)) {
        return NULL;
    }
    uint64_t id = ((uint64_t)(uintptr_t)data - (uint64_t)(uintptr_t)g_tiny_qbuf_pool.data_buffer) /
        g_tiny_qbuf_pool.block_size;
    return (umq_buf_t *)((char *)g_tiny_qbuf_pool.data_buffer + id * g_tiny_qbuf_pool.block_size);
}

int umq_tiny_qbuf_pool_info_get(umq_qbuf_pool_stats_t *qbuf_pool_stats)
{
    if (!g_tiny_qbuf_pool.inited) {
        return UMQ_SUCCESS;
    }
    if (qbuf_pool_stats->num >= UMQ_STATS_QBUF_POOL_TYPE_MAX) {
        return -UMQ_ERR_EINVAL;
    }

    umq_qbuf_pool_info_t *info = &qbuf_pool_stats->qbuf_pool_info[qbuf_pool_stats->num];
    info->mode = g_tiny_qbuf_pool.mode;
    info->total_size = g_tiny_qbuf_pool.total_size;
    info->total_block_num = g_tiny_qbuf_pool.total_block_num;
    info->block_size = g_tiny_qbuf_pool.block_size;
    info->headroom_size = g_tiny_qbuf_pool.headroom_size;
    info->umq_buf_t_size = (uint32_t)sizeof(umq_buf_t);
    if (g_tiny_qbuf_pool.mode == UMQ_BUF_SPLIT) {
        info->data_size = g_tiny_qbuf_pool.block_size;
        info->buf_size = g_tiny_qbuf_pool.block_size + (uint32_t)sizeof(umq_buf_t);
        info->available_mem.split.block_num_with_data = g_tiny_qbuf_pool.block_pool.buf_cnt_with_data;
        info->available_mem.split.size_with_data =
            g_tiny_qbuf_pool.block_pool.buf_cnt_with_data * (g_tiny_qbuf_pool.block_size + sizeof(umq_buf_t));
        info->available_mem.split.block_num_without_data = 0;
        info->available_mem.split.size_without_data = 0;
    } else {
        info->data_size = g_tiny_qbuf_pool.block_size - (uint32_t)sizeof(umq_buf_t);
        info->buf_size = g_tiny_qbuf_pool.block_size;
        info->available_mem.combine.block_num_with_data = g_tiny_qbuf_pool.block_pool.buf_cnt_with_data;
        info->available_mem.combine.size_with_data =
            g_tiny_qbuf_pool.block_pool.buf_cnt_with_data * g_tiny_qbuf_pool.block_size;
    }
    qbuf_pool_stats->num++;

    (void)pthread_spin_lock(&g_tiny_tls_stats_lock);
    local_tiny_qbuf_pool_t *pool_iter = NULL;
    URPC_LIST_FOR_EACH(pool_iter, tls_node, &g_tiny_tls_register_head) {
        if (qbuf_pool_stats->local_qbuf_pool_num >= UMQ_LOCAL_QBUF_POOL_MAX_NUM) {
            break;
        }
        umq_local_qbuf_pool_stats_t *s =
            &qbuf_pool_stats->local_qbuf_pool_stats[qbuf_pool_stats->local_qbuf_pool_num];
        (void)memset(s, 0, sizeof(*s));
        s->tid = pool_iter->stats.tid;
        s->capacity_with_data = pool_iter->block_pool.capacity_with_data;
        s->buf_cnt_with_data = pool_iter->block_pool.buf_cnt_with_data;
        s->tls_fetch_cnt_with_data = pool_iter->stats.tls_fetch_cnt_with_data;
        s->tls_fetch_buf_cnt_with_data = pool_iter->stats.tls_fetch_buf_cnt_with_data;
        s->tls_return_cnt_with_data = pool_iter->stats.tls_return_cnt_with_data;
        s->tls_return_buf_cnt_with_data = pool_iter->stats.tls_return_buf_cnt_with_data;
        s->alloc_cnt_with_data = pool_iter->stats.alloc_cnt_with_data;
        s->free_cnt_with_data = pool_iter->stats.free_cnt_with_data;
        qbuf_pool_stats->local_qbuf_pool_num++;
    }
    (void)pthread_spin_unlock(&g_tiny_tls_stats_lock);
    return UMQ_SUCCESS;
}

int umq_tiny_qbuf_register_seg(uint8_t *ctx, mempool_segment_ops_t *ops)
{
    if (!g_tiny_qbuf_pool.inited || ops == NULL || ops->register_seg_callback == NULL) {
        return -UMQ_ERR_EINVAL;
    }
    return ops->register_seg_callback(ctx, TINY_QBUF_POOL_MEMPOOL_ID, g_tiny_qbuf_pool.data_buffer,
        g_tiny_qbuf_pool.total_size);
}

void umq_tiny_qbuf_unregister_seg(uint8_t *ctx, mempool_segment_ops_t *ops)
{
    if (!g_tiny_qbuf_pool.inited || ops == NULL || ops->unregister_seg_callback == NULL) {
        return;
    }
    ops->unregister_seg_callback(ctx, TINY_QBUF_POOL_MEMPOOL_ID);
}
