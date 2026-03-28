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
#include "urpc_thread_closure.h"
#include "urpc_util.h"
#include "urpc_list.h"
#include "umq_qbuf_pool.h"

#define QBUF_POOL_TLS_TOTAL_SIZE          (16384)   // T: all thread-local pool capacity sum budget (per type)
#define QBUF_POOL_EXPAND_MAX(__total_size) ((__total_size) * 7 / 8) // M = T*7/8, single thread capacity cap
#define QBUF_POOL_SHRINK_THRESHOLD        (64)      // self-driven shrink threshold: N/4 >= this value (N >= 256)
#define QBUF_POOL_SELF_SHRINK_RATIO       (4)       // adaptive shrink ratio(1/4)
#define QBUF_POOL_SHRINK_IDLE_MS          (1000)    // idle threshold for global shrink trigger (1s)
#define QBUF_POOL_SHRINK_CLEAR_THRESHOLD  (64)      // clear to zero if capacity below this

typedef struct local_qbuf_pool {
    bool inited;
    local_block_pool_t block_pool;
    urpc_list_t registry_node;          // global registry doubly-linked list node
} local_qbuf_pool_t;

typedef struct local_qbuf_pool_cfg {
    uint64_t tls_total_size;
    uint64_t tls_expand_size_max;
} local_qbuf_pool_cfg_t;

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
    local_qbuf_pool_cfg_t local_pool_cfg;
} qbuf_pool_t;

static qbuf_pool_t g_qbuf_pool = {0};
static __thread local_qbuf_pool_t g_thread_cache = {0};
static uint8_t g_umq_qbuf_size_pow_samll = UMQ_QBUF_SIZE_POW_8K;

// --- global registry and capacity counters for elastic scaling ---
static pthread_rwlock_t g_tls_registry_rwlock = PTHREAD_RWLOCK_INITIALIZER;
static urpc_list_t g_tls_registry_head;
static volatile uint64_t g_total_local_cap_with_data = 0;     // sum of all threads' capacity_with_data
static volatile uint64_t g_total_local_cap_without_data = 0;  // sum of all threads' capacity_without_data

static void *g_buffer_addr = NULL;
static uint64_t g_total_len = 0;

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
        (void)pthread_spin_init(&g_thread_cache.block_pool.pool_lock, PTHREAD_PROCESS_PRIVATE);
        g_thread_cache.block_pool.capacity_with_data = 0;
        g_thread_cache.block_pool.capacity_without_data = 0;
        g_thread_cache.block_pool.last_used_with_data = get_timestamp_ms();
        g_thread_cache.block_pool.last_used_without_data = get_timestamp_ms();

        pthread_rwlock_wrlock(&g_tls_registry_rwlock);
        urpc_list_push_back(&g_tls_registry_head, &g_thread_cache.registry_node);
        pthread_rwlock_unlock(&g_tls_registry_rwlock);

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
    if (!g_thread_cache.inited) {
        return;
    }

    // Step 1: wrlock to wait for all ongoing execute_global_shrink to complete
    pthread_rwlock_wrlock(&g_tls_registry_rwlock);
    // Step 2: remove from linked list -- execute_global_shrink will no longer iterate to this pool
    urpc_list_remove(&g_thread_cache.registry_node);
    pthread_rwlock_unlock(&g_tls_registry_rwlock);

    // Step 3: return bufs to global pool -- check if global pool is initialized
    local_block_pool_t *local_pool = &g_thread_cache.block_pool;
    if (g_qbuf_pool.inited) {
        (void)pthread_mutex_lock(&g_qbuf_pool.block_pool.global_mutex);
        if (local_pool->head_with_data.first != NULL) {
            // release thread cache no need check double free
            uint32_t cnt = release_to_global(&local_pool->head_with_data,
                &g_qbuf_pool.block_pool.head_with_data);
            local_pool->buf_cnt_with_data += cnt;
        }
        if (local_pool->head_without_data.first != NULL) {
            // release thread cache no need check double free
            uint32_t cnt = release_to_global(&local_pool->head_without_data,
                &g_qbuf_pool.block_pool.head_without_data);
            local_pool->buf_cnt_without_data += cnt;
        }
        (void)pthread_mutex_unlock(&g_qbuf_pool.block_pool.global_mutex);

        __atomic_fetch_sub(&g_total_local_cap_with_data, local_pool->capacity_with_data, __ATOMIC_RELAXED);
        __atomic_fetch_sub(&g_total_local_cap_without_data, local_pool->capacity_without_data, __ATOMIC_RELAXED);
        
    }

    // Step 4: destroy spinlock
    (void)pthread_spin_destroy(&local_pool->pool_lock);
    g_thread_cache.inited = false;
}

// return_n_to_global_lock_free: caller ALREADY holds global_mutex (non-reentrant)
static ALWAYS_INLINE void return_n_to_global_lock_free(
    global_block_pool_t *global_pool, local_block_pool_t *cache_pool,
    bool with_data, uint32_t return_count)
{
    if (return_count == 0) {
        return;
    }

    uint64_t *global_buf_cnt;
    umq_buf_list_t *global_head;
    uint64_t *local_buf_cnt;
    umq_buf_list_t *local_head;

    if (with_data) {
        global_buf_cnt = &global_pool->buf_cnt_with_data;
        global_head = &global_pool->head_with_data;
        local_buf_cnt = &cache_pool->buf_cnt_with_data;
        local_head = &cache_pool->head_with_data;
    } else {
        global_buf_cnt = &global_pool->buf_cnt_without_data;
        global_head = &global_pool->head_without_data;
        local_buf_cnt = &cache_pool->buf_cnt_without_data;
        local_head = &cache_pool->head_without_data;
    }

    uint32_t cnt = 0;
    umq_buf_t *cur_node = NULL;
    umq_buf_t *switch_node = NULL;
    QBUF_LIST_FOR_EACH(cur_node, local_head) {
        if (++cnt == return_count) {
            switch_node = cur_node;
            break;
        }
    }

    if (switch_node == NULL) {
        return;
    }

    umq_buf_t *head = QBUF_LIST_FIRST(global_head);
    QBUF_LIST_FIRST(global_head) = QBUF_LIST_FIRST(local_head);
    QBUF_LIST_FIRST(local_head) = QBUF_LIST_NEXT(switch_node);
    QBUF_LIST_NEXT(switch_node) = head;
    *global_buf_cnt += cnt;
    *local_buf_cnt -= cnt;
}

// return_n_to_global: caller does NOT hold global_mutex
static ALWAYS_INLINE void return_n_to_global(
    global_block_pool_t *global_pool, local_block_pool_t *cache_pool, bool with_data, uint32_t return_count)
{
    (void)pthread_mutex_lock(&global_pool->global_mutex);
    return_n_to_global_lock_free(global_pool, cache_pool, with_data, return_count);
    (void)pthread_mutex_unlock(&global_pool->global_mutex);
}

// shrink_one_type: shrink a specific pool's capacity for a given type
// caller holds pool->pool_lock
static ALWAYS_INLINE void shrink_one_type(local_qbuf_pool_t *pool, bool with_data)
{
    uint64_t now = get_timestamp_ms();
    uint64_t *cap;
    uint64_t buf_cnt;
    volatile uint64_t *g_total;
    uint64_t last_used_time;
    local_block_pool_t *local_pool = &pool->block_pool;

    if (with_data) {
        cap = &local_pool->capacity_with_data;
        buf_cnt = local_pool->buf_cnt_with_data;
        last_used_time = local_pool->buf_cnt_with_data;
        g_total = &g_total_local_cap_with_data;
    } else {
        cap = &local_pool->capacity_without_data;
        buf_cnt = local_pool->buf_cnt_without_data;
        last_used_time = local_pool->buf_cnt_without_data;
        g_total = &g_total_local_cap_without_data;
    }

    if ((*cap) == 0 || now - last_used_time >= QBUF_POOL_SHRINK_IDLE_MS) {
        return;
    }

    uint32_t shrink = 0;
    if ((*cap) <= QBUF_POOL_SHRINK_CLEAR_THRESHOLD) {
        shrink = *cap;
    } else {
        shrink = (*cap) / 2;
    }
    *cap -= shrink;

    if (buf_cnt > (*cap)) {
        return_to_global_lock_free(&g_qbuf_pool.block_pool, local_pool, with_data, (*cap));
    }

    if (shrink > 0) {
        __atomic_fetch_sub(g_total, shrink, __ATOMIC_RELAXED);
    }
}

int umq_qbuf_pool_init(qbuf_pool_cfg_t *cfg)
{
    if (g_qbuf_pool.inited) {
        UMQ_VLOG_INFO(VLOG_UMQ, "qbuf pool has already been inited\n");
        return -UMQ_ERR_EEXIST;
    }

    urpc_list_init(&g_tls_registry_head);
    umq_qbuf_block_pool_init(&g_qbuf_pool.block_pool);
    g_qbuf_pool.mode = cfg->mode;
    g_qbuf_pool.total_size = cfg->total_size;
    g_qbuf_pool.headroom_size = cfg->headroom_size;
    g_qbuf_pool.data_size = cfg->data_size;
    g_qbuf_pool.local_pool_cfg.tls_total_size =
        (cfg->tls_total_size == 0) ? QBUF_POOL_TLS_TOTAL_SIZE : cfg->tls_total_size;
    g_qbuf_pool.local_pool_cfg.tls_expand_size_max = QBUF_POOL_EXPAND_MAX(g_qbuf_pool.local_pool_cfg.tls_total_size);

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

void umq_qbuf_pool_uninit(void)
{
    if (!g_qbuf_pool.inited) {
        return;
    }
    release_thread_cache(0);
    umq_qbuf_block_pool_uninit(&g_qbuf_pool.block_pool);
    memset(&g_qbuf_pool, 0, sizeof(qbuf_pool_t));
    urpc_list_init(&g_tls_registry_head);
    __atomic_store_n(&g_total_local_cap_with_data, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_total_local_cap_without_data, 0, __ATOMIC_RELAXED);
}

// umq_qbuf_local_pool_fetch_and_expand: expand local pool capacity and fetch bufs from global
// *buf_cnt is updated to the new local buf count after fetch
// Returns: 0 on success, negative error code on failure
static ALWAYS_INLINE int umq_qbuf_local_pool_fetch_and_expand(
    uint32_t needed, local_block_pool_t *local_pool, bool with_data)
{
    uint64_t *cap;
    uint64_t *buf_cnt;
    volatile uint64_t *g_total;
    if (with_data) {
        cap = &local_pool->capacity_with_data;
        buf_cnt = &local_pool->buf_cnt_with_data;
        g_total = &g_total_local_cap_with_data;
        
    } else {
        cap = &local_pool->capacity_without_data;
        buf_cnt = &local_pool->buf_cnt_without_data;
        g_total = &g_total_local_cap_without_data;
    }

    uint32_t fetch_count = needed - *buf_cnt;
    uint32_t grow = fetch_count;

    if ((*cap) + grow > g_qbuf_pool.local_pool_cfg.tls_expand_size_max) {
        grow = ((*cap) >= g_qbuf_pool.local_pool_cfg.tls_expand_size_max) ?
            0 : (g_qbuf_pool.local_pool_cfg.tls_expand_size_max - (*cap));
    }
    if ((*g_total) + grow > g_qbuf_pool.local_pool_cfg.tls_total_size) {
        grow = ((*g_total) >= g_qbuf_pool.local_pool_cfg.tls_total_size) ?
            0 : ((uint32_t)(g_qbuf_pool.local_pool_cfg.tls_total_size - (*g_total)));
    }

    int ret = fetch_from_global(&g_qbuf_pool.block_pool, local_pool, with_data, fetch_count);
    if (ret <= 0) {
        return ret;
    }

    if (grow > 0) {
        *cap = (*cap) + grow;
        __atomic_fetch_add(g_total, grow, __ATOMIC_RELAXED);
    }
    return UMQ_SUCCESS;
}

// thread_cache_self_shrink: self-driven shrink after successful alloc
// N = remaining buf count after alloc; if N/4 >= SHRINK_THRESHOLD, shrink capacity by N/4
static ALWAYS_INLINE void thread_cache_self_shrink(bool with_data)
{
    uint64_t remaining;
    uint64_t *cap_ptr;
    volatile uint64_t *g_total;

    if (with_data) {
        remaining = (uint32_t)g_thread_cache.block_pool.buf_cnt_with_data;
        cap_ptr = &g_thread_cache.block_pool.capacity_with_data;
        g_total = &g_total_local_cap_with_data;
    } else {
        remaining = (uint32_t)g_thread_cache.block_pool.buf_cnt_without_data;
        cap_ptr = &g_thread_cache.block_pool.capacity_without_data;
        g_total = &g_total_local_cap_without_data;
    }

    uint64_t shrink = remaining / QBUF_POOL_SELF_SHRINK_RATIO;
    if (shrink < QBUF_POOL_SHRINK_THRESHOLD || (*cap_ptr) == 0) {
        return;
    }
    *cap_ptr = (*cap_ptr) - shrink;
    __atomic_fetch_sub(g_total, shrink, __ATOMIC_RELAXED);
}

int umq_qbuf_alloc(uint32_t request_size, uint32_t num, umq_alloc_option_t *option, umq_buf_list_t *list)
{
    if (!g_qbuf_pool.inited) {
        UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "qbuf pool has not been inited\n");
        return -UMQ_ERR_ENOMEM;
    }

    local_block_pool_t *local_pool = get_thread_cache();
    (void)pthread_spin_lock(&local_pool->pool_lock);

    bool flag = (option != NULL && (option->flag & UMQ_ALLOC_FLAG_HEAD_ROOM_SIZE) != 0);
    qbuf_alloc_param_t param;
    param.shm = false;
    param.headroom_size = flag ? option->headroom_size : g_qbuf_pool.headroom_size;

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
            (void)pthread_spin_unlock(&local_pool->pool_lock);
            return -UMQ_ERR_EINVAL;
        }

        if (g_qbuf_pool.mode != UMQ_BUF_SPLIT) {
            UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "cannot alloc memory size 0 in combine mode\n");
            (void)pthread_spin_unlock(&local_pool->pool_lock);
            return -UMQ_ERR_ENOMEM;
        }

        if (local_pool->buf_cnt_without_data < num) {
            int ret = umq_qbuf_local_pool_fetch_and_expand(num - local_pool->buf_cnt_without_data, local_pool, false);
            if (ret != UMQ_SUCCESS) {
                UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "umq qbuf local pool fetch and expand failed, ret: %d\n", ret);
                return ret;
            }
        }

        umq_qbuf_alloc_nodata(local_pool, num, list, param.shm);
        thread_cache_self_shrink(false);

        local_pool->last_used_without_data = get_timestamp_ms();
        (void)pthread_spin_unlock(&local_pool->pool_lock);
        return 0;
    }

    uint32_t needed = param.actual_buf_count;
    uint32_t buf_cnt = (uint32_t)local_pool->buf_cnt_with_data;

    if (buf_cnt < needed) {
        int ret = umq_qbuf_local_pool_fetch_and_expand(needed, local_pool, true);
        if (ret != UMQ_SUCCESS) {
            UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "umq qbuf local pool fetch and expand failed, ret: %d\n", ret);
            return ret;
        }
    }

    if (g_qbuf_pool.mode == UMQ_BUF_SPLIT) {
        umq_qbuf_alloc_data_with_split(local_pool, request_size, &param, list);
    } else {
        umq_qbuf_alloc_data_with_combine(local_pool, request_size, &param, list);
    }

    thread_cache_self_shrink(true);

    local_pool->last_used_with_data = get_timestamp_ms();
    (void)pthread_spin_unlock(&local_pool->pool_lock);
    return UMQ_SUCCESS;
}

void umq_qbuf_free(umq_buf_list_t *list)
{
    if (!g_qbuf_pool.inited) {
        UMQ_LIMIT_VLOG_ERR(VLOG_UMQ, "qbuf pool has not been inited\n");
        return;
    }

    local_block_pool_t *local_pool = get_thread_cache();
    (void)pthread_spin_lock(&local_pool->pool_lock);
    
    // split mode and buf is in head no data zone
    if (g_qbuf_pool.mode == UMQ_BUF_SPLIT && (void *)QBUF_LIST_FIRST(list) >= g_qbuf_pool.ext_header_buffer) {
        uint32_t cnt = release_batch(list, &local_pool->head_without_data, false);
        local_pool->buf_cnt_without_data += cnt;

        uint32_t cap = local_pool->capacity_without_data;
        if (local_pool->buf_cnt_without_data > cap) {
            return_to_global(&g_qbuf_pool.block_pool, local_pool, false, cap);
        }

        local_pool->last_used_without_data = get_timestamp_ms();
        (void)pthread_spin_unlock(&local_pool->pool_lock);
        return;
    }

    uint32_t cnt = release_batch(list, &local_pool->head_with_data, false);
    local_pool->buf_cnt_with_data += cnt;

    uint32_t cap = local_pool->capacity_with_data;
    uint32_t buf_cnt = (uint32_t)local_pool->buf_cnt_with_data;
    if (buf_cnt > cap) {
        uint32_t excess = buf_cnt - cap;
        return_n_to_global(&g_qbuf_pool.block_pool, local_pool, true, excess);
    }

    local_pool->last_used_with_data = get_timestamp_ms();
    (void)pthread_spin_unlock(&local_pool->pool_lock);
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
    return register_seg_func(ctx, UMQ_QBUF_DEFAULT_MEMPOOL_ID, g_qbuf_pool.data_buffer,  g_qbuf_pool.total_size);
}

int umq_qbuf_unregister_seg(uint8_t *ctx, unregister_seg_callback_t unregister_seg_func)
{
    return unregister_seg_func(ctx, UMQ_QBUF_DEFAULT_MEMPOOL_ID);
}

// execute_global_shrink: called from fetch_from_global with global_mutex already held
void execute_global_shrink(bool with_data)
{
    (void)pthread_rwlock_rdlock(&g_tls_registry_rwlock);
    local_qbuf_pool_t *pool = NULL;
    URPC_LIST_FOR_EACH(pool, registry_node, &g_tls_registry_head) {
        if (pool == &g_thread_cache) {
            continue;
        }
        if (pthread_spin_trylock(&pool->block_pool.pool_lock) != 0) {
            continue;
        }
        shrink_one_type(pool, with_data);
        (void)pthread_spin_unlock(&pool->block_pool.pool_lock);
    }
    (void)pthread_rwlock_unlock(&g_tls_registry_rwlock);
}