/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: umq perf
 * Create: 2025-10-29
 */

#include <pthread.h>

#include "umq_errno.h"
#include "urpc_util.h"
#include "umq_vlog.h"
#include "perf.h"
#include <stdarg.h>

#define UMQ_PERF_MAX_THRESH_NS         (100000u)

#define UMQ_PERF_IO_DIRECTION_ALL_OFFSET     (0)
#define UMQ_PERF_IO_DIRECTION_TX_OFFSET      (1)
#define UMQ_PERF_IO_DIRECTION_RX_OFFSET      (2)

#define UMQ_PERFTEST_EQUALS "=========================================================================================\
================================================================================"
#define UMQ_PERFTEST_UNDERLINE "--------------------------------------------------------------------------------------\
-----------------------------------------------------------------------------------"
#define UMQ_PERFTEST_PERF_REC_NAME_MAX_LEN 20 // stay synchronized with the output format
static char g_perf_record_type_name[UMQ_PERF_RECORD_TYPE_MAX][UMQ_PERFTEST_PERF_REC_NAME_MAX_LEN] = {
    "umq_enqueue",
    "umq_dequeue",
    "umq_dequeue_empty",
    "umq_post_all",
    "umq_post_tx",
    "umq_post_rx",
    "umq_poll_all",
    "umq_poll_tx",
    "umq_poll_rx",
    "umq_poll_all_empty",
    "umq_poll_tx_empty",
    "umq_poll_rx_empty",
    "umq_notify",
    "tp_post_send",
    "tp_post_recv",
    "tp_poll_tx",
    "tp_poll_rx",
    "tp_poll_tx_empty",
    "tp_poll_rx_empty",
};

static __thread uint32_t g_perf_record_index = -1;
static __thread pthread_once_t g_dp_thread_run_once = PTHREAD_ONCE_INIT;
static bool g_umq_perf_record_enable = false;

typedef struct umq_perf_record_ctx {
    umq_perf_record_t perf_record_table[UMQ_PERF_REC_MAX_NUM];
    uint64_t perf_quantile_thresh[UMQ_PERF_QUANTILE_MAX_NUM + 1];
    pthread_mutex_t lock;
} umq_perf_record_ctx_t;

static umq_perf_record_ctx_t *g_umq_perf_record_ctx;

int umq_perf_init(void)
{
    if (g_umq_perf_record_ctx != NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "umq perf has been inited\n");
        return -UMQ_ERR_EEXIST;
    }

    g_umq_perf_record_ctx = (umq_perf_record_ctx_t *)calloc(1, sizeof(umq_perf_record_ctx_t));
    if (g_umq_perf_record_ctx == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "calloc for umq_perf_record failed\n");
        return -UMQ_ERR_ENOMEM;
    }
    pthread_mutex_init(&g_umq_perf_record_ctx->lock, NULL);
    return UMQ_SUCCESS;
}

void umq_perf_uninit(void)
{
    if (g_umq_perf_record_ctx == NULL) {
        return;
    }
    g_umq_perf_record_enable = false;
    pthread_mutex_destroy(&g_umq_perf_record_ctx->lock);
    free(g_umq_perf_record_ctx);
    g_umq_perf_record_ctx = NULL;
}

static void umq_clear_perf_record_item(uint32_t record_idx)
{
    umq_perf_record_t *cur_record = &g_umq_perf_record_ctx->perf_record_table[record_idx];
    for (int type = 0; type < UMQ_PERF_RECORD_TYPE_MAX; ++type) {
        cur_record->type_record[type].accumulation = 0;
        cur_record->type_record[type].min = UINT64_MAX;
        cur_record->type_record[type].max = 0;
        cur_record->type_record[type].cnt = 0;
        (void)memset(cur_record->type_record[type].bucket, 0, sizeof(cur_record->type_record[type].bucket));
    }
}

void umq_perf_record_alloc(void)
{
    uint32_t idx;
    (void)pthread_mutex_lock(&g_umq_perf_record_ctx->lock);
    for (idx = 0; idx < UMQ_PERF_REC_MAX_NUM; ++idx) {
        if (!g_umq_perf_record_ctx->perf_record_table[idx].is_used) {
            break;
        }
    }
    if (idx == UMQ_PERF_REC_MAX_NUM) {
        (void)pthread_mutex_unlock(&g_umq_perf_record_ctx->lock);
        UMQ_VLOG_WARN(VLOG_UMQ, "perf_rec table capacity %u were exhausted, alloc perf_rec failed\n",
            UMQ_PERF_REC_MAX_NUM);
        return;
    }

    umq_clear_perf_record_item(idx);
    g_umq_perf_record_ctx->perf_record_table[idx].is_used = true;
    (void)pthread_mutex_unlock(&g_umq_perf_record_ctx->lock);

    g_perf_record_index = idx;
}

static void umq_dp_thread_run_once(void)
{
    umq_perf_record_alloc();
}

uint64_t umq_perf_get_start_timestamp(void)
{
    if (!g_umq_perf_record_enable) {
        return 0;
    }
    pthread_once(&g_dp_thread_run_once, umq_dp_thread_run_once);
    return urpc_get_cpu_cycles();
}

static inline uint32_t find_perf_record_bucket(uint64_t delta)
{
    if (g_umq_perf_record_ctx->perf_quantile_thresh[0] == 0) {
        // quantile thresh is not set, don't fill the bucket
        return UINT32_MAX;
    }
    uint32_t idx;
    for (idx = 0; idx < UMQ_PERF_QUANTILE_MAX_NUM; ++idx) {
        if (delta <= g_umq_perf_record_ctx->perf_quantile_thresh[idx]) {
            break;
        }
    }
    return idx;
}

static inline void umq_perf_fill_perf_record(umq_perf_record_type_t type, uint64_t start)
{
    uint64_t delta = urpc_get_cpu_cycles() - start;
    umq_perf_record_t *cur_rec = &g_umq_perf_record_ctx->perf_record_table[g_perf_record_index];
    cur_rec->type_record[type].accumulation += delta;
    (delta < cur_rec->type_record[type].min) ? cur_rec->type_record[type].min = delta : 0;
    (delta > cur_rec->type_record[type].max) ? cur_rec->type_record[type].max = delta : 0;
    uint32_t bucket_idx = find_perf_record_bucket(delta);
    if (bucket_idx != UINT32_MAX) {
        ++cur_rec->type_record[type].bucket[bucket_idx];
    }
    ++cur_rec->type_record[type].cnt;
}

void umq_perf_record_write(umq_perf_record_type_t type, uint64_t start)
{
    if (!g_umq_perf_record_enable || start == 0 || g_perf_record_index >= UMQ_PERF_REC_MAX_NUM) {
        return;
    }
    umq_perf_fill_perf_record(type, start);
}

void umq_perf_record_write_with_direction(umq_perf_record_type_t type, uint64_t start, umq_io_direction_t direction)
{
    if (!g_umq_perf_record_enable || start == 0 || g_perf_record_index >= UMQ_PERF_REC_MAX_NUM) {
        return;
    }

    static const umq_perf_record_type_t perf_record_type_map[UMQ_IO_MAX] = {
        [UMQ_IO_ALL] = UMQ_PERF_IO_DIRECTION_ALL_OFFSET,
        [UMQ_IO_TX]  = UMQ_PERF_IO_DIRECTION_TX_OFFSET,
        [UMQ_IO_RX]  = UMQ_PERF_IO_DIRECTION_RX_OFFSET,
    };
    umq_perf_fill_perf_record(type + perf_record_type_map[direction], start);
}

int umq_perf_start(void)
{
    // initialize perf at first start
    if (g_umq_perf_record_ctx == NULL) {
        int ret = umq_perf_init();
        if (ret != UMQ_SUCCESS) {
            UMQ_VLOG_ERR(VLOG_UMQ, "umq perf init failed\n");
            return ret;
        }
    }

    // IO perf record has been started, user must stop it first before restart
    if (g_umq_perf_record_enable) {
        UMQ_VLOG_ERR(VLOG_UMQ, "invalid parameter\n");
        return -UMQ_ERR_EINVAL;
    }

    g_umq_perf_record_enable = true;
    return UMQ_SUCCESS;
}

int umq_perf_reset(umq_perf_stats_cfg_t *perf_stats_cfg)
{
    // IO perf record has been started, user must stop it first before restart
    if (g_umq_perf_record_ctx == NULL || perf_stats_cfg == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "invalid parameter\n");
        return -UMQ_ERR_EINVAL;
    }

    for (uint32_t i = 0; i < UMQ_PERF_REC_MAX_NUM; ++i) {
        umq_clear_perf_record_item(i);
    }

    uint32_t thresh_num = perf_stats_cfg->thresh_num;
    if (thresh_num > UMQ_PERF_QUANTILE_MAX_NUM) {
        UMQ_VLOG_ERR(VLOG_UMQ, "configured thresh num %u exceeds the max thresh_num %u, only the minimum %d of them"
            "are used\n", thresh_num, UMQ_PERF_QUANTILE_MAX_NUM, UMQ_PERF_QUANTILE_MAX_NUM);
        return -UMQ_ERR_EAGAIN;
    }

    uint64_t *thresh_array = perf_stats_cfg->thresh_array;
    // set quantile bucket
    uint32_t idx = 0;
    for (uint32_t i = 0; i < thresh_num; ++i) {
        if (thresh_array[i] > UMQ_PERF_MAX_THRESH_NS) {
            continue;
        }
        if (idx == 0 || thresh_array[i] > g_umq_perf_record_ctx->perf_quantile_thresh[idx - 1]) {
            g_umq_perf_record_ctx->perf_quantile_thresh[idx++] = thresh_array[i] * urpc_get_cpu_hz() / NS_PER_SEC;
        }
    }

    return UMQ_SUCCESS;
}

int umq_perf_stop(void)
{
    if (g_umq_perf_record_ctx == NULL || !g_umq_perf_record_enable) {
        UMQ_VLOG_ERR(VLOG_UMQ, "invalid parameter\n");
        return -UMQ_ERR_EINVAL;
    }

    g_umq_perf_record_enable = false;
    return UMQ_SUCCESS;
}

static inline uint64_t cpu_cycles_to_ns(uint64_t cycles)
{
    // The CPU frequency is around X GHz, so dividing by CPU hz will solve the overflow issue.
    if (cycles != 0 && UINT64_MAX / cycles <= NS_PER_SEC) {
        return cycles / urpc_get_cpu_hz() * NS_PER_SEC;
    } else {
        return cycles * NS_PER_SEC / urpc_get_cpu_hz();
    }
}

static inline void umq_perf_convert_cycles_to_ns(umq_perf_record_t *perf_rec)
{
    for (int type = 0; type < UMQ_PERF_RECORD_TYPE_MAX; ++type) {
        perf_rec->type_record[type].accumulation = cpu_cycles_to_ns(perf_rec->type_record[type].accumulation);
        // min default value is inited as UINT64_MAX, we output it as 0 for readability
        perf_rec->type_record[type].min =
            perf_rec->type_record[type].min == UINT64_MAX ? 0 : cpu_cycles_to_ns(perf_rec->type_record[type].min);
        perf_rec->type_record[type].max = cpu_cycles_to_ns(perf_rec->type_record[type].max);
    }
}

int umq_perf_info_get(umq_perf_stats_t *perf_info)
{
    if (g_umq_perf_record_ctx == NULL || perf_info == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "invalid parameter\n");
        return -UMQ_ERR_EINVAL;
    }

    (void)pthread_mutex_lock(&g_umq_perf_record_ctx->lock);

    perf_info->perf_record_num = 0;
    for (uint32_t i = 0; i < UMQ_PERF_REC_MAX_NUM; ++i) {
        if (!g_umq_perf_record_ctx->perf_record_table[i].is_used) {
            continue;
        }
        
        perf_info->perf_record[i] = g_umq_perf_record_ctx->perf_record_table[i];
        perf_info->perf_record_num++;
        umq_perf_convert_cycles_to_ns(&perf_info->perf_record[i]);
    }

    (void)pthread_mutex_unlock(&g_umq_perf_record_ctx->lock);
    return 0;
}

static inline int umq_write_perf_recode_msg(char *str_buf, int all_str_len, const char *format, ...)
{
    if (all_str_len <= 0) {
        // Return the length of the string that should be written
        all_str_len = 0;
    }

    va_list args;
    va_start(args, format);
    int ret = vsnprintf(str_buf, all_str_len, format, args);
    va_end(args);
    return ret;
}

static uint64_t umq_perf_cal_quantile(
    umq_perf_record_t *record, umq_perf_record_type_t type, uint64_t count, uint64_t *thresh, uint32_t thresh_num)
{
    if (thresh_num == 0) {
        return 0;
    }

    uint32_t idx;
    uint64_t quantile_cnt = count;
    for (idx = 0; idx < thresh_num; ++idx) {
        if (record->type_record[type].bucket[idx] >= quantile_cnt) {
            break;
        }
        quantile_cnt -= record->type_record[type].bucket[idx];
    }

    // the queried quantile cnt exceeds the maximum thresh records, return the max thresh
    if (idx >= thresh_num) {
        return thresh[thresh_num - 1];
    }

    if (record->type_record[type].bucket[idx] == 0) {
        return 0;
    }

    uint64_t base = (idx == 0) ? 0 : thresh[idx - 1];
    return ((double)quantile_cnt / record->type_record[type].bucket[idx]) * (thresh[idx] - base) + base;
}

static int umq_perf_analyse_and_output(
    char *perf_info_str, int perf_info_size, umq_perf_record_t *perf_rec, uint64_t *thresh, uint32_t thresh_num)
{
    int ret = 0;
    for (int type = 0; type < UMQ_PERF_RECORD_TYPE_MAX; ++type) {
        uint64_t ave_cost = perf_rec->type_record[type].cnt != 0 ?
                            (perf_rec->type_record[type].accumulation / perf_rec->type_record[type].cnt) : 0;
        uint64_t median = umq_perf_cal_quantile(perf_rec, type,
            (uint64_t)(0.5 * perf_rec->type_record[type].cnt), thresh, thresh_num);
        uint64_t p90 = umq_perf_cal_quantile(perf_rec, type,
            (uint64_t)(0.9 * perf_rec->type_record[type].cnt), thresh, thresh_num);
        uint64_t p99 = umq_perf_cal_quantile(perf_rec, type,
            (uint64_t)(0.99 * perf_rec->type_record[type].cnt), thresh, thresh_num);
        ret += umq_write_perf_recode_msg(perf_info_str + ret, perf_info_size - ret,
            "%-20s %-20lu %-20lu %-20lu %-20lu %-20lu %-20lu %-20lu\n",
            g_perf_record_type_name[type], perf_rec->type_record[type].cnt, ave_cost,
            perf_rec->type_record[type].min, perf_rec->type_record[type].max, median, p90, p99);
    }
    return ret;
}

int umq_perf_info_to_string(umq_perf_stats_t *perf_stats, umq_perf_stats_cfg_t *perf_stats_cfg,
    char *umq_perf_stats_buf, int buf_szie)
{
    if (perf_stats_cfg == NULL || perf_stats == NULL || umq_perf_stats_buf == NULL) {
        return -UMQ_ERR_EINVAL;
    }

    if (perf_stats_cfg->thresh_num > UMQ_PERF_QUANTILE_MAX_NUM) {
        UMQ_VLOG_ERR(VLOG_UMQ, "configured thresh num %u exceeds the max thresh_num %u, only the minimum %d of them"
            "are used\n", perf_stats_cfg->thresh_num, UMQ_PERF_QUANTILE_MAX_NUM, UMQ_PERF_QUANTILE_MAX_NUM);
        return -UMQ_ERR_EINVAL;
    }

    // set quantile bucket
    uint32_t idx = 0;
    uint64_t thresh_array[UMQ_PERF_QUANTILE_MAX_NUM + 1];
    for (uint32_t i = 0; i < perf_stats_cfg->thresh_num; ++i) {
        if (perf_stats_cfg->thresh_array[i] > UMQ_PERF_MAX_THRESH_NS) {
            continue;
        }
        if (idx == 0 || perf_stats_cfg->thresh_array[i] > thresh_array[idx - 1]) {
            thresh_array[idx++] = perf_stats_cfg->thresh_array[i];
        }
    }

    int str_size = 0;
    char *ret_str = umq_perf_stats_buf;
    (void)memset(ret_str, 0, buf_szie);

    str_size += umq_write_perf_recode_msg(ret_str + str_size, buf_szie - str_size,
        "%s\n", UMQ_PERFTEST_EQUALS);
    str_size += umq_write_perf_recode_msg(ret_str + str_size, buf_szie - str_size,
        "                                                                    Analyse IO performance records\n");
    str_size += umq_write_perf_recode_msg(ret_str + str_size, buf_szie - str_size,
        "%s\n", UMQ_PERFTEST_EQUALS);
    str_size += umq_write_perf_recode_msg(ret_str + str_size, buf_szie - str_size,
        "%-20s %-20s %-20s %-20s %-20s %-20s %-20s %-20s\n",
        "Type", "Sample Num", "Average (ns)", "Minimum (ns)", "Maxinum (ns)", "Median (ns)", "P90 (ns)", "P99 (ns)");
    str_size += umq_write_perf_recode_msg(ret_str + str_size, buf_szie - str_size,
        "%s\n", UMQ_PERFTEST_UNDERLINE);
    umq_perf_record_t *recv_perf = perf_stats->perf_record;

    // Ananlyse the recved perf data
    for (uint32_t i = 0; i < perf_stats->perf_record_num; i++) {
        if (!recv_perf[i].is_used) {
            continue;
        }
        str_size += umq_write_perf_recode_msg(ret_str + str_size, buf_szie - str_size,
            "                                                                           Data Thread %u\n", i);
        str_size += umq_write_perf_recode_msg(ret_str + str_size, buf_szie - str_size,
            "%s\n", UMQ_PERFTEST_UNDERLINE);
        str_size += umq_perf_analyse_and_output(ret_str + str_size, buf_szie - str_size,
            &recv_perf[i], perf_stats_cfg->thresh_array, perf_stats_cfg->thresh_num);
        str_size += umq_write_perf_recode_msg(ret_str + str_size, buf_szie - str_size,
            "%s\n", UMQ_PERFTEST_UNDERLINE);
    }
    str_size += umq_write_perf_recode_msg(ret_str + str_size, buf_szie - str_size,
        "%s\n", UMQ_PERFTEST_EQUALS);

    return str_size;
}
