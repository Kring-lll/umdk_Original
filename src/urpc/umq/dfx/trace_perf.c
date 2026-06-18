/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: umq trace perf
 * Create: 2026-6-18
 */

#include <pthread.h>
#include <stdarg.h>

#include "umq_errno.h"
#include "umq_vlog.h"
#include "urpc_timer.h"
#include "urpc_thread_closure.h"
#include "urpc_util.h"
#include "perf.h"

#define DATA_RECORD_NUM 1000000
#define DATA_RECORD_LIMIT 5000

/*
 * Thread-local trace state — one per thread, no lock needed.
 *
 * g_trace_index:    slot index into g_umq_trace_ctx->data_table[], allocated once
 *                   by umq_trace_alloc() via pthread_once.  Persists for the
 *                   lifetime of the thread; all trace records for this thread
 *                   live in that single slot.
 *
 * g_trace_active_id: ring-buffer index of the currently-open record, set by
 *                    umq_trace_start_record() and cleared to -1 by
 *                    umq_trace_end_record().  While >= 0, any call to
 *                    umq_trace_sub_record() or umq_trace_item_record() will
 *                    append to that record — no need to pass an explicit id
 *                    through intermediate call stacks.
 *
 *                    When == -1, there is no active record; sub_write /
 *                    data_write are immediate no-ops.  This is the
 *                    mechanism by which helper functions (e.g. the FC poll
 *                    functions, shared-credit send) automatically skip
 *                    tracing when called from outside a traced context.
 */
static __thread uint32_t g_trace_index = -1;
static __thread int32_t g_trace_active_id = -1;
static __thread pthread_once_t g_dp_thread_run_once = PTHREAD_ONCE_INIT;
static uint32_t g_umq_output_record_limit = DATA_RECORD_LIMIT;
static uint32_t g_umq_data_record_num = DATA_RECORD_NUM;
static bool g_umq_trace_enable = false;
static pthread_spinlock_t g_umq_trace_lock;

/* unified trace — single ring buffer for all types, one per thread */
typedef struct umq_trace_buf {
    uint32_t record_cnt;
    uint32_t pre_output_cnt;
    umq_data_record_t *data_record;    /* dynamically allocated, size = g_umq_data_record_num */
    bool is_used;
} umq_trace_buf_t;

typedef struct umq_trace_ctx {
    pthread_once_t *dp_thread_run_once[UMQ_PERF_REC_MAX_NUM];
    umq_trace_buf_t data_table[UMQ_PERF_REC_MAX_NUM];
} umq_trace_ctx_t;

static umq_trace_ctx_t *g_umq_trace_ctx;

int umq_trace_init(umq_trace_cfg_t *cfg)
{
    if (g_umq_trace_ctx != NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "umq perf has been inited\n");
        return -UMQ_ERR_EEXIST;
    }
    if (cfg->flag & UMQ_PERF_FLAG_RECORD_NUM) {
        if (cfg->data_record_num == 0) {
            UMQ_VLOG_ERR(VLOG_UMQ, "data_record_num must be non-zero\n");
            return -UMQ_ERR_EINVAL;
        }
        g_umq_data_record_num = cfg->data_record_num;
    }
    if (cfg->flag & UMQ_PERF_FLAG_RECORD_LIMIT) {
        g_umq_output_record_limit = cfg->output_record_limit;
    }
    (void)pthread_spin_init(&g_umq_trace_lock, PTHREAD_PROCESS_PRIVATE);

    g_umq_trace_ctx = (umq_trace_ctx_t *)calloc(1, sizeof(umq_trace_ctx_t));
    if (g_umq_trace_ctx == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "calloc for umq_trace failed\n");
        return -UMQ_ERR_ENOMEM;
    }
    return UMQ_SUCCESS;
}

void umq_trace_uninit(void)
{
    if (g_umq_trace_ctx == NULL) {
        return;
    }

    (void)pthread_spin_lock(&g_umq_trace_lock);
    for (uint32_t i = 0; i < UMQ_PERF_REC_MAX_NUM; i++) {
        if (g_umq_trace_ctx->dp_thread_run_once[i] != NULL) {
            *g_umq_trace_ctx->dp_thread_run_once[i] = PTHREAD_ONCE_INIT;
        }
        if (g_umq_trace_ctx->data_table[i].data_record != NULL) {
            free(g_umq_trace_ctx->data_table[i].data_record);
            g_umq_trace_ctx->data_table[i].data_record = NULL;
        }
    }

    g_umq_trace_enable = false;
    free(g_umq_trace_ctx);
    g_umq_trace_ctx = NULL;
    (void)pthread_spin_unlock(&g_umq_trace_lock);
    (void)pthread_spin_destroy(&g_umq_trace_lock);
}


