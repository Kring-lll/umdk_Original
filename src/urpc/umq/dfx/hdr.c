/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: umq perf hdr histogram
 * Create: 2025-10-29
 */

#include <stdlib.h>

#include "hdr.h"

/* map a value to its counts-array index (O(1), branch-light hot path) */
static inline uint32_t hdr_counts_index(uint64_t v)
{
    if (v < HDR_SUB_BUCKET_HALF_COUNT) {
        return (uint32_t)v;
    }
    if (v >= HDR_HIGHEST_TRACKABLE) {
        return HDR_COUNTS_ARRAY_SIZE - 1;
    }
    int bit_len = 64 - __builtin_clzll(v);
    int bucket_index = bit_len - (HDR_SUB_BUCKET_HALF_MAG + 1);
    uint32_t sub_index = (uint32_t)(v >> bucket_index);
    return (uint32_t)bucket_index * HDR_SUB_BUCKET_HALF_COUNT + sub_index;
}

/* recover the lower-bound value represented by a counts-array index */
static inline uint64_t hdr_value_from_index(uint32_t index)
{
    if (index < HDR_SUB_BUCKET_COUNT) {
        return (uint64_t)index;
    }
    int bucket_index = (int)(index >> HDR_SUB_BUCKET_HALF_MAG) - 1;
    int sub_index = (int)(index & (HDR_SUB_BUCKET_HALF_COUNT - 1)) + HDR_SUB_BUCKET_HALF_COUNT;
    return ((uint64_t)sub_index) << bucket_index;
}

hdr_histogram_t *hdr_create(void)
{
    hdr_histogram_t *h = (hdr_histogram_t *)calloc(1, sizeof(hdr_histogram_t));
    if (h == NULL) {
        return NULL;
    }
    h->counts = (uint64_t *)calloc(HDR_COUNTS_ARRAY_SIZE, sizeof(uint64_t));
    if (h->counts == NULL) {
        free(h);
        return NULL;
    }
    h->min_value = UINT64_MAX;
    return h;
}

void hdr_destroy(hdr_histogram_t *h)
{
    if (h == NULL) {
        return;
    }
    free(h->counts);
    free(h);
}

void hdr_reset(hdr_histogram_t *h)
{
    if (h == NULL) {
        return;
    }
    for (uint32_t i = 0; i < HDR_COUNTS_ARRAY_SIZE; i++) {
        h->counts[i] = 0;
    }
    h->total_count = 0;
    h->min_value = UINT64_MAX;
    h->max_value = 0;
    h->sum = 0;
}

void hdr_record(hdr_histogram_t *h, uint64_t value)
{
    if (h == NULL || h->counts == NULL) {
        return;
    }
    uint32_t idx = hdr_counts_index(value);
    h->counts[idx]++;
    h->total_count++;
    h->sum += value;
    if (value < h->min_value) {
        h->min_value = value;
    }
    if (value > h->max_value) {
        h->max_value = value;
    }
}

uint64_t hdr_value_at_quantile(const hdr_histogram_t *h, double percentile)
{
    if (h == NULL || h->counts == NULL || h->total_count == 0 || percentile <= 0.0) {
        return 0;
    }
    if (percentile >= HDR_MAX_PERCENTILE) {
        return h->max_value;
    }
    // ceil((percentile/100) * total_count) without libm
    double target = (percentile / HDR_MAX_PERCENTILE) * (double)h->total_count;
    uint64_t need = (uint64_t)target;
    if ((double)need < target) {
        need++;
    }
    if (need < 1) {
        need = 1;
    }
    if (need > h->total_count) {
        need = h->total_count;
    }
    uint64_t cumulative = 0;
    for (uint32_t i = 0; i < HDR_COUNTS_ARRAY_SIZE; i++) {
        cumulative += h->counts[i];
        if (cumulative >= need) {
            return hdr_value_from_index(i);
        }
    }
    return h->max_value;
}

void hdr_merge(hdr_histogram_t *dst, const hdr_histogram_t *src)
{
    if (dst == NULL || src == NULL || dst->counts == NULL || src->counts == NULL) {
        return;
    }
    for (uint32_t i = 0; i < HDR_COUNTS_ARRAY_SIZE; i++) {
        dst->counts[i] += src->counts[i];
    }
    dst->total_count += src->total_count;
    dst->sum += src->sum;
    if (src->min_value < dst->min_value) {
        dst->min_value = src->min_value;
    }
    if (src->max_value > dst->max_value) {
        dst->max_value = src->max_value;
    }
}

size_t hdr_counts_size(void)
{
    return (size_t)HDR_COUNTS_ARRAY_SIZE;
}
