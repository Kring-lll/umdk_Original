#ifndef URMA_PERF_H
#define URMA_PERF_H

#include <stdint.h>
#include <stdbool.h>
#include <threads.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum urma_perf_type {
    UB_JETTY_POST_SEND,
    BOND_JETTY_POST_SEND,
    UB_JFS_POST_SEND,
    BOND_JFS_POST_SEND,
    UB_JETTY_POST_RECV,
    BOND_JETTY_POST_RECV,
    UB_POST_JFR_RECV,
    BOND_POST_JFR_RECV,
    UB_POLL_JFC,
    BOND_POLL_JFC,
    UB_WAIT_JFC,
    BOND_WAIT_JFC,
    UB_ACK_JFC,
    UB_REARM_JFC,
    BOND_REARM_JFC,
    URMA_PERF_RECORD_TYPE_MAX,
} urma_perf_record_type_t;

typedef struct urma_perf_stats {
    struct {
        urma_perf_record_type_t type;
        uint64_t sample_num;
        uint64_t average;
        uint64_t mininum;
        uint64_t maxinum;
        uint64_t p90;
        uint64_t p99;
    } type_record[URMA_PERF_RECORD_TYPE_MAX];
} urma_perf_stats_t;

typedef struct {
    int64_t lowest_trackable_value;
    int64_t highest_trackable_value;
    int32_t significant_figures;
    int32_t unit_magnitude;
    int32_t sub_bucket_half_count_magnitude;
    int32_t sub_bucket_half_count;
    int32_t sub_bucket_mask;
    int32_t sub_bucket_count;
    int32_t bucket_count;
    int64_t total_count;
    int64_t min_value;
    int64_t max_value;
    int64_t *counts;
} HdrHistogramData;

typedef struct {
    HdrHistogramData* histograms[URMA_PERF_RECORD_TYPE_MAX];
    urma_perf_stats_t stats;
    int32_t is_running;
} UrmaPerfContext;

extern thread_local UrmaPerfContext* g_perf_ctx;

#define PERF_PROFILING_START(type) \
    uint64_t __perf_start_##type = 0; \
    do { \
        if (g_perf_ctx != NULL && g_perf_ctx->is_running == 1) { \
            __perf_start_##type = get_cycles(); \
        } \
    } while (0)

#define PERF_PROFILING_END(type) \
    do { \
        if (g_perf_ctx != NULL && g_perf_ctx->is_running == 1) { \
            uint64_t _perf_end = get_cycles(); \
            double _perf_mhz = get_cpu_mhz(false); \
            uint64_t _perf_elapsed = (_perf_end - __perf_start_##type) / _perf_mhz; \
            urma_step_perf(type, _perf_elapsed); \
        } \
    } while (0)

urma_status_t urma_start_perf(void);
urma_status_t urma_stop_perf(void);
void urma_step_perf(urma_perf_record_type_t type, uint64_t latency);
urma_status_t urma_get_perf_info(char *perf_buf, uint32_t *length);
void urma_perf_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif
