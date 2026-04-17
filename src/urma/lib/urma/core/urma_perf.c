/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2021-2025. All rights reserved.
 * Description: Performance monitoring and profiling for URMA
 * Author: Tang Zhedong
 * Create: 2026-04-08
 * Note:
 * History: 2026-04-08 create file
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

#include "urma_log.h"
#include "ub_get_clock.h"
#include "urma_perf.h"
#include "urma_private.h"

typedef struct urma_perf_record {
    struct {
        uint64_t accumulation;
        uint64_t min;
        uint64_t max;
        uint64_t cnt;
        uint64_t bucket[URMA_PERF_BUCKET_MAX_NUM + 1];
    } type_record[URMA_PERF_RECORD_TYPE_MAX];
    bool is_used;
} urma_perf_record_t;

typedef struct urma_perf_record_ctx {
    urma_perf_record_t record_table[URMA_PERF_THREAD_MAX_NUM];
    pthread_once_t *thread_run_once[URMA_PERF_THREAD_MAX_NUM];
    uint64_t thresh_cycle[URMA_PERF_BUCKET_MAX_NUM];
    uint64_t thresh_ns[URMA_PERF_BUCKET_MAX_NUM];
    uint32_t thresh_num;
} urma_perf_record_ctx_t;

static const char* perf_type_names[] = {
    "UB_JETTY_POST_SEND",
    "BOND_JETTY_POST_SEND",
    "UB_JFS_POST_SEND",
    "BOND_JFS_POST_SEND",
    "UB_JETTY_POST_RECV",
    "BOND_JETTY_POST_RECV",
    "UB_POST_JFR_RECV",
    "BOND_POST_JFR_RECV",
    "UB_POLL_JFC",
    "BOND_POLL_JFC",
    "UB_WAIT_JFC",
    "BOND_WAIT_JFC",
    "UB_ACK_JFC",
    "UB_REARM_JFC",
    "BOND_REARM_JFC"
};

static uint64_t g_cpu_hz = 0;
static __thread uint32_t g_perf_record_index = (uint32_t)-1;
static __thread pthread_once_t g_thread_run_once = PTHREAD_ONCE_INIT;
static pthread_spinlock_t g_urma_perf_record_lock;
bool g_urma_perf_record_enable = false;
static urma_perf_record_ctx_t *g_urma_perf_record_ctx = NULL;

static inline uint64_t get_cpu_hz_cached(void)
{
    if (g_cpu_hz == 0) {
        g_cpu_hz = get_cpu_mhz(false) * 1000000UL;
    }
    return g_cpu_hz;
}

static inline uint64_t ns_to_cycles(uint64_t ns)
{
    return ns * get_cpu_hz_cached() / 1000000000UL;
}

static inline uint64_t cycles_to_ns(uint64_t cycles)
{
    if (cycles != 0 && UINT64_MAX / cycles <= 1000000000UL) {
        return cycles / get_cpu_hz_cached() * 1000000000UL;
    } else {
        return cycles * 1000000000UL / get_cpu_hz_cached();
    }
}

static int urma_perf_init(void)
{
    if (g_urma_perf_record_ctx != NULL) {
        return 0;
    }
    g_urma_perf_record_ctx = (urma_perf_record_ctx_t *)calloc(1, sizeof(urma_perf_record_ctx_t));
    if (g_urma_perf_record_ctx == NULL) {
        URMA_LOG_ERR("Failed to allocate performance record context");
        return -1;
    }
    (void)pthread_spin_init(&g_urma_perf_record_lock, PTHREAD_PROCESS_PRIVATE);
    return 0;
}

static void urma_perf_uninit(void)
{
    if (g_urma_perf_record_ctx == NULL) {
        return;
    }

    (void)pthread_spin_lock(&g_urma_perf_record_lock);
    for (uint32_t i = 0; i < URMA_PERF_THREAD_MAX_NUM; i++) {
        if (g_urma_perf_record_ctx->thread_run_once[i] != NULL) {
            *g_urma_perf_record_ctx->thread_run_once[i] = PTHREAD_ONCE_INIT;
        }
    }
    g_urma_perf_record_enable = false;
    free(g_urma_perf_record_ctx);
    g_urma_perf_record_ctx = NULL;
    (void)pthread_spin_unlock(&g_urma_perf_record_lock);
    (void)pthread_spin_destroy(&g_urma_perf_record_lock);
}

static void urma_init_perf_record_item(uint32_t record_idx) {
    uint64_t context_threshold[URMA_PERF_BUCKET_MAX_NUM] = {
        20, 30, 40, 50, 60, 70, 80, 90, 100,
        150, 200, 250, 300, 350, 400, 450, 500, 600, 650, 700, 750, 800, 850, 900, 950, 1000,
        2000, 3000, 4000, 8000, 16000, 32000};

    g_perf_record_index = record_idx;
    g_urma_perf_record_ctx->record_table[record_idx].is_used = true;
    g_urma_perf_record_ctx->thread_run_once[record_idx] = &g_thread_run_once;
    g_urma_perf_record_ctx->thresh_num = URMA_PERF_BUCKET_MAX_NUM;
    for (int i = 0; i < URMA_PERF_BUCKET_MAX_NUM; ++i) {
        g_urma_perf_record_ctx->thresh_ns[i] = context_threshold[i];
        g_urma_perf_record_ctx->thresh_cycle[i] = ns_to_cycles(context_threshold[i]);
    }
}
static void urma_clear_perf_record_item(uint32_t record_idx)
{
    urma_perf_record_t *cur_record = &g_urma_perf_record_ctx->record_table[record_idx];
    for (int type = 0; type < URMA_PERF_RECORD_TYPE_MAX; ++type) {
        cur_record->type_record[type].accumulation = 0;
        cur_record->type_record[type].min = UINT64_MAX;
        cur_record->type_record[type].max = 0;
        cur_record->type_record[type].cnt = 0;
        (void)memset(cur_record->type_record[type].bucket, 0, sizeof(cur_record->type_record[type].bucket));
    }
}

static void urma_init_perf_record(void)
{
    uint32_t idx;
    (void)pthread_spin_lock(&g_urma_perf_record_lock);
    if (g_urma_perf_record_ctx == NULL) {
        (void)pthread_spin_unlock(&g_urma_perf_record_lock);
        return;
    }

    for (idx = 0; idx < URMA_PERF_THREAD_MAX_NUM; ++idx) {
        if (!g_urma_perf_record_ctx->record_table[idx].is_used) {
            break;
        }
    }
    if (idx == URMA_PERF_THREAD_MAX_NUM) {
        (void)pthread_spin_unlock(&g_urma_perf_record_lock);
        return;
    }

    urma_clear_perf_record_item(idx);
    urma_init_perf_record_item(idx);
    (void)pthread_spin_unlock(&g_urma_perf_record_lock);
}

static void urma_thread_run_once_func(void)
{
    urma_init_perf_record();
}

static inline void urma_perf_fill_perf_record(urma_perf_record_type_t type, uint64_t delta)
{
    uint32_t bucket_idx;
    urma_perf_record_t *cur_record = &g_urma_perf_record_ctx->record_table[g_perf_record_index];

    if (delta < g_urma_perf_record_ctx->thresh_cycle[0]) {
        URMA_LOG_WARN("Urma perf type %d delta is %lu, less than threshold %lu. \n", 
            type, delta, g_urma_perf_record_ctx->thresh_cycle[0]);
        return;
    }

    for (bucket_idx = 0; bucket_idx < URMA_PERF_BUCKET_MAX_NUM; ++bucket_idx) {
        if (delta <= g_urma_perf_record_ctx->thresh_cycle[bucket_idx]) {
            break;
        }
    }
    cur_record->type_record[type].bucket[bucket_idx]++;
    cur_record->type_record[type].cnt++;
    cur_record->type_record[type].accumulation += delta;
    (delta < cur_record->type_record[type].min) ? cur_record->type_record[type].min = delta : 0;
    (delta > cur_record->type_record[type].max) ? cur_record->type_record[type].max = delta : 0;
}

static uint64_t urma_perf_cal_quantile(urma_perf_record_t *record, urma_perf_record_type_t type,
    uint64_t quantile_count)
{
    uint32_t bucket_idx = 0;
    uint64_t base;
    uint64_t cur_count = quantile_count;
    uint64_t *thresh_arr = g_urma_perf_record_ctx->thresh_ns;
    uint32_t thresh_num = g_urma_perf_record_ctx->thresh_num;


    for (bucket_idx = 0; bucket_idx < thresh_num; ++bucket_idx) {
        if (record->type_record[type].bucket[bucket_idx] >= cur_count) {
            break;
        }
        cur_count -= record->type_record[type].bucket[bucket_idx];
    }
    // outlier handling
    if (bucket_idx >= thresh_num) {
        return thresh_arr[thresh_num - 1];
    }
    base = (bucket_idx == 0) ? 0 : thresh_arr[bucket_idx - 1];
    return ((double)cur_count / record->type_record[type].bucket[bucket_idx]) * (thresh_arr[bucket_idx] - base) + base;
}

static void urma_perf_record_merge(urma_perf_record_t *total_perf_record, urma_perf_record_t *perf_record)
{
    for (uint32_t i = 0; i < URMA_PERF_RECORD_TYPE_MAX; i++) {
        if (perf_record->type_record[i].cnt == 0) {
            continue;
        }
        total_perf_record->type_record[i].accumulation += perf_record->type_record[i].accumulation;
        total_perf_record->type_record[i].min =
            (total_perf_record->type_record[i].min != 0 &&
            total_perf_record->type_record[i].min < perf_record->type_record[i].min) ?
            total_perf_record->type_record[i].min : perf_record->type_record[i].min;
        total_perf_record->type_record[i].max =
            total_perf_record->type_record[i].max > perf_record->type_record[i].max ?
            total_perf_record->type_record[i].max : perf_record->type_record[i].max;
        total_perf_record->type_record[i].cnt += perf_record->type_record[i].cnt;
        for (uint32_t j = 0; j <= URMA_PERF_BUCKET_MAX_NUM; j++) {
            total_perf_record->type_record[i].bucket[j] += perf_record->type_record[i].bucket[j];
        }
    }
}

static void urma_perf_convert_cycles_to_ns(urma_perf_record_t *perf_record)
{
    for (uint32_t i = 0; i < URMA_PERF_RECORD_TYPE_MAX; i++) {
        perf_record->type_record[i].accumulation = cycles_to_ns(perf_record->type_record[i].accumulation);
        perf_record->type_record[i].min =
            perf_record->type_record[i].min == UINT64_MAX ? 0 : cycles_to_ns(perf_record->type_record[i].min);
        perf_record->type_record[i].max = cycles_to_ns(perf_record->type_record[i].max);
    }
}
static int urma_perf_info_get_internal(urma_perf_stats_t *perf_info)
{
    urma_perf_record_t total_perf_record = {0};

    for (uint32_t i = 0; i < URMA_PERF_THREAD_MAX_NUM; ++i) {
        if (!g_urma_perf_record_ctx->record_table[i].is_used) {
            continue;
        }
        urma_perf_record_merge(&total_perf_record, &g_urma_perf_record_ctx->record_table[i]);
    }
    urma_perf_convert_cycles_to_ns(&total_perf_record);

    for (uint32_t i = 0; i < URMA_PERF_RECORD_TYPE_MAX; i++) {
        perf_info->type_record[i].type = (urma_perf_record_type_t)i;
        perf_info->type_record[i].sample_num = total_perf_record.type_record[i].cnt;
        perf_info->type_record[i].average = total_perf_record.type_record[i].cnt != 0 ?
            (total_perf_record.type_record[i].accumulation / total_perf_record.type_record[i].cnt) : 0;
        perf_info->type_record[i].maxinum = total_perf_record.type_record[i].max;
        perf_info->type_record[i].mininum = total_perf_record.type_record[i].min;
        perf_info->type_record[i].p50 = urma_perf_cal_quantile(&total_perf_record, i,
            (uint64_t)(0.5 * total_perf_record.type_record[i].cnt));
        perf_info->type_record[i].p90 = urma_perf_cal_quantile(&total_perf_record, i,
            (uint64_t)(0.9 * total_perf_record.type_record[i].cnt));
        perf_info->type_record[i].p99 = urma_perf_cal_quantile(&total_perf_record, i,
            (uint64_t)(0.99 * total_perf_record.type_record[i].cnt));
    }
    perf_info->retry_count = urma_ubagg_switch_get();

    return 0;
}

bool urma_perf_is_enabled()
{
    return g_urma_perf_record_enable;
}

urma_status_t urma_start_perf(void)
{
    int ret = 0;
    if (urma_perf_is_enabled()) {
        URMA_LOG_ERR("Urma perf start failed. perf record ctx is not NULL. \n");
        return 0;
    }

    ret = urma_perf_init();
    if (ret != 0) {
        return ret;
    }
    g_urma_perf_record_enable = true;
    return 0;
}

urma_status_t urma_stop_perf(void)
{
    if (!urma_perf_is_enabled()) {
        URMA_LOG_ERR("Urma perf stop failed. perf record ctx is NULL. \n");
        return -1;
    }

    urma_perf_uninit();
    g_urma_perf_record_enable = false;
    return 0;
}

urma_status_t urma_config_perf_attr(urma_perf_attr_t *perf_attr)
{
    uint32_t thresh_num;
    uint64_t *thresh_array;
    uint32_t bucket_idx = 0;

    if (!urma_perf_is_enabled()) {
        URMA_LOG_ERR("Urma perf config failed. perf record ctx is NULL. \n");
        return -1;
    }
    if ((perf_attr == NULL) || perf_attr->thresh_num > URMA_PERF_BUCKET_MAX_NUM) {
        URMA_LOG_ERR("Urma perf config failed. perf_attr is invalid. \n");
        return -1;
    }
    thresh_num = perf_attr->thresh_num;
    thresh_array = perf_attr->thresh_array;

    (void)pthread_spin_lock(&g_urma_perf_record_lock);
    for (uint32_t i = 0; i < URMA_PERF_THREAD_MAX_NUM; ++i) {
        urma_clear_perf_record_item(i);
    }
    for (uint32_t i = 0; i < thresh_num; ++i) {
        if (thresh_array[i] > URMA_PERF_MAX_THRESH_NS || thresh_array[i] == 0) {
            URMA_LOG_WARN("Urma perf config failed. thresh: %lu is invalid. \n", thresh_array[i]);
            continue;
        }
        if (bucket_idx == 0 || thresh_array[i] > g_urma_perf_record_ctx->thresh_ns[bucket_idx - 1]) {
            g_urma_perf_record_ctx->thresh_ns[bucket_idx] = thresh_array[i];
            g_urma_perf_record_ctx->thresh_cycle[bucket_idx] = ns_to_cycles(thresh_array[i]);
            bucket_idx++;
        }
    }
    g_urma_perf_record_ctx->thresh_num = bucket_idx;
    (void)pthread_spin_unlock(&g_urma_perf_record_lock);
    return 0;
}

uint64_t urma_get_perf_timestamp(void)
{
    if (!urma_perf_is_enabled()) {
        URMA_LOG_ERR("Urma perf get timestamp failed. perf record is not enabled. \n");
        return 0;
    }
    pthread_once(&g_thread_run_once, urma_thread_run_once_func);
    return get_cycles();
}
void urma_step_perf(urma_perf_record_type_t type, uint64_t delta)
{
    urma_perf_fill_perf_record(type, delta);
}

urma_status_t urma_get_perf_info(char *perf_buf, uint32_t *length)
{
    urma_perf_stats_t perf_info;

    if (perf_buf == NULL || length == NULL || *length < sizeof(urma_perf_stats_t)) {
        URMA_LOG_ERR("Urma perf info get failed, perf_buf or length is invalid\n");
        return -1;
    }

    (void)pthread_spin_lock(&g_urma_perf_record_lock);
    int ret = urma_perf_info_get_internal(&perf_info);
    if (ret != 0) {
        (void)pthread_spin_unlock(&g_urma_perf_record_lock);
        return ret;
    }
    (void)memcpy(perf_buf, &perf_info, sizeof(urma_perf_stats_t));
    (void)pthread_spin_unlock(&g_urma_perf_record_lock);
    return 0;
}

urma_status_t urma_get_perf_info_to_string(void)
{
    urma_perf_stats_t perf_info;

    if (!urma_perf_is_enabled()) {
        URMA_LOG_ERR("Urma perf info get failed. perf record is not enabled. \n");
        return -1;
    }

    (void)pthread_spin_lock(&g_urma_perf_record_lock);
    int ret = urma_perf_info_get_internal(&perf_info);
    if (ret != 0) {
        URMA_LOG_ERR("Urma perf get info failed. ret: %d\n", ret);
        (void)pthread_spin_unlock(&g_urma_perf_record_lock);
        return ret;
    }
    printf("retry_count: %lu\n", perf_info.retry_count);
    printf("+----------------------+----------+----------+----------+----------+----------+----------+----------+\n");
    printf("| Type                 | samples  | avg      | min      | max      | p50      | p90      | p99      |\n");
    printf("+----------------------+----------+----------+----------+----------+----------+----------+----------+\n");
    for (uint32_t i = 0; i < URMA_PERF_RECORD_TYPE_MAX; i++) {
        if (perf_info.type_record[i].sample_num == 0) {
            continue;
        }
        printf("| %-20s | %-8lu | %-8lu | %-8lu | %-8lu | %-8lu | %-8lu | %-8lu |\n",
            perf_type_names[i],
            perf_info.type_record[i].sample_num,
            perf_info.type_record[i].average,
            perf_info.type_record[i].mininum,
            perf_info.type_record[i].maxinum,
            perf_info.type_record[i].p50,
            perf_info.type_record[i].p90,
            perf_info.type_record[i].p99);
    }
    printf("+----------------------+----------+----------+----------+----------+----------+----------+----------+\n");
    (void)pthread_spin_unlock(&g_urma_perf_record_lock);
    return 0;
}