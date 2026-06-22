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
#include "urpc_thread_closure.h"
#include "urpc_util.h"
#include "tdigest.h"
#include "perf.h"

#define UMQ_PERF_IO_DIRECTION_ALL_OFFSET     (0)
#define UMQ_PERF_IO_DIRECTION_TX_OFFSET      (1)
#define UMQ_PERF_IO_DIRECTION_RX_OFFSET      (2)

#define UMQ_PERF_INTERRUPT_DIRECTION_TX_OFFSET      (0)
#define UMQ_PERF_INTERRUPT_DIRECTION_RX_OFFSET      (1)

#define UMQ_PERF_DEFAULT_COMPRESSION  (100)
#define UMQ_PERF_DEFAULT_P50          (50.0)
#define UMQ_PERF_DEFAULT_P90          (90.0)
#define UMQ_PERF_DEFAULT_P99          (99.0)
#define UMQ_PERF_DEFAULT_QUANTILE_NUM (3)

static __thread uint32_t g_perf_record_index = -1;
static __thread pthread_once_t g_dp_thread_run_once = PTHREAD_ONCE_INIT;
static bool g_umq_perf_record_enable = false;

typedef struct umq_perf_record {
    struct {
        umq_perf_record_type_t type; // types of probe points supported by perf probe
        uint64_t accumulation; // total latency
        uint64_t min; // min latency
        uint64_t max; // max latency
        uint64_t cnt; // statistical count
        tdigest_t *td; // t-digest for quantile estimation
    } type_record[UMQ_PERF_RECORD_TYPE_MAX]; // statistical results list for each type of probe point
    bool is_used; // the statistic item valid
} umq_perf_record_t;

typedef struct umq_perf_record_ctx {
    umq_perf_record_t perf_record_table[UMQ_PERF_REC_MAX_NUM];
    pthread_once_t *dp_thread_run_once[UMQ_PERF_REC_MAX_NUM];
    double quantile_pct[UMQ_PERF_QUANTILE_MAX_NUM]; // configured quantile percentages
    uint32_t quantile_num;                          // how many are configured (0→use defaults)
    int compression;                                // t-digest compression (0→use default)
} umq_perf_record_ctx_t;

static pthread_spinlock_t g_umq_perf_record_lock;
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
    (void)pthread_spin_init(&g_umq_perf_record_lock, PTHREAD_PROCESS_PRIVATE);
    return UMQ_SUCCESS;
}

void umq_perf_uninit(void)
{
    if (g_umq_perf_record_ctx == NULL) {
        return;
    }

    (void)pthread_spin_lock(&g_umq_perf_record_lock);
    for (uint32_t i = 0; i < UMQ_PERF_REC_MAX_NUM; i++) {
        if (g_umq_perf_record_ctx->dp_thread_run_once[i] != NULL) {
            *g_umq_perf_record_ctx->dp_thread_run_once[i] = PTHREAD_ONCE_INIT;
        }
        umq_perf_record_t *rec = &g_umq_perf_record_ctx->perf_record_table[i];
        for (int type = 0; type < UMQ_PERF_RECORD_TYPE_MAX; ++type) {
            if (rec->type_record[type].td != NULL) {
                tdigest_destroy(rec->type_record[type].td);
                rec->type_record[type].td = NULL;
            }
        }
    }

    g_umq_perf_record_enable = false;
    free(g_umq_perf_record_ctx);
    g_umq_perf_record_ctx = NULL;
    (void)pthread_spin_unlock(&g_umq_perf_record_lock);
    (void)pthread_spin_destroy(&g_umq_perf_record_lock);
}

static void umq_clear_perf_record_item(uint32_t record_idx)
{
    umq_perf_record_t *cur_record = &g_umq_perf_record_ctx->perf_record_table[record_idx];
    for (int type = 0; type < UMQ_PERF_RECORD_TYPE_MAX; ++type) {
        cur_record->type_record[type].accumulation = 0;
        cur_record->type_record[type].min = UINT64_MAX;
        cur_record->type_record[type].max = 0;
        cur_record->type_record[type].cnt = 0;
        if (cur_record->type_record[type].td != NULL) {
            tdigest_reset(cur_record->type_record[type].td);
        }
    }
}

static void umq_perf_record_closure(uint64_t idx)
{
    (void)pthread_spin_lock(&g_umq_perf_record_lock);
    if (g_umq_perf_record_ctx == NULL) {
        (void)pthread_spin_unlock(&g_umq_perf_record_lock);
        return;
    }
    umq_perf_record_t *cur = &g_umq_perf_record_ctx->perf_record_table[idx];
    for (int type = 0; type < UMQ_PERF_RECORD_TYPE_MAX; ++type) {
        if (cur->type_record[type].td != NULL) {
            tdigest_destroy(cur->type_record[type].td);
            cur->type_record[type].td = NULL;
        }
    }
    cur->is_used = false;
    g_umq_perf_record_ctx->dp_thread_run_once[idx] = NULL;
    (void)pthread_spin_unlock(&g_umq_perf_record_lock);
}

void umq_perf_record_alloc(void)
{
    uint32_t idx;
    (void)pthread_spin_lock(&g_umq_perf_record_lock);
    if (g_umq_perf_record_ctx == NULL) {
        (void)pthread_spin_unlock(&g_umq_perf_record_lock);
        UMQ_VLOG_ERR(VLOG_UMQ, "perf record ctx invalid\n");
        return;
    }

    for (idx = 0; idx < UMQ_PERF_REC_MAX_NUM; ++idx) {
        if (!g_umq_perf_record_ctx->perf_record_table[idx].is_used) {
            break;
        }
    }
    if (idx == UMQ_PERF_REC_MAX_NUM) {
        (void)pthread_spin_unlock(&g_umq_perf_record_lock);
        UMQ_VLOG_WARN(VLOG_UMQ, "perf_rec table capacity %u were exhausted, alloc perf_rec failed\n",
            UMQ_PERF_REC_MAX_NUM);
        return;
    }

    umq_clear_perf_record_item(idx);
    // t-digest instances allocated on-demand to avoid pre-allocating
    // digests for record types this thread never uses.
    g_umq_perf_record_ctx->perf_record_table[idx].is_used = true;
    (void)pthread_spin_unlock(&g_umq_perf_record_lock);

    g_perf_record_index = idx;
    g_umq_perf_record_ctx->dp_thread_run_once[idx] = &g_dp_thread_run_once;
    urpc_thread_closure_register(THREAD_CLOSURE_UMQ_PERF, idx, umq_perf_record_closure);
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

static tdigest_t *umq_perf_ensure_tdigest(umq_perf_record_t *rec, umq_perf_record_type_t type)
{
    tdigest_t *td = rec->type_record[type].td;
    if (td == NULL) {
        td = tdigest_create(g_umq_perf_record_ctx->compression);
        rec->type_record[type].td = td;
    }
    return td;
}

static inline void umq_perf_fill_perf_record(umq_perf_record_type_t type, uint64_t start)
{
    uint64_t delta = urpc_get_cpu_cycles() - start;
    umq_perf_record_t *cur_rec = &g_umq_perf_record_ctx->perf_record_table[g_perf_record_index];
    cur_rec->type_record[type].accumulation += delta;
    (delta < cur_rec->type_record[type].min) ? cur_rec->type_record[type].min = delta : 0;
    (delta > cur_rec->type_record[type].max) ? cur_rec->type_record[type].max = delta : 0;

    tdigest_t *td = umq_perf_ensure_tdigest(cur_rec, type);
    if (td != NULL) {
        tdigest_insert(td, (double)delta, 1);
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
    if (!g_umq_perf_record_enable || start == 0 ||
        g_perf_record_index >= UMQ_PERF_REC_MAX_NUM || direction >= UMQ_IO_MAX) {
        return;
    }

    static const umq_perf_record_type_t perf_record_type_map[UMQ_IO_MAX] = {
        [UMQ_IO_ALL] = UMQ_PERF_IO_DIRECTION_ALL_OFFSET,
        [UMQ_IO_TX]  = UMQ_PERF_IO_DIRECTION_TX_OFFSET,
        [UMQ_IO_RX]  = UMQ_PERF_IO_DIRECTION_RX_OFFSET,
    };
    umq_perf_fill_perf_record(type + perf_record_type_map[direction], start);
}

void umq_perf_record_write_interrupt_with_direction(
    umq_perf_record_type_t type, uint64_t start, umq_io_direction_t direction)
{
    if (!g_umq_perf_record_enable || start == 0 ||
        g_perf_record_index >= UMQ_PERF_REC_MAX_NUM || direction >= UMQ_IO_MAX || direction == UMQ_IO_ALL) {
        return;
    }

    static const umq_perf_record_type_t perf_record_type_map[UMQ_IO_MAX] = {
        [UMQ_IO_TX]  = UMQ_PERF_INTERRUPT_DIRECTION_TX_OFFSET,
        [UMQ_IO_RX]  = UMQ_PERF_INTERRUPT_DIRECTION_RX_OFFSET,
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
    if (g_umq_perf_record_ctx == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "invalid parameter\n");
        return -UMQ_ERR_EINVAL;
    }

    for (uint32_t i = 0; i < UMQ_PERF_REC_MAX_NUM; ++i) {
        umq_clear_perf_record_item(i);
    }

    // parse config; fall back to defaults if none given
    if (perf_stats_cfg != NULL && perf_stats_cfg->quantile_num > 0) {
        uint32_t n = perf_stats_cfg->quantile_num;
        if (n > UMQ_PERF_QUANTILE_MAX_NUM) {
            UMQ_VLOG_WARN(VLOG_UMQ,
                "quantile_num %u exceeds max %u, clamping\n",
                n, UMQ_PERF_QUANTILE_MAX_NUM);
            n = UMQ_PERF_QUANTILE_MAX_NUM;
        }
        for (uint32_t i = 0; i < n; i++) {
            if (perf_stats_cfg->quantile_pct[i] <= 0.0 ||
                perf_stats_cfg->quantile_pct[i] > TDIGEST_PCT_MAX) {
                UMQ_VLOG_WARN(VLOG_UMQ,
                    "quantile_pct[%u]=%.1f out of range (0,100], using defaults\n",
                    i, perf_stats_cfg->quantile_pct[i]);
                n = 0;
                break;
            }
            g_umq_perf_record_ctx->quantile_pct[i] =
                perf_stats_cfg->quantile_pct[i];
        }
        g_umq_perf_record_ctx->quantile_num = n;
    }

    if (g_umq_perf_record_ctx->quantile_num == 0) {
        // Use built-in defaults when no valid user config was provided.
        g_umq_perf_record_ctx->quantile_pct[0] = UMQ_PERF_DEFAULT_P50;
        g_umq_perf_record_ctx->quantile_pct[1] = UMQ_PERF_DEFAULT_P90;
        g_umq_perf_record_ctx->quantile_pct[UMQ_PERF_DEFAULT_QUANTILE_NUM - 1] = UMQ_PERF_DEFAULT_P99;
        g_umq_perf_record_ctx->quantile_num = UMQ_PERF_DEFAULT_QUANTILE_NUM;
    }

    g_umq_perf_record_ctx->compression =
        (perf_stats_cfg != NULL && perf_stats_cfg->compression > 0)
        ? perf_stats_cfg->compression
        : UMQ_PERF_DEFAULT_COMPRESSION;

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

static ALWAYS_INLINE void umq_perf_record_add(umq_perf_record_t *total_perf_record, umq_perf_record_t *perf_record)
{
    for (uint32_t i = 0; i < UMQ_PERF_RECORD_TYPE_MAX; i++) {
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
        if (total_perf_record->type_record[i].td != NULL && perf_record->type_record[i].td != NULL) {
            tdigest_merge(total_perf_record->type_record[i].td, perf_record->type_record[i].td);
        }
    }
}

int umq_perf_info_get(umq_perf_stats_t *perf_info)
{
    if (g_umq_perf_record_ctx == NULL || perf_info == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "invalid parameter\n");
        return -UMQ_ERR_EINVAL;
    }

    umq_perf_record_t total_perf_record = {0};
    for (int i = 0; i < UMQ_PERF_RECORD_TYPE_MAX; i++) {
        total_perf_record.type_record[i].td = tdigest_create(g_umq_perf_record_ctx->compression);
    }

    (void)pthread_spin_lock(&g_umq_perf_record_lock);

    for (uint32_t i = 0; i < UMQ_PERF_REC_MAX_NUM; ++i) {
        if (!g_umq_perf_record_ctx->perf_record_table[i].is_used) {
            continue;
        }

        umq_perf_record_add(&total_perf_record, &g_umq_perf_record_ctx->perf_record_table[i]);
    }
    umq_perf_convert_cycles_to_ns(&total_perf_record);

    for (uint32_t i = 0; i < UMQ_PERF_RECORD_TYPE_MAX; i++) {
        uint64_t cnt = total_perf_record.type_record[i].cnt;
        perf_info->type_record[i].type = i;
        perf_info->type_record[i].sample_num = cnt;
        perf_info->type_record[i].average = cnt != 0 ?
            (total_perf_record.type_record[i].accumulation / cnt) : 0;
        perf_info->type_record[i].maxinum = total_perf_record.type_record[i].max;
        perf_info->type_record[i].mininum = total_perf_record.type_record[i].min;

        // Copy the resolved quantile configuration to the output.
        uint32_t qn = g_umq_perf_record_ctx->quantile_num;
        for (uint32_t j = 0; j < qn; j++) {
            perf_info->type_record[i].quantile_pct[j] =
                g_umq_perf_record_ctx->quantile_pct[j];
        }
        perf_info->type_record[i].quantile_num = qn;

        // 0 if no samples
        if (cnt > 0 && total_perf_record.type_record[i].td != NULL) {
            tdigest_t *td = total_perf_record.type_record[i].td;
            for (uint32_t j = 0; j < qn; j++) {
                uint64_t val_ns = cpu_cycles_to_ns(
                    (uint64_t)tdigest_quantile(td,
                        perf_info->type_record[i].quantile_pct[j]));
                perf_info->type_record[i].quantile_val[j] = val_ns;
            }
        }

        if (total_perf_record.type_record[i].td != NULL) {
            tdigest_destroy(total_perf_record.type_record[i].td);
            total_perf_record.type_record[i].td = NULL;
        }
    }

    (void)pthread_spin_unlock(&g_umq_perf_record_lock);
    return 0;
}
