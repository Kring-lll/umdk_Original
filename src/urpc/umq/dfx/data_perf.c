/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: umq perf
 * Create: 2025-10-29
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

static __thread uint32_t g_perf_data_index = -1;
static __thread pthread_once_t g_dp_thread_run_once = PTHREAD_ONCE_INIT;
static uint32_t g_umq_data_record_limit = DATA_RECORD_LIMIT;
static uint32_t g_umq_data_record_num = DATA_RECORD_NUM;
static bool g_umq_perf_data_enable = false;
static pthread_spinlock_t g_umq_perf_data_lock;

typedef struct umq_perf_data {
    uint32_t post_cnt;
    uint32_t pre_post_cnt;
    uint32_t poll_cnt;
    uint32_t pre_poll_cnt;
    uint32_t inter_cnt;
    uint32_t pre_inter_cnt;
    umq_data_record_t post_data_record[DATA_RECORD_NUM];
    umq_data_record_t poll_data_record[DATA_RECORD_NUM];
    umq_data_record_t inter_data_record[DATA_RECORD_NUM];
    bool is_used;
} umq_perf_data_t;

typedef struct umq_perf_data_ctx {
    pthread_once_t *dp_thread_run_once[UMQ_PERF_REC_MAX_NUM];
    umq_perf_data_t data_table[UMQ_PERF_REC_MAX_NUM];
} umq_perf_data_ctx_t;

static umq_perf_data_ctx_t *g_umq_perf_data_ctx;

int umq_perf_data_init(umq_perf_data_cfg_t cfg)
{
    if (g_umq_perf_data_ctx != NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "umq perf has been inited\n");
        return -UMQ_ERR_EEXIST;
    }
    if (cfg.perf_flag & UMQ_PERF_FLAG_RECORD_NUM) {
        g_umq_data_record_num = cfg.data_record_num;
    }
    if (cfg.perf_flag & UMQ_PERF_FLAG_RECORD_LIMIT) {
        g_umq_data_record_limit = cfg.data_record_limit;
    }
    (void)pthread_spin_init(&g_umq_perf_data_lock, PTHREAD_PROCESS_PRIVATE);

    g_umq_perf_data_ctx = (umq_perf_data_ctx_t *)calloc(1, sizeof(umq_perf_data_ctx_t));
    if (g_umq_perf_data_ctx == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "calloc for umq_perf_data failed\n");
        return -UMQ_ERR_ENOMEM;
    }
    return UMQ_SUCCESS;
}

void umq_perf_data_uninit(void)
{
    if (g_umq_perf_data_ctx == NULL) {
        return;
    }

    (void)pthread_spin_lock(&g_umq_perf_data_lock);
    for (uint32_t i = 0; i < UMQ_PERF_REC_MAX_NUM; i++) {
        if (g_umq_perf_data_ctx->dp_thread_run_once[i] != NULL) {
            *g_umq_perf_data_ctx->dp_thread_run_once[i] = PTHREAD_ONCE_INIT;
        }
    }

    g_umq_perf_data_enable = false;
    free(g_umq_perf_data_ctx);
    g_umq_perf_data_ctx = NULL;
    (void)pthread_spin_unlock(&g_umq_perf_data_lock);
    (void)pthread_spin_destroy(&g_umq_perf_data_lock);
}


static void umq_perf_data_closure(uint64_t idx)
{
    (void)pthread_spin_lock(&g_umq_perf_data_lock);
    if (g_umq_perf_data_ctx == NULL) {
        (void)pthread_spin_unlock(&g_umq_perf_data_lock);
        return;
    }
    g_umq_perf_data_ctx->data_table[idx].is_used = false;
    g_umq_perf_data_ctx->dp_thread_run_once[idx] = NULL;
    (void)pthread_spin_unlock(&g_umq_perf_data_lock);
}

static void umq_clear_data_record(uint32_t idx)
{
    umq_perf_data_t *cur_record = &g_umq_perf_data_ctx->data_table[idx];
    cur_record->post_cnt = 0;
    cur_record->poll_cnt = 0;
    cur_record->pre_post_cnt = 0;
    cur_record->pre_poll_cnt = 0;
    cur_record->inter_cnt = 0;
    cur_record->pre_inter_cnt = 0;
}

