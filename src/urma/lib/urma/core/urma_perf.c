#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <threads.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "urma_api.h"

#define URMA_PERF_MIN_VALUE 1
#define URMA_PERF_MAX_VALUE 3600000000L
#define URMA_PERF_SIGNIFICANT_FIGURES 3

typedef int32_t urma_status_t;
#define URMA_SUCCESS 0
#define URMA_ERROR -1

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

thread_local UrmaPerfContext* g_perf_ctx = NULL;

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

static int32_t get_bucket_index(int64_t value, int32_t unit_magnitude)
{
    int32_t leading_zero_count = __builtin_clzll(value);
    int32_t pow2_ceiling = 64 - leading_zero_count;
    return pow2_ceiling - unit_magnitude;
}

static int32_t get_sub_bucket_index(int64_t value, int32_t bucket_index, int32_t unit_magnitude, int32_t sub_bucket_mask)
{
    int32_t sub_bucket_index = value >> (bucket_index + unit_magnitude);
    return sub_bucket_index + sub_bucket_mask;
}

static int32_t counts_index(int32_t bucket_index, int32_t sub_bucket_index, int32_t sub_bucket_half_count)
{
    return (bucket_index + 1) * (sub_bucket_half_count * 2) + sub_bucket_index;
}

static HdrHistogramData* hdr_init(int64_t lowest_trackable_value, int64_t highest_trackable_value, int32_t significant_figures)
{
    int32_t unit_magnitude = (int32_t)floor(log(lowest_trackable_value) / log(2));
    int32_t sub_bucket_half_count_magnitude = (int32_t)ceil(log(significant_figures * 2.0) / log(2));
    int32_t sub_bucket_half_count = 1 << sub_bucket_half_count_magnitude;
    int32_t sub_bucket_count = sub_bucket_half_count * 2;
    int32_t sub_bucket_mask = sub_bucket_count - 1;
    
    int32_t bucket_count = get_bucket_index(highest_trackable_value, unit_magnitude) + 1;
    
    size_t counts_len = (bucket_count + 1) * sub_bucket_count;
    size_t size = sizeof(HdrHistogramData) + sizeof(int64_t) * counts_len;
    
    HdrHistogramData* data = (HdrHistogramData*)calloc(1, size);
    data->lowest_trackable_value = lowest_trackable_value;
    data->highest_trackable_value = highest_trackable_value;
    data->significant_figures = significant_figures;
    data->unit_magnitude = unit_magnitude;
    data->sub_bucket_half_count_magnitude = sub_bucket_half_count_magnitude;
    data->sub_bucket_half_count = sub_bucket_half_count;
    data->sub_bucket_mask = sub_bucket_mask;
    data->sub_bucket_count = sub_bucket_count;
    data->bucket_count = bucket_count;
    data->min_value = INT64_MAX;
    data->max_value = 0;
    data->counts = (int64_t*)(data + 1);
    
    return data;
}

static void hdr_record_value(HdrHistogramData* data, int64_t value)
{
    if (value < data->lowest_trackable_value) value = data->lowest_trackable_value;
    if (value > data->highest_trackable_value) value = data->highest_trackable_value;
    
    int32_t bucket_index = get_bucket_index(value, data->unit_magnitude);
    int32_t sub_bucket_index = get_sub_bucket_index(value, bucket_index, data->unit_magnitude, data->sub_bucket_mask);
    int32_t index = counts_index(bucket_index, sub_bucket_index, data->sub_bucket_half_count);
    
    data->counts[index]++;
    data->total_count++;
    
    if (value < data->min_value) data->min_value = value;
    if (value > data->max_value) data->max_value = value;
}

static int64_t hdr_get_value_at_percentile(HdrHistogramData* data, double percentile)
{
    if (data->total_count == 0) return 0;
    
    int64_t threshold = (int64_t)ceil(percentile * data->total_count / 100.0);
    int64_t count_to_prev = 0;
    
    for (int32_t i = 0; i < data->bucket_count; i++) {
        for (int32_t j = 0; j < data->sub_bucket_count; j++) {
            int32_t index = counts_index(i, j, data->sub_bucket_half_count);
            int64_t count_at_index = data->counts[index];
            
            if (count_at_index > 0) {
                count_to_prev += count_at_index;
                if (count_to_prev >= threshold) {
                    int64_t value = (int64_t)j << (i + data->unit_magnitude);
                    return value;
                }
            }
        }
    }
    return data->max_value;
}

static int64_t hdr_get_mean(HdrHistogramData* data)
{
    if (data->total_count == 0) return 0;
    
    int64_t total = 0;
    for (int32_t i = 0; i < data->bucket_count; i++) {
        for (int32_t j = 0; j < data->sub_bucket_count; j++) {
            int32_t index = counts_index(i, j, data->sub_bucket_half_count);
            int64_t count = data->counts[index];
            if (count > 0) {
                int64_t value = (int64_t)j << (i + data->unit_magnitude);
                total += value * count;
            }
        }
    }
    return total / data->total_count;
}

static void hdr_free(HdrHistogramData* data)
{
    free(data);
}

static void update_stats(UrmaPerfContext* ctx, urma_perf_record_type_t type)
{
    HdrHistogramData* hist = ctx->histograms[type];
    ctx->stats.type_record[type].sample_num = hist->total_count;
    ctx->stats.type_record[type].average = hdr_get_mean(hist);
    ctx->stats.type_record[type].mininum = hist->min_value;
    ctx->stats.type_record[type].maxinum = hist->max_value;
    ctx->stats.type_record[type].p90 = hdr_get_value_at_percentile(hist, 90.0);
    ctx->stats.type_record[type].p99 = hdr_get_value_at_percentile(hist, 99.0);
}

urma_status_t urma_start_perf(void)
{
    if (g_perf_ctx != NULL) {
        printf("g_perf_ctx != NULL");
        return URMA_ERROR;
    }
    
    //g_perf_ctx = (UrmaPerfContext*)calloc(1, sizeof(UrmaPerfContext));
    if (g_perf_ctx == NULL) {
        printf("g_perf_ctx == NULL");
        return URMA_ERROR;
    }
    
    for (int i = 0; i < URMA_PERF_RECORD_TYPE_MAX; i++) {
        g_perf_ctx->histograms[i] = hdr_init(URMA_PERF_MIN_VALUE, URMA_PERF_MAX_VALUE, URMA_PERF_SIGNIFICANT_FIGURES);
        g_perf_ctx->stats.type_record[i].type = (urma_perf_record_type_t)i;
    }
    
    g_perf_ctx->is_running = 1;
    printf("id: %ld\n", (long)syscall(__NR_gettid));
    printf("start: g_perf_ctx->is_running: %d\n", g_perf_ctx->is_running);
    return URMA_SUCCESS;
}

urma_status_t urma_stop_perf(void)
{
    if (g_perf_ctx == NULL) {
        return URMA_ERROR;
    }
    
    if (!g_perf_ctx->is_running) {
        return URMA_ERROR;
    }
    
    g_perf_ctx->is_running = 0;
    return URMA_SUCCESS;
}

void urma_step_perf(urma_perf_record_type_t type, uint64_t latency)
{
    if (g_perf_ctx == NULL || !g_perf_ctx->is_running) return;
    if (type >= URMA_PERF_RECORD_TYPE_MAX) return;
    
    hdr_record_value(g_perf_ctx->histograms[type], latency);
    update_stats(g_perf_ctx, type);
}

urma_status_t urma_get_perf_info(char *perf_buf, uint32_t *length)
{
    if (g_perf_ctx == NULL || perf_buf == NULL || length == NULL) {
        return URMA_ERROR;
    }
    
    char* ptr = perf_buf;
    uint32_t remaining = *length;
    int written;
    
    for (int i = 0; i < URMA_PERF_RECORD_TYPE_MAX; i++) {
        written = snprintf(ptr, remaining, 
            "[%s] samples: %lu, avg: %lu, min: %lu, max: %lu, p90: %lu, p99: %lu\n",
            perf_type_names[i],
            g_perf_ctx->stats.type_record[i].sample_num,
            g_perf_ctx->stats.type_record[i].average,
            g_perf_ctx->stats.type_record[i].mininum,
            g_perf_ctx->stats.type_record[i].maxinum,
            g_perf_ctx->stats.type_record[i].p90,
            g_perf_ctx->stats.type_record[i].p99);
        
        if (written < 0 || written >= remaining) {
            return URMA_ERROR;
        }
        
        ptr += written;
        remaining -= written;
    }
    
    *length = ptr - perf_buf;
    return URMA_SUCCESS;
}

void urma_perf_cleanup(void)
{
    if (g_perf_ctx == NULL) return;
    
    for (int i = 0; i < URMA_PERF_RECORD_TYPE_MAX; i++) {
        hdr_free(g_perf_ctx->histograms[i]);
    }
    free(g_perf_ctx);
    g_perf_ctx = NULL;
}