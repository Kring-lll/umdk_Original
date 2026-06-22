/**
* Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
* t-digest algorithm used to implement tpX (e.g. tp99 tp90) - C language version
*/
#ifndef UMQ_PERF_TDIGEST_H
#define UMQ_PERF_TDIGEST_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UMQ_TDIGEST_QMIN               (1e-15)
#define UMQ_TDIGEST_QMAX               (1.0 - 1e-15)
#define UMQ_TDIGEST_QMID               (0.5)
#define UMQ_TDIGEST_SCALE_FACTOR       (2.0)
#define UMQ_TDIGEST_PCT_MAX            (100.0)
#define UMQ_TDIGEST_BUFFER_RATIO       (2)
#define UMQ_TDIGEST_HALF_WEIGHT_DIV    ((uint32_t)2)
#define UMQ_TDIGEST_WEIGHT_SINGLE      ((uint32_t)1)
#define UMQ_TDIGEST_MAX_COMPRESSION    (10000)
#define UMQ_TDIGEST_DEFAULT_COMPRESSION (100)
#define UMQ_TDIGEST_NORMALIZER_K4      (4)
#define UMQ_TDIGEST_NORMALIZER_K21     (21)

typedef struct umq_tdigest_centroid {
    double mean;
    uint32_t weight;
} umq_tdigest_centroid_t;

typedef struct umq_tdigest_centroid_list {
    umq_tdigest_centroid_t *centroids;
    size_t capacity;
    size_t size;
    uint64_t total_weight;
} umq_tdigest_centroid_list_t;

typedef enum umq_tdigest_insert_result {
    TDIGEST_NO_NEED_COMPRESS = 0,
    TDIGEST_OVERFLOW         = 1
} umq_tdigest_insert_result_e;

typedef struct umq_tdigest {
    tdigest_centroid_list_t one;
    tdigest_centroid_list_t two;
    tdigest_centroid_list_t buffer;
    tdigest_centroid_list_t *active;
    int compression;
    uint64_t min_value;
    uint64_t max_value;
    bool forward;
} umq_tdigest_t;

umq_tdigest_t *umq_tdigest_create(int compression);
void tdigest_destroy(tdigest_t *td);
void tdigest_reset(tdigest_t *td);
void tdigest_insert(tdigest_t *td, double value, uint32_t weight);
double tdigest_quantile(tdigest_t *td, double p);
void tdigest_merge(tdigest_t *dst, tdigest_t *src);

#ifdef __cplusplus
}
#endif

#endif