void umq_perf_data_alloc(void)
{
    uint32_t idx;
    (void)pthread_spin_lock(&g_umq_perf_data_lock);
    if (g_umq_perf_data_ctx == NULL) {
        (void)pthread_spin_unlock(&g_umq_perf_data_lock);
        UMQ_VLOG_ERR(VLOG_UMQ, "perf data ctx invalid\n");
        return;
    }

    for (idx = 0; idx < UMQ_PERF_REC_MAX_NUM; ++idx) {
        if (!g_umq_perf_data_ctx->data_table[idx].is_used) {
            break;
        }
    }
    if (idx == UMQ_PERF_REC_MAX_NUM) {
        (void)pthread_spin_unlock(&g_umq_perf_data_lock);
        UMQ_VLOG_WARN(VLOG_UMQ, "perf data table capacity %u were exhausted, alloc perf_rec failed\n",
            UMQ_PERF_REC_MAX_NUM);
        return;
    }

    umq_clear_data_record(idx);
    g_umq_perf_data_ctx->data_table[idx].is_used = true;
    (void)pthread_spin_unlock(&g_umq_perf_data_lock);

    g_perf_data_index = idx;
    g_umq_perf_data_ctx->dp_thread_run_once[idx] = &g_dp_thread_run_once;
    urpc_thread_closure_register(THREAD_CLOSURE_UMQ_DATA_PERF, idx, umq_perf_data_closure);
}

static void umq_dp_thread_run_once(void)
{
    umq_perf_data_alloc();
}

uint64_t umq_perf_data_get_start_timestamp(void)
{
    if (!g_umq_perf_data_enable) {
        return 0;
    }
    pthread_once(&g_dp_thread_run_once, umq_dp_thread_run_once);
    return get_timestamp_ns();
}

uint64_t umq_perf_data_get_timestamp(void)
{
    if (!g_umq_perf_data_enable) {
        return 0;
    }
    return get_timestamp_ns();
}

static inline void umq_perf_fill_data_record(umq_perf_data_type_t type, umq_data_record_t data)
{
    umq_perf_data_t *cur_rec = &g_umq_perf_data_ctx->data_table[g_perf_data_index];
    if (type == UMQ_PERF_DATA_TYPE_POST) {
        uint32_t post_cnt = cur_rec->post_cnt;
        cur_rec->post_data_record[post_cnt].user_data = data.user_data;
        cur_rec->post_data_record[post_cnt].total_size = data.total_size;
        cur_rec->post_data_record[post_cnt].jetty_id = data.jetty_id;
        cur_rec->post_data_record[post_cnt].local_umq_id = data.local_umq_id;
        cur_rec->post_data_record[post_cnt].remote_umq_id = data.remote_umq_id;
        cur_rec->post_data_record[post_cnt].timestamp = data.timestamp;
        cur_rec->post_data_record[post_cnt].post.urma_post_time = data.post.urma_post_time;
        cur_rec->post_data_record[post_cnt].post.urma_post_start = data.post.urma_post_start;
        cur_rec->post_data_record[post_cnt].post.urma_post_end = data.post.urma_post_end;
        cur_rec->post_data_record[post_cnt].post.fc_send_req = data.post.fc_send_req;
        cur_rec->post_data_record[post_cnt].post.umq_post_start = data.post.umq_post_start;
        cur_rec->post_cnt++;
        if (cur_rec->post_cnt >= DATA_RECORD_NUM) {
            cur_rec->post_cnt = 0;
        }
    } else if (type == UMQ_PERF_DATA_TYPE_POLL) {
        uint32_t poll_cnt = cur_rec->poll_cnt;
        cur_rec->poll_data_record[poll_cnt].user_data = data.user_data;
        cur_rec->poll_data_record[poll_cnt].total_size = data.total_size;
        cur_rec->poll_data_record[poll_cnt].jetty_id = data.jetty_id;
        cur_rec->poll_data_record[poll_cnt].local_umq_id = data.local_umq_id;
        cur_rec->poll_data_record[poll_cnt].remote_umq_id = data.remote_umq_id;
        cur_rec->poll_data_record[poll_cnt].timestamp = data.timestamp;
        cur_rec->poll_data_record[poll_cnt].poll.urma_poll_time = data.poll.urma_poll_time;
        cur_rec->poll_data_record[poll_cnt].poll.umq_poll_end = data.poll.umq_poll_end;
        cur_rec->poll_cnt++;
        if (cur_rec->poll_cnt >= DATA_RECORD_NUM) {
            cur_rec->poll_cnt = 0;
        }
    } else if (type == UMQ_PERF_DATA_TYPE_INTERRUPT)  {
        uint32_t inter_cnt = cur_rec->inter_cnt;
        cur_rec->poll_data_record[inter_cnt].jetty_id = data.jetty_id;
        cur_rec->poll_data_record[inter_cnt].inter.umq_wait_start = data.inter.umq_wait_start;
        cur_rec->poll_data_record[inter_cnt].inter.urma_wait_start = data.inter.urma_wait_start;
        cur_rec->poll_data_record[inter_cnt].inter.urma_wait_time = data.inter.urma_wait_time;
        cur_rec->poll_data_record[inter_cnt].inter.umq_wait_end = data.inter.umq_wait_end;
        cur_rec->poll_data_record[inter_cnt].inter.urma_ack_start = data.inter.urma_ack_start;
        cur_rec->poll_data_record[inter_cnt].inter.urma_ack_time = data.inter.urma_ack_time;
        cur_rec->poll_data_record[inter_cnt].inter.umq_rearm_start = data.inter.umq_rearm_start;
        cur_rec->poll_data_record[inter_cnt].inter.urma_rearm_start = data.inter.urma_rearm_start;
        cur_rec->poll_data_record[inter_cnt].inter.urma_rearm_time = data.inter.urma_rearm_time;
        cur_rec->poll_data_record[inter_cnt].inter.urma_fc_rearm_start = data.inter.urma_fc_rearm_start;
        cur_rec->poll_data_record[inter_cnt].inter.urma_fc_rearm_time = data.inter.urma_fc_rearm_time;
        cur_rec->poll_data_record[inter_cnt].inter.umq_rearm_end = data.inter.umq_rearm_end;
        cur_rec->inter_cnt++;
        if (cur_rec->inter_cnt >= DATA_RECORD_NUM) {
            cur_rec->inter_cnt = 0;
        }
    }
}

void umq_perf_data_write(umq_perf_data_type_t type, umq_data_record_t data)
{
    if (!g_umq_perf_data_enable || g_perf_data_index >= UMQ_PERF_REC_MAX_NUM) {
        return;
    }
    umq_perf_fill_data_record(type, data);
}

void umq_ub_data_print(void *args)
{
    if (!g_umq_perf_data_enable) {
 	    return;
 	}
    for (uint32_t k = 0; k < UMQ_PERF_REC_MAX_NUM; ++k) {
        if (!g_umq_perf_data_ctx->data_table[k].is_used) {
            break;
        }
        umq_perf_data_t *cur_rec = &g_umq_perf_data_ctx->data_table[k];
        uint32_t post_cnt = cur_rec->post_cnt;
        uint32_t poll_cnt = cur_rec->poll_cnt;
        uint32_t pre_post_cnt = cur_rec->pre_post_cnt;
        uint32_t pre_poll_cnt = cur_rec->pre_poll_cnt;
        uint32_t post_end = post_cnt < pre_post_cnt? (post_cnt + DATA_RECORD_NUM): post_cnt;
        uint32_t poll_end = poll_cnt < pre_poll_cnt? (poll_cnt + DATA_RECORD_NUM): poll_cnt;
        uint32_t inter_cnt = cur_rec->inter_cnt;
        uint32_t pre_inter_cnt = cur_rec->pre_inter_cnt;
        uint32_t inter_end = inter_cnt < pre_inter_cnt? (inter_cnt + DATA_RECORD_NUM): inter_cnt;
        if (post_end - pre_post_cnt > g_umq_data_record_limit) {
            uint32_t idx = 0;
            UMQ_VLOG_INFO(VLOG_UMQ, "============thread %d post: %d--%d ===========\n", k, pre_post_cnt, post_end);
            for (uint32_t i = pre_post_cnt; i < post_end; i++) {
                idx = i % DATA_RECORD_NUM;
                UMQ_VLOG_INFO(VLOG_UMQ, "user_data: %u, total_size: %u, jetty_id: %u, local_umq_id: %u, "
                "remote_umq_id: %u, timestamp: %lu, fc_send_req: %lu, urma_post_start: %lu, urma_post_end: %lu, "
                "urma_post_time: %lu, umq_post_start: %lu\n",
                cur_rec->post_data_record[idx].user_data,
                cur_rec->post_data_record[idx].total_size,
                cur_rec->post_data_record[idx].jetty_id, 
                cur_rec->post_data_record[idx].local_umq_id, 
                cur_rec->post_data_record[idx].remote_umq_id,
                cur_rec->post_data_record[idx].timestamp, 
                cur_rec->post_data_record[idx].post.fc_send_req,
                cur_rec->post_data_record[idx].post.urma_post_start,
                cur_rec->post_data_record[idx].post.urma_post_end,
                cur_rec->post_data_record[idx].post.urma_post_time,
                cur_rec->post_data_record[idx].post.umq_post_start
                );
            }
            cur_rec->pre_post_cnt = post_cnt;
        }
        if (poll_end - pre_poll_cnt > g_umq_data_record_limit) {
            uint32_t idx = 0;
            UMQ_VLOG_INFO(VLOG_UMQ, "============thread %d poll: %d--%d ===========\n", k, pre_poll_cnt, poll_end);
            for (uint32_t i = pre_poll_cnt; i < poll_end; i++) {
                idx = i % DATA_RECORD_NUM;
                UMQ_VLOG_INFO(VLOG_UMQ, "user_data: %u, total_size: %u, jetty_id: %u, local_umq_id: %u, remote_umq_id: "
                    "%u, timestamp: %lu, umq_poll_end: %lu, urma_poll_time: %lu\n",
                    cur_rec->poll_data_record[idx].user_data,
                    cur_rec->poll_data_record[idx].total_size,
                    cur_rec->poll_data_record[idx].jetty_id, 
                    cur_rec->poll_data_record[idx].local_umq_id, 
                    cur_rec->poll_data_record[idx].remote_umq_id,
                    cur_rec->poll_data_record[idx].timestamp, 
                    cur_rec->poll_data_record[idx].poll.umq_poll_end,
                    cur_rec->poll_data_record[idx].poll.urma_poll_time);
            }
            cur_rec->pre_poll_cnt = poll_cnt;
        }
        if (inter_end - pre_inter_cnt > g_umq_data_record_limit)  {
            uint32_t idx = 0;
            UMQ_VLOG_INFO(VLOG_UMQ, "==========thread %d interrupt: %d--%d ==========\n", k, pre_inter_cnt, inter_end);
            for (uint32_t i = pre_inter_cnt; i < inter_end; i++) {
                idx = i % DATA_RECORD_NUM;
                UMQ_VLOG_INFO(VLOG_UMQ, "jetty_id: %u, umq_wait_start: %lu, urma_wait_start: %lu, urma_wait_time: %lu, "
                    "umq_wait_end: %lu, urma_ack_start: %lu, urma_ack_time: %lu, urma_rearm_start: %lu, "
                    "urma_rearm_time: %lu, urma_fc_rearm_start: %lu, urma_fc_rearm_time: %lu, umq_rearm_end: %lu\n",
                    cur_rec->inter_data_record[idx].jetty_id,
                    cur_rec->inter_data_record[idx].inter.umq_wait_start,
                    cur_rec->inter_data_record[idx].inter.urma_wait_start, 
                    cur_rec->inter_data_record[idx].inter.urma_wait_time, 
                    cur_rec->inter_data_record[idx].inter.umq_wait_end,
                    cur_rec->inter_data_record[idx].inter.urma_ack_start, 
                    cur_rec->inter_data_record[idx].inter.urma_ack_time,
                    cur_rec->inter_data_record[idx].inter.urma_rearm_start,
                    cur_rec->inter_data_record[idx].inter.urma_rearm_time,
                    cur_rec->inter_data_record[idx].inter.urma_fc_rearm_start,
                    cur_rec->inter_data_record[idx].inter.urma_fc_rearm_time,
                    cur_rec->inter_data_record[idx].inter.umq_rearm_end);
            }
            cur_rec->pre_inter_cnt = inter_cnt;
        }
    }
}

void umq_ub_remain_perf_data_print(void)
{
    if (!g_umq_perf_data_enable) {
 	    return;
 	}
    g_umq_data_record_limit = 0;
    void *args = NULL;
    umq_ub_data_print(args);
}

static int umq_ub_data_print_timer_create(void)
{
    urpc_timer_t *timer = urpc_timer_create(0xFFFFFFFF, false);
    if (URPC_UNLIKELY(timer == NULL)) {
        UMQ_VLOG_ERR(VLOG_UMQ, "umq timer create failed\n");
        return UMQ_FAIL;
    }
    void *args = NULL;
    int ret = urpc_timer_start(timer, 1, umq_ub_data_print, args, true);
    if (URPC_UNLIKELY(ret != UMQ_SUCCESS)) {
        goto TIMER_DESTROY;
    }
    return UMQ_SUCCESS;

TIMER_DESTROY:
    urpc_timer_destroy(timer);
    return UMQ_FAIL;
}

int umq_perf_data_start(umq_perf_data_cfg_t cfg)
{
    if (g_umq_perf_data_ctx == NULL) {
        int ret = umq_perf_data_init(cfg);
        if (ret != UMQ_SUCCESS) {
            UMQ_VLOG_ERR(VLOG_UMQ, "umq perf data init failed\n");
            return ret;
        }
    }

    // IO perf record has been started, user must stop it first before restart
    if (g_umq_perf_data_enable) {
        UMQ_VLOG_ERR(VLOG_UMQ, "invalid parameter\n");
        return -UMQ_ERR_EINVAL;
    }
    if (umq_ub_data_print_timer_create() != UMQ_SUCCESS) {
        UMQ_VLOG_ERR(VLOG_UMQ, "umq print data timer create failed\n");
        return -UMQ_ERR_EINVAL;
    }

    g_umq_perf_data_enable = true;
    return UMQ_SUCCESS;
}

int umq_perf_data_stop(void)
{
    if (g_umq_perf_data_ctx == NULL || !g_umq_perf_data_enable) {
        UMQ_VLOG_ERR(VLOG_UMQ, "invalid parameter\n");
        return -UMQ_ERR_EINVAL;
    }

    g_umq_perf_data_enable = false;
    return UMQ_SUCCESS;
}
