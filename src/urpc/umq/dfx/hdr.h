/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: umq perf hdr histogram
 * Create: 2025-10-29
 */

#ifndef UMQ_PERF_HDR_H
#define UMQ_PERF_HDR_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* significant_figures = 2 -> constant ~0.78% relative value error across the whole range */
#define HDR_SIG_FIGURES             (2)
#define HDR_MAX_PERCENTILE          (100.0)
#define HDR_SUB_BUCKET_HALF_MAG     (7)
#define HDR_SUB_BUCKET_HALF_COUNT   (1u << HDR_SUB_BUCKET_HALF_MAG)
#define HDR_SUB_BUCKET_COUNT        (HDR_SUB_BUCKET_HALF_COUNT * 2u)
#define HDR_BUCKET_COUNT            (14)
/* 256 + 14 * 128 = 2048 buckets */
#define HDR_COUNTS_ARRAY_SIZE       (HDR_SUB_BUCKET_COUNT + HDR_BUCKET_COUNT * HDR_SUB_BUCKET_HALF_COUNT)
/* highest trackable value = subBucketCount << bucketCount = 2^22 cycles */
#define HDR_HIGHEST_TRACKABLE       ((uint64_t)1 << (HDR_BUCKET_COUNT + HDR_SUB_BUCKET_HALF_MAG + 1))

typedef struct hdr_histogram {
    uint64_t *counts;
    uint64_t total_count;
    uint64_t min_value;
    uint64_t max_value;
    uint64_t sum;
} hdr_histogram_t;

hdr_histogram_t *hdr_create(void);
void hdr_destroy(hdr_histogram_t *h);
void hdr_reset(hdr_histogram_t *h);
void hdr_record(hdr_histogram_t *h, uint64_t value);
uint64_t hdr_value_at_quantile(const hdr_histogram_t *h, double percentile);
void hdr_merge(hdr_histogram_t *dst, const hdr_histogram_t *src);
size_t hdr_counts_size(void);

#ifdef __cplusplus
}
#endif

#endif
