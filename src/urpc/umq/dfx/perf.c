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
#include "umq_perf_hdr.h"
#include "perf.h"

#define UMQ_PERF_IO_DIRECTION_ALL_OFFSET     (0)
#define UMQ_PERF_IO_DIRECTION_TX_OFFSET      (1)
#define UMQ_PERF_IO_DIRECTION_RX_OFFSET      (2)

#define UMQ_PERF_INTERRUPT_DIRECTION_TX_OFFSET      (0)
#define UMQ_PERF_INTERRUPT_DIRECTION_RX_OFFSET      (1)

/* hardcoded quantile percentages: p50, p90, p99, p9999 */
static const double g_quantile_pct[UMQ_PERF_QUANTILE_CNT] = {
    50.0,   /* p50  */
    90.0,   /* p90  */
    99.0,   /* p99  */
    99.99   /* p9999 */
};

static __thread uint32_t g_perf_record_index = -1;
static __thread pthread_once_t g_dp_thread_run_once = PTHREAD_ONCE_INIT;
static bool g_umq_perf_record_enable = false;
static umq_perf_stats_cfg_t g_umq_perf_cfg = {
    .sig_figures = UMQ_PERF_HDR_DEFAULT_SIG_FIGURES,
    .max_value_ms = UMQ_PERF_HDR_DEFAULT_MAX_MS
};
static uint64_t g_umq_perf_hdr_max_cycles = 0;

typedef struct umq_perf_record {
    struct {
        umq_perf_record_type_t type; // types of probe points supported by perf probe
        uint64_t accumulation; // total latency
        uint64_t min; // min latency
        uint64_t max; // max latency
        uint64_t cnt; // statistical count
        umq_perf_hdr_t *hdr; // hdr histogram for quantile estimation
    } type_record[UMQ_PERF_RECORD_TYPE_MAX]; // statistical results list for each type of probe point
    bool is_used; // the statistic item valid
} umq_perf_record_t;

typedef struct umq_perf_record_ctx {
    umq_perf_record_t perf_record_table[UMQ_PERF_REC_MAX_NUM];
    pthread_once_t *dp_thread_run_once[UMQ_PERF_REC_MAX_NUM];
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
    g_umq_perf_hdr_max_cycles = (uint64_t)g_umq_perf_cfg.max_value_ms * urpc_get_cpu_hz() / UMQ_PERF_HDR_MS_PER_SEC;
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
            if (rec->type_record[type].hdr != NULL) {
                umq_perf_hdr_destroy(rec->type_record[type].hdr);
                rec->type_record[type].hdr = NULL;
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
        if (cur_record->type_record[type].hdr != NULL) {
            umq_perf_hdr_reset(cur_record->type_record[type].hdr);
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
        if (cur->type_record[type].hdr != NULL) {
            umq_perf_hdr_destroy(cur->type_record[type].hdr);
            cur->type_record[type].hdr = NULL;
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

// log hdr allocation failure once per thread to avoid flooding the hot path
static void umq_perf_log_hdr_alloc_fail(umq_perf_record_type_t type)
{
    static __thread bool hdr_alloc_failed = false;
    if (!hdr_alloc_failed) {
        hdr_alloc_failed = true;
        UMQ_VLOG_ERR(VLOG_UMQ, "umq_perf_hdr_create failed, quantile data of type %d will be missing\n", type);
    }
}

static inline umq_perf_hdr_t *umq_perf_ensure_hdr(umq_perf_record_t *rec, umq_perf_record_type_t type)
{
    umq_perf_hdr_t *h = rec->type_record[type].hdr;
    if (h == NULL) {
        h = umq_perf_hdr_create(g_umq_perf_cfg.sig_figures, g_umq_perf_hdr_max_cycles);
        if (h == NULL) {
            umq_perf_log_hdr_alloc_fail(type);
            return NULL;
        }
        rec->type_record[type].hdr = h;
    }
    return h;
}

static inline void umq_perf_fill_perf_record(umq_perf_record_type_t type, uint64_t start)
{
    uint64_t delta = urpc_get_cpu_cycles() - start;
    umq_perf_record_t *cur_rec = &g_umq_perf_record_ctx->perf_record_table[g_perf_record_index];
    cur_rec->type_record[type].accumulation += delta;
    (delta < cur_rec->type_record[type].min) ? cur_rec->type_record[type].min = delta : 0;
    (delta > cur_rec->type_record[type].max) ? cur_rec->type_record[type].max = delta : 0;
    ++cur_rec->type_record[type].cnt;

    umq_perf_hdr_t *h = umq_perf_ensure_hdr(cur_rec, type);
    if (h != NULL) {
        umq_perf_hdr_record(h, delta);
    }
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

int umq_perf_reset(const umq_perf_stats_cfg_t *cfg)
{
    if (g_umq_perf_record_ctx == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "invalid parameter\n");
        return -UMQ_ERR_EINVAL;
    }

    if (cfg != NULL) {
        g_umq_perf_cfg = *cfg;
        if (!umq_perf_hdr_sig_figures_valid(g_umq_perf_cfg.sig_figures)) {
            g_umq_perf_cfg.sig_figures = UMQ_PERF_HDR_DEFAULT_SIG_FIGURES;
        }
        if (g_umq_perf_cfg.max_value_ms == 0) {
            g_umq_perf_cfg.max_value_ms = UMQ_PERF_HDR_DEFAULT_MAX_MS;
        }
        g_umq_perf_hdr_max_cycles = (uint64_t)g_umq_perf_cfg.max_value_ms * urpc_get_cpu_hz() / UMQ_PERF_HDR_MS_PER_SEC;
    }

    for (uint32_t i = 0; i < UMQ_PERF_REC_MAX_NUM; ++i) {
        umq_clear_perf_record_item(i);
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
        if (perf_record->type_record[i].hdr != NULL) {
            if (total_perf_record->type_record[i].hdr == NULL) {
                total_perf_record->type_record[i].hdr = umq_perf_hdr_create(g_umq_perf_cfg.sig_figures,
                                                                             g_umq_perf_hdr_max_cycles);
            }
            if (total_perf_record->type_record[i].hdr != NULL) {
                umq_perf_hdr_merge(total_perf_record->type_record[i].hdr, perf_record->type_record[i].hdr);
            }
        }
    }
}

int umq_perf_info_get(umq_perf_stats_t *perf_info)
{
    if (g_umq_perf_record_ctx == NULL || perf_info == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "invalid parameter\n");
        return -UMQ_ERR_EINVAL;
    }

    (void)pthread_spin_lock(&g_umq_perf_record_lock);

    umq_perf_record_t total_perf_record = {0};
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

        // compute hardcoded quantiles: p50, p90, p99, p9999
        if (cnt > 0 && total_perf_record.type_record[i].hdr != NULL) {
            umq_perf_hdr_t *h = total_perf_record.type_record[i].hdr;
            for (uint32_t j = 0; j < UMQ_PERF_QUANTILE_CNT; j++) {
                uint64_t val_ns = cpu_cycles_to_ns(umq_perf_hdr_value_at_quantile(h, g_quantile_pct[j]));
                perf_info->type_record[i].quantile_val[j] = val_ns;
            }
        } else {
            for (uint32_t j = 0; j < UMQ_PERF_QUANTILE_CNT; j++) {
                perf_info->type_record[i].quantile_val[j] = 0;
            }
        }

        if (total_perf_record.type_record[i].hdr != NULL) {
            umq_perf_hdr_destroy(total_perf_record.type_record[i].hdr);
            total_perf_record.type_record[i].hdr = NULL;
        }
    }

    (void)pthread_spin_unlock(&g_umq_perf_record_lock);
    return 0;
}