static void umq_trace_closure(uint64_t idx)
{
    (void)pthread_spin_lock(&g_umq_trace_lock);
    if (g_umq_trace_ctx == NULL) {
        (void)pthread_spin_unlock(&g_umq_trace_lock);
        return;
    }
    g_umq_trace_ctx->data_table[idx].is_used = false;
    g_umq_trace_ctx->dp_thread_run_once[idx] = NULL;
    if (g_umq_trace_ctx->data_table[idx].data_record != NULL) {
        free(g_umq_trace_ctx->data_table[idx].data_record);
        g_umq_trace_ctx->data_table[idx].data_record = NULL;
    }
    (void)pthread_spin_unlock(&g_umq_trace_lock);
}

static void umq_trace_record_clear(uint32_t idx)
{
    umq_trace_buf_t *cur_record = &g_umq_trace_ctx->data_table[idx];
    cur_record->record_cnt = 0;
    cur_record->pre_output_cnt = 0;
}

void umq_trace_alloc(void)
{
    uint32_t idx;
    (void)pthread_spin_lock(&g_umq_trace_lock);
    if (g_umq_trace_ctx == NULL) {
        (void)pthread_spin_unlock(&g_umq_trace_lock);
        UMQ_VLOG_ERR(VLOG_UMQ, "perf data ctx invalid\n");
        return;
    }

    for (idx = 0; idx < UMQ_PERF_REC_MAX_NUM; ++idx) {
        if (!g_umq_trace_ctx->data_table[idx].is_used) {
            break;
        }
    }
    if (idx == UMQ_PERF_REC_MAX_NUM) {
        (void)pthread_spin_unlock(&g_umq_trace_lock);
        UMQ_VLOG_WARN(VLOG_UMQ, "perf data table capacity %u were exhausted, alloc perf_rec failed\n",
            UMQ_PERF_REC_MAX_NUM);
        return;
    }

    /* allocate ring buffer on first use */
    if (g_umq_trace_ctx->data_table[idx].data_record == NULL) {
        g_umq_trace_ctx->data_table[idx].data_record =
            (umq_data_record_t *)calloc(g_umq_data_record_num, sizeof(umq_data_record_t));
        if (g_umq_trace_ctx->data_table[idx].data_record == NULL) {
            (void)pthread_spin_unlock(&g_umq_trace_lock);
            UMQ_VLOG_ERR(VLOG_UMQ, "calloc for data_record failed\n");
            return;
        }
    }

    umq_trace_record_clear(idx);
    g_umq_trace_ctx->data_table[idx].is_used = true;
    (void)pthread_spin_unlock(&g_umq_trace_lock);

    g_trace_index = idx;
    g_umq_trace_ctx->dp_thread_run_once[idx] = &g_dp_thread_run_once;
    urpc_thread_closure_register(THREAD_CLOSURE_UMQ_DATA_PERF, idx, umq_trace_closure);
}

static void umq_dp_thread_run_once(void)
{
    umq_trace_alloc();
}

uint64_t umq_trace_start_timestamp_get(void)
{
    if (!g_umq_trace_enable) {
        return 0;
    }
    pthread_once(&g_dp_thread_run_once, umq_dp_thread_run_once);
    return get_timestamp_ns();
}

uint64_t umq_trace_timestamp_get(void)
{
    if (!g_umq_trace_enable) {
        return 0;
    }
    return get_timestamp_ns();
}

/* ================================================================
 *  New unified record APIs
 * ================================================================ */

static ALWAYS_INLINE umq_trace_buf_t *umq_trace_get(void)
{
    if (g_umq_trace_ctx == NULL || g_trace_index >= UMQ_PERF_REC_MAX_NUM) {
        return NULL;
    }
    return &g_umq_trace_ctx->data_table[g_trace_index];
}

void umq_trace_start_record(umq_trace_type_t type, uint64_t time,
                            uint32_t jetty_id, uint32_t local_umq_id,
                            uint32_t remote_umq_id)
{
    if (!g_umq_trace_enable) {
        return;
    }

    umq_trace_buf_t *cur_rec = umq_trace_get();
    if (cur_rec == NULL || cur_rec->data_record == NULL) {
        return;
    }

    uint32_t idx = cur_rec->record_cnt % g_umq_data_record_num;
    umq_data_record_t *rec = &cur_rec->data_record[idx];

    rec->jetty_id = jetty_id;
    rec->local_umq_id = local_umq_id;
    rec->remote_umq_id = remote_umq_id;
    rec->timestamp = umq_trace_timestamp_get();
    rec->type = type;
    rec->start_time = time;
    rec->end_time = 0;
    rec->sub_time_cnt = 0;
    rec->item_cnt = 0;

    cur_rec->record_cnt++;
    g_trace_active_id = (int32_t)idx;
}

int32_t umq_trace_sub_record(umq_trace_type_t type, umq_urma_func_type_t func_type,
                             uint64_t start_time, uint64_t exec_time)
{
    if (!g_umq_trace_enable || g_trace_active_id < 0) {
        return -UMQ_ERR_EINVAL;
    }

    umq_trace_buf_t *cur_rec = umq_trace_get();
    if (cur_rec == NULL || cur_rec->data_record == NULL) {
        return -UMQ_ERR_EINVAL;
    }

    uint32_t idx = (uint32_t)g_trace_active_id;
    if (idx >= g_umq_data_record_num) {
        return -UMQ_ERR_EINVAL;
    }
    umq_data_record_t *rec = &cur_rec->data_record[idx];

    if (rec->type != type || rec->sub_time_cnt >= UMQ_PERF_MAX_SUB_TIME_NUM) {
        return -UMQ_ERR_EINVAL;
    }

    umq_sub_time_t *sub = &rec->sub_time[rec->sub_time_cnt];
    sub->start_time = start_time;
    sub->exec_time = exec_time;
    sub->func_type = func_type;
    rec->sub_time_cnt++;

    return (int32_t)(rec->sub_time_cnt - 1);
}

int32_t umq_trace_item_record(uint32_t msn, uint32_t size)
{
    if (!g_umq_trace_enable || g_trace_active_id < 0) {
        return -UMQ_ERR_EINVAL;
    }

    umq_trace_buf_t *cur_rec = umq_trace_get();
    if (cur_rec == NULL || cur_rec->data_record == NULL) {
        return -UMQ_ERR_EINVAL;
    }

    uint32_t idx = (uint32_t)g_trace_active_id;
    if (idx >= g_umq_data_record_num) {
        return -UMQ_ERR_EINVAL;
    }
    umq_data_record_t *rec = &cur_rec->data_record[idx];

    if (rec->item_cnt >= UMQ_TRACE_ITEM_NUM) {
        return -UMQ_ERR_EINVAL;
    }

    rec->items[rec->item_cnt].msn = msn;
    rec->items[rec->item_cnt].size = size;
    rec->item_cnt++;

    return (int32_t)(rec->item_cnt - 1);
}

void umq_trace_end_record(umq_trace_type_t type, uint64_t time)
{
    if (!g_umq_trace_enable || g_trace_active_id < 0) {
        return;
    }

    umq_trace_buf_t *cur_rec = umq_trace_get();
    if (cur_rec == NULL || cur_rec->data_record == NULL) {
        return;
    }

    uint32_t idx = (uint32_t)g_trace_active_id;
    if (idx >= g_umq_data_record_num) {
        return;
    }
    umq_data_record_t *rec = &cur_rec->data_record[idx];

    if (rec->type == type && rec->end_time == 0) {
        rec->end_time = time;
    }

    g_trace_active_id = -1;
}

/* ================================================================
 *  Unified output
 * ================================================================ */

static const char *umq_trace_type_str(umq_trace_type_t type)
{
    static const char *type_str[] = {
        [UMQ_TRACE_DATA_TYPE_POST]  = "POST",
        [UMQ_TRACE_DATA_TYPE_POLL]  = "POLL",
        [UMQ_TRACE_DATA_TYPE_WAIT]  = "WAIT",
        [UMQ_TRACE_DATA_TYPE_REARM] = "REARM",
    };
    return (type < UMQ_TRACE_DATA_TYPE_MAX) ? type_str[type] : "UNKNOWN";
}

static const char *umq_urma_func_str(umq_urma_func_type_t func_type)
{
    static const char *func_str[] = {
        [UMQ_URMA_FUNC_POST_SEND]  = "POST_SEND",
        [UMQ_URMA_FUNC_POST_RECV]  = "POST_RECV",
        [UMQ_URMA_FUNC_POLL_TX]    = "POLL_TX",
        [UMQ_URMA_FUNC_POLL_RX]    = "POLL_RX",
        [UMQ_URMA_FUNC_WAIT_JFC]   = "WAIT_JFC",
        [UMQ_URMA_FUNC_ACK_JFC]    = "ACK_JFC",
        [UMQ_URMA_FUNC_REARM_JFC]  = "REARM_JFC",
    };
    return (func_type < UMQ_URMA_FUNC_MAX) ? func_str[func_type] : "UNKNOWN";
}

static void umq_trace_print_single(umq_trace_buf_t *cur_rec, uint32_t thread_id)
{
    uint32_t record_cnt = cur_rec->record_cnt;
    uint32_t pre_output_cnt = cur_rec->pre_output_cnt;
    uint32_t new_records;

    if (record_cnt >= pre_output_cnt) {
        new_records = record_cnt - pre_output_cnt;
    } else {
        new_records = (g_umq_data_record_num - pre_output_cnt) + record_cnt;
    }

    if (new_records <= g_umq_output_record_limit) {
        return;
    }

    UMQ_VLOG_INFO(VLOG_UMQ, "============ thread %u records: %u--%u ===========\n",
                  thread_id, pre_output_cnt, record_cnt);

    for (uint32_t i = pre_output_cnt; i < record_cnt; i++) {
        uint32_t idx = i % g_umq_data_record_num;
        umq_data_record_t *rec = &cur_rec->data_record[idx];
        uint64_t umq_exec = (rec->end_time > rec->start_time) ?
                            (rec->end_time - rec->start_time) : 0;

        /* header line: record index + type + meta */
        UMQ_VLOG_INFO(VLOG_UMQ, "#%u type=%s jetty_id=%u local_umq_id=%u remote_umq_id=%u "
            "item_cnt=%u ts=%lu\n",
            i, umq_trace_type_str(rec->type),
            rec->jetty_id, rec->local_umq_id, rec->remote_umq_id,
            rec->item_cnt, rec->timestamp);

        /* data item lines */
        for (uint32_t k = 0; k < rec->item_cnt; k++) {
            UMQ_VLOG_INFO(VLOG_UMQ, "  item[%u] msn=%u size=%u\n",
                          k, rec->items[k].msn, rec->items[k].size);
        }

        /* timing line */
        UMQ_VLOG_INFO(VLOG_UMQ, "  umq_start=%lu umq_end=%lu umq_exec=%lu\n",
                      rec->start_time, rec->end_time, umq_exec);

        /* sub_time lines */
        for (uint32_t j = 0; j < rec->sub_time_cnt; j++) {
            umq_sub_time_t *sub = &rec->sub_time[j];
            UMQ_VLOG_INFO(VLOG_UMQ, "  sub[%u] func=%s start=%lu exec=%lu\n",
                          j, umq_urma_func_str(sub->func_type),
                          sub->start_time, sub->exec_time);
        }
    }

    cur_rec->pre_output_cnt = record_cnt;
}

void umq_trace_print(void *args)
{
    (void)args;
    if (!g_umq_trace_enable) {
        return;
    }
    for (uint32_t i = 0; i < UMQ_PERF_REC_MAX_NUM; ++i) {
        if (!g_umq_trace_ctx->data_table[i].is_used) {
            continue;
        }
        umq_trace_buf_t *cur_rec = &g_umq_trace_ctx->data_table[i];
        if (cur_rec->record_cnt == 0) {
            continue;
        }
        umq_trace_print_single(cur_rec, i);
    }
}

void umq_ub_remain_trace_print(void)
{
    if (!g_umq_trace_enable) {
        return;
    }
    g_umq_output_record_limit = 0;
    umq_trace_print(NULL);
}

static int umq_ub_trace_print_timer_create(void)
{
    urpc_timer_t *timer = urpc_timer_create(0xFFFFFFFF, false);
    if (URPC_UNLIKELY(timer == NULL)) {
        UMQ_VLOG_ERR(VLOG_UMQ, "umq timer create failed\n");
        return UMQ_FAIL;
    }
    int ret = urpc_timer_start(timer, 1, umq_trace_print, NULL, true);
    if (URPC_UNLIKELY(ret != UMQ_SUCCESS)) {
        goto TIMER_DESTROY;
    }
    return UMQ_SUCCESS;

TIMER_DESTROY:
    urpc_timer_destroy(timer);
    return UMQ_FAIL;
}

int umq_trace_start(umq_trace_cfg_t *cfg)
{
    if (g_umq_trace_ctx == NULL) {
        int ret = umq_trace_init(cfg);
        if (ret != UMQ_SUCCESS) {
            UMQ_VLOG_ERR(VLOG_UMQ, "umq perf data init failed\n");
            return ret;
        }
    }

    if (g_umq_trace_enable) {
        UMQ_VLOG_ERR(VLOG_UMQ, "invalid parameter\n");
        return -UMQ_ERR_EINVAL;
    }
    if (umq_ub_trace_print_timer_create() != UMQ_SUCCESS) {
        UMQ_VLOG_ERR(VLOG_UMQ, "umq print data timer create failed\n");
        return -UMQ_ERR_EINVAL;
    }

    g_umq_trace_enable = true;
    return UMQ_SUCCESS;
}

int umq_trace_stop(void)
{
    if (g_umq_trace_ctx == NULL || !g_umq_trace_enable) {
        UMQ_VLOG_ERR(VLOG_UMQ, "invalid parameter\n");
        return -UMQ_ERR_EINVAL;
    }

    g_umq_trace_enable = false;
    return UMQ_SUCCESS;
}
