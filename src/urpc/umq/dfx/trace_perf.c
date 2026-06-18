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

static __thread uint32_t g_trace_perf_index = -1;
static __thread pthread_once_t g_dp_thread_run_once = PTHREAD_ONCE_INIT;
static uint32_t g_umq_output_record_limit = DATA_RECORD_LIMIT;
static uint32_t g_umq_data_record_num = DATA_RECORD_NUM;
static bool g_umq_trace_perf_enable = false;
static pthread_spinlock_t g_umq_trace_perf_lock;

typedef struct umq_trace_perf {
    uint32_t post_cnt;
    uint32_t pre_post_cnt;
    uint32_t poll_cnt;
    uint32_t pre_poll_cnt;
    uint32_t interrupt_cnt;
    uint32_t pre_interrupt_cnt;
    umq_data_record_t post_data_record[g_umq_data_record_num];
    umq_data_record_t poll_data_record[g_umq_data_record_num];
    umq_data_record_t interrupt_data_record[g_umq_data_record_num];
    bool is_used;
} umq_trace_perf_t;

typedef struct umq_trace_perf_ctx {
    pthread_once_t *dp_thread_run_once[UMQ_PERF_REC_MAX_NUM];
    umq_trace_perf_t data_table[UMQ_PERF_REC_MAX_NUM];
} umq_trace_perf_ctx_t;

static umq_trace_perf_ctx_t *g_umq_trace_perf_ctx;

int umq_trace_perf_init(umq_trace_perf_cfg_t *cfg)
{
    if (g_umq_trace_perf_ctx != NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "umq perf has been inited\n");
        return -UMQ_ERR_EEXIST;
    }
    if (cfg->flag & UMQ_PERF_FLAG_RECORD_NUM) {
        g_umq_data_record_num = cfg->data_record_num;
    }
    if (cfg->flag & UMQ_PERF_FLAG_RECORD_LIMIT) {
        g_umq_output_record_limit = cfg->output_record_limit;
    }
    (void)pthread_spin_init(&g_umq_trace_perf_lock, PTHREAD_PROCESS_PRIVATE);

    g_umq_trace_perf_ctx = (umq_trace_perf_ctx_t *)calloc(1, sizeof(umq_trace_perf_ctx_t));
    if (g_umq_trace_perf_ctx == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "calloc for umq_trace_perf failed\n");
        return -UMQ_ERR_ENOMEM;
    }
    return UMQ_SUCCESS;
}

void umq_trace_perf_uninit(void)
{
    if (g_umq_trace_perf_ctx == NULL) {
        return;
    }

    (void)pthread_spin_lock(&g_umq_trace_perf_lock);
    for (uint32_t i = 0; i < UMQ_PERF_REC_MAX_NUM; i++) {
        if (g_umq_trace_perf_ctx->dp_thread_run_once[i] != NULL) {
            *g_umq_trace_perf_ctx->dp_thread_run_once[i] = PTHREAD_ONCE_INIT;
        }
    }

    g_umq_trace_perf_enable = false;
    free(g_umq_trace_perf_ctx);
    g_umq_trace_perf_ctx = NULL;
    (void)pthread_spin_unlock(&g_umq_trace_perf_lock);
    (void)pthread_spin_destroy(&g_umq_trace_perf_lock);
}


static void umq_trace_perf_closure(uint64_t idx)
{
    (void)pthread_spin_lock(&g_umq_trace_perf_lock);
    if (g_umq_trace_perf_ctx == NULL) {
        (void)pthread_spin_unlock(&g_umq_trace_perf_lock);
        return;
    }
    g_umq_trace_perf_ctx->data_table[idx].is_used = false;
    g_umq_trace_perf_ctx->dp_thread_run_once[idx] = NULL;
    (void)pthread_spin_unlock(&g_umq_trace_perf_lock);
}

static void umq_trace_record_clear(uint32_t idx)
{
    umq_trace_perf_t *cur_record = &g_umq_trace_perf_ctx->data_table[idx];
    cur_record->post_cnt = 0;
    cur_record->poll_cnt = 0;
    cur_record->pre_post_cnt = 0;
    cur_record->pre_poll_cnt = 0;
    cur_record->interrupt_cnt = 0;
    cur_record->pre_interrupt_cnt = 0;
}

void umq_trace_perf_alloc(void)
{
    uint32_t idx;
    (void)pthread_spin_lock(&g_umq_trace_perf_lock);
    if (g_umq_trace_perf_ctx == NULL) {
        (void)pthread_spin_unlock(&g_umq_trace_perf_lock);
        UMQ_VLOG_ERR(VLOG_UMQ, "perf data ctx invalid\n");
        return;
    }

    for (idx = 0; idx < UMQ_PERF_REC_MAX_NUM; ++idx) {
        if (!g_umq_trace_perf_ctx->data_table[idx].is_used) {
            break;
        }
    }
    if (idx == UMQ_PERF_REC_MAX_NUM) {
        (void)pthread_spin_unlock(&g_umq_trace_perf_lock);
        UMQ_VLOG_WARN(VLOG_UMQ, "perf data table capacity %u were exhausted, alloc perf_rec failed\n",
            UMQ_PERF_REC_MAX_NUM);
        return;
    }

    umq_trace_record_clear(idx);
    g_umq_trace_perf_ctx->data_table[idx].is_used = true;
    (void)pthread_spin_unlock(&g_umq_trace_perf_lock);

    g_trace_perf_index = idx;
    g_umq_trace_perf_ctx->dp_thread_run_once[idx] = &g_dp_thread_run_once;
    urpc_thread_closure_register(THREAD_CLOSURE_UMQ_DATA_PERF, idx, umq_trace_perf_closure);
}

static void umq_dp_thread_run_once(void)
{
    umq_trace_perf_alloc();
}

uint64_t umq_trace_perf_start_timestamp_get(void)
{
    if (!g_umq_trace_perf_enable) {
        return 0;
    }
    pthread_once(&g_dp_thread_run_once, umq_dp_thread_run_once);
    return get_timestamp_ns();
}

uint64_t umq_trace_perf_timestamp_get(void)
{
    if (!g_umq_trace_perf_enable) {
        return 0;
    }
    return get_timestamp_ns();
}

static void post_trace_record_fill(umq_trace_perf_t *cur_rec, umq_data_record_t *data)
{
    uint32_t post_cnt = cur_rec->post_cnt;
    cur_rec->post_data_record[post_cnt].user_data = data->user_data;
    cur_rec->post_data_record[post_cnt].total_size = data->total_size;
    cur_rec->post_data_record[post_cnt].jetty_id = data->jetty_id;
    cur_rec->post_data_record[post_cnt].local_umq_id = data->local_umq_id;
    cur_rec->post_data_record[post_cnt].remote_umq_id = data->remote_umq_id;
    cur_rec->post_data_record[post_cnt].timestamp = data->timestamp;
    cur_rec->post_data_record[post_cnt].post.tp_post_time = data->post.tp_post_time;
    cur_rec->post_data_record[post_cnt].post.tp_post_start = data->post.tp_post_start;
    cur_rec->post_data_record[post_cnt].post.tp_post_end = data->post.tp_post_end;
    cur_rec->post_data_record[post_cnt].post.fc_send_req = data->post.fc_send_req;
    cur_rec->post_data_record[post_cnt].post.umq_post_start = data->post.umq_post_start;
    cur_rec->post_cnt++;
    if (cur_rec->post_cnt >= g_umq_data_record_num) {
        cur_rec->post_cnt = 0;
    }
}

static void poll_trace_record_fill(umq_trace_perf_t *cur_rec, umq_data_record_t *data)
{
    uint32_t poll_cnt = cur_rec->poll_cnt;
    cur_rec->poll_data_record[poll_cnt].user_data = data->user_data;
    cur_rec->poll_data_record[poll_cnt].total_size = data->total_size;
    cur_rec->poll_data_record[poll_cnt].jetty_id = data->jetty_id;
    cur_rec->poll_data_record[poll_cnt].local_umq_id = data->local_umq_id;
    cur_rec->poll_data_record[poll_cnt].remote_umq_id = data->remote_umq_id;
    cur_rec->poll_data_record[poll_cnt].timestamp = data->timestamp;
    cur_rec->poll_data_record[poll_cnt].poll.tp_poll_time = data->poll.tp_poll_time;
    cur_rec->poll_data_record[poll_cnt].poll.umq_poll_end = data->poll.umq_poll_end;
    cur_rec->poll_cnt++;
    if (cur_rec->poll_cnt >= g_umq_data_record_num) {
        cur_rec->poll_cnt = 0;
    }
}

static void interrupt_trace_record_fill(umq_trace_perf_t *cur_rec, umq_data_record_t *data)
{
    uint32_t interrupt_cnt = cur_rec->interrupt_cnt;
    cur_rec->poll_data_record[interrupt_cnt].jetty_id = data->jetty_id;
    cur_rec->poll_data_record[interrupt_cnt].interrupt.umq_wait_start = data->interrupt.umq_wait_start;
    cur_rec->poll_data_record[interrupt_cnt].interrupt.tp_wait_start = data->interrupt.tp_wait_start;
    cur_rec->poll_data_record[interrupt_cnt].interrupt.tp_wait_time = data->interrupt.tp_wait_time;
    cur_rec->poll_data_record[interrupt_cnt].interrupt.umq_wait_end = data->interrupt.umq_wait_end;
    cur_rec->poll_data_record[interrupt_cnt].interrupt.tp_ack_start = data->interrupt.tp_ack_start;
    cur_rec->poll_data_record[interrupt_cnt].interrupt.tp_ack_time = data->interrupt.tp_ack_time;
    cur_rec->poll_data_record[interrupt_cnt].interrupt.umq_rearm_start = data->interrupt.umq_rearm_start;
    cur_rec->poll_data_record[interrupt_cnt].interrupt.tp_rearm_start = data->interrupt.tp_rearm_start;
    cur_rec->poll_data_record[interrupt_cnt].interrupt.tp_rearm_time = data->interrupt.tp_rearm_time;
    cur_rec->poll_data_record[interrupt_cnt].interrupt.tp_fc_rearm_start = data->interrupt.tp_fc_rearm_start;
    cur_rec->poll_data_record[interrupt_cnt].interrupt.tp_fc_rearm_time = data->interrupt.tp_fc_rearm_time;
    cur_rec->poll_data_record[interrupt_cnt].interrupt.umq_rearm_end = data->interrupt.umq_rearm_end;
    cur_rec->interrupt_cnt++;
    if (cur_rec->interrupt_cnt >= g_umq_data_record_num) {
        cur_rec->interrupt_cnt = 0;
    }
}

typedef void (*trace_handler_t)(umq_trace_perf_t *cur_rec, umq_data_record_t *data);

static void umq_perf_trace_record_fill(umq_trace_perf_type_t type, umq_data_record_t *data)
{
    umq_trace_perf_t *cur_rec = &g_umq_trace_perf_ctx->data_table[g_trace_perf_index];
    static trace_handler_t trace_record_map[UMQ_PERF_DATA_TYPE_MAX] = {
        [UMQ_PERF_DATA_TYPE_POST] = post_trace_record_fill,
        [UMQ_PERF_DATA_TYPE_POLL]  = poll_trace_record_fill,
        [UMQ_PERF_DATA_TYPE_INTERRUPT]  = interrupt_trace_record_fill,
    };
    trace_record_map[type](cur_rec, data);
}

void umq_trace_perf_write(umq_trace_perf_type_t type, umq_data_record_t *data)
{
    if (!g_umq_trace_perf_enable || g_trace_perf_index >= UMQ_PERF_REC_MAX_NUM) {
        return;
    }
    umq_perf_trace_record_fill(type, data);
}

static void umq_post_trace_perf_print(umq_trace_perf_t *cur_rec, uint32_t i)
{
    uint32_t post_cnt = cur_rec->post_cnt;
    uint32_t pre_post_cnt = cur_rec->pre_post_cnt;
    uint32_t post_end = post_cnt < pre_post_cnt? (post_cnt + g_umq_data_record_num): post_cnt;
    if (post_end - pre_post_cnt > g_umq_output_record_limit) {
        uint32_t idx = 0;
        UMQ_VLOG_INFO(VLOG_UMQ, "============thread %d post: %d--%d ===========\n", i, pre_post_cnt, post_end);
        for (uint32_t j = pre_post_cnt; j < post_end; j++) {
            idx = j % g_umq_data_record_num;
            UMQ_VLOG_INFO(VLOG_UMQ, "user_data: %u, total_size: %u, jetty_id: %u, local_umq_id: %u, "
            "remote_umq_id: %u, timestamp: %lu, fc_send_req: %lu, tp_post_start: %lu, tp_post_end: %lu, "
            "tp_post_time: %lu, umq_post_start: %lu\n",
            cur_rec->post_data_record[idx].user_data,
            cur_rec->post_data_record[idx].total_size,
            cur_rec->post_data_record[idx].jetty_id, 
            cur_rec->post_data_record[idx].local_umq_id, 
            cur_rec->post_data_record[idx].remote_umq_id,
            cur_rec->post_data_record[idx].timestamp, 
            cur_rec->post_data_record[idx].post.fc_send_req,
            cur_rec->post_data_record[idx].post.tp_post_start,
            cur_rec->post_data_record[idx].post.tp_post_end,
            cur_rec->post_data_record[idx].post.tp_post_time,
            cur_rec->post_data_record[idx].post.umq_post_start
            );
        }
        cur_rec->pre_post_cnt = post_cnt;
    }
}

static void umq_poll_trace_perf_print(umq_trace_perf_t *cur_rec, uint32_t i)
{
    uint32_t poll_cnt = cur_rec->poll_cnt;
    uint32_t pre_poll_cnt = cur_rec->pre_poll_cnt;
    uint32_t poll_end = poll_cnt < pre_poll_cnt? (poll_cnt + g_umq_data_record_num): poll_cnt;
    if (poll_end - pre_poll_cnt > g_umq_output_record_limit) {
        uint32_t idx = 0;
        UMQ_VLOG_INFO(VLOG_UMQ, "============thread %d poll: %d--%d ===========\n", i, pre_poll_cnt, poll_end);
        for (uint32_t j = pre_poll_cnt; j < poll_end; j++) {
            idx = j % g_umq_data_record_num;
            UMQ_VLOG_INFO(VLOG_UMQ, "user_data: %u, total_size: %u, jetty_id: %u, local_umq_id: %u, remote_umq_id: "
                "%u, timestamp: %lu, umq_poll_end: %lu, tp_poll_time: %lu\n",
                cur_rec->poll_data_record[idx].user_data,
                cur_rec->poll_data_record[idx].total_size,
                cur_rec->poll_data_record[idx].jetty_id, 
                cur_rec->poll_data_record[idx].local_umq_id, 
                cur_rec->poll_data_record[idx].remote_umq_id,
                cur_rec->poll_data_record[idx].timestamp, 
                cur_rec->poll_data_record[idx].poll.umq_poll_end,
                cur_rec->poll_data_record[idx].poll.tp_poll_time);
        }
        cur_rec->pre_poll_cnt = poll_cnt;
    }
}

static void umq_interrupt_trace_perf_print(umq_trace_perf_t *cur_rec, uint32_t i)
{
    uint32_t interrupt_cnt = cur_rec->interrupt_cnt;
    uint32_t pre_interrupt_cnt = cur_rec->pre_interrupt_cnt;
    uint32_t interrupt_end = interrupt_cnt < pre_interrupt_cnt? (interrupt_cnt + g_umq_data_record_num): interrupt_cnt;
    if (interrupt_end - pre_interrupt_cnt > g_umq_output_record_limit)  {
        uint32_t idx = 0;
        UMQ_VLOG_INFO(VLOG_UMQ, "==========thread %d interrupt: %d--%d ==========\n", i, pre_interrupt_cnt, interrupt_end);
        for (uint32_t j = pre_interrupt_cnt; j < interrupt_end; j++) {
            idx = j % g_umq_data_record_num;
            UMQ_VLOG_INFO(VLOG_UMQ, "jetty_id: %u, umq_wait_start: %lu, tp_wait_start: %lu, tp_wait_time: %lu, "
                "umq_wait_end: %lu, tp_ack_start: %lu, tp_ack_time: %lu, tp_rearm_start: %lu, "
                "tp_rearm_time: %lu, tp_fc_rearm_start: %lu, tp_fc_rearm_time: %lu, umq_rearm_end: %lu\n",
                cur_rec->interrupt_data_record[idx].jetty_id,
                cur_rec->interrupt_data_record[idx].interrupt.umq_wait_start,
                cur_rec->interrupt_data_record[idx].interrupt.tp_wait_start, 
                cur_rec->interrupt_data_record[idx].interrupt.tp_wait_time, 
                cur_rec->interrupt_data_record[idx].interrupt.umq_wait_end,
                cur_rec->interrupt_data_record[idx].interrupt.tp_ack_start, 
                cur_rec->interrupt_data_record[idx].interrupt.tp_ack_time,
                cur_rec->interrupt_data_record[idx].interrupt.tp_rearm_start,
                cur_rec->interrupt_data_record[idx].interrupt.tp_rearm_time,
                cur_rec->interrupt_data_record[idx].interrupt.tp_fc_rearm_start,
                cur_rec->interrupt_data_record[idx].interrupt.tp_fc_rearm_time,
                cur_rec->interrupt_data_record[idx].interrupt.umq_rearm_end);
        }
        cur_rec->pre_interrupt_cnt = interrupt_cnt;
    }
}

void umq_trace_perf_print(void *args)
{
    if (!g_umq_trace_perf_enable) {
 	    return;
 	}
    for (uint32_t i = 0; i < UMQ_PERF_REC_MAX_NUM; ++i) {
        if (!g_umq_trace_perf_ctx->data_table[i].is_used) {
            break;
        }
        umq_trace_perf_t *cur_rec = &g_umq_trace_perf_ctx->data_table[i];
        umq_post_trace_perf_print(cur_rec, i);
        umq_poll_trace_perf_print(cur_rec, i);
        umq_interrupt_trace_perf_print(cur_rec, i);
    }
}

void umq_ub_remain_trace_perf_print(void)
{
    if (!g_umq_trace_perf_enable) {
 	    return;
 	}
    g_umq_output_record_limit = 0;
    void *args = NULL;
    umq_trace_perf_print(args);
}

static int umq_ub_trace_print_timer_create(void)
{
    urpc_timer_t *timer = urpc_timer_create(0xFFFFFFFF, false);
    if (URPC_UNLIKELY(timer == NULL)) {
        UMQ_VLOG_ERR(VLOG_UMQ, "umq timer create failed\n");
        return UMQ_FAIL;
    }
    void *args = NULL;
    int ret = urpc_timer_start(timer, 1, umq_trace_perf_print, args, true);
    if (URPC_UNLIKELY(ret != UMQ_SUCCESS)) {
        goto TIMER_DESTROY;
    }
    return UMQ_SUCCESS;

TIMER_DESTROY:
    urpc_timer_destroy(timer);
    return UMQ_FAIL;
}

int umq_trace_perf_start(umq_trace_perf_cfg_t *cfg)
{
    if (g_umq_trace_perf_ctx == NULL) {
        int ret = umq_trace_perf_init(cfg);
        if (ret != UMQ_SUCCESS) {
            UMQ_VLOG_ERR(VLOG_UMQ, "umq perf data init failed\n");
            return ret;
        }
    }

    // IO perf record has been started, user must stop it first before restart
    if (g_umq_trace_perf_enable) {
        UMQ_VLOG_ERR(VLOG_UMQ, "invalid parameter\n");
        return -UMQ_ERR_EINVAL;
    }
    if (umq_ub_trace_print_timer_create() != UMQ_SUCCESS) {
        UMQ_VLOG_ERR(VLOG_UMQ, "umq print data timer create failed\n");
        return -UMQ_ERR_EINVAL;
    }

    g_umq_trace_perf_enable = true;
    return UMQ_SUCCESS;
}

int umq_trace_perf_stop(void)
{
    if (g_umq_trace_perf_ctx == NULL || !g_umq_trace_perf_enable) {
        UMQ_VLOG_ERR(VLOG_UMQ, "invalid parameter\n");
        return -UMQ_ERR_EINVAL;
    }

    g_umq_trace_perf_enable = false;
    return UMQ_SUCCESS;
}
