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

#define TDIGEST_QMIN               (1e-15)
#define TDIGEST_QMAX               (1.0 - 1e-15)
#define TDIGEST_QMID               (0.5)
#define TDIGEST_SCALE_FACTOR       (2.0)
#define TDIGEST_PCT_MAX            (100.0)
#define TDIGEST_BUFFER_RATIO       (2)
#define TDIGEST_HALF_WEIGHT_DIV    ((uint32_t)2)
#define TDIGEST_WEIGHT_SINGLE      ((uint32_t)1)
#define TDIGEST_MAX_COMPRESSION    (10000)
#define TDIGEST_DEFAULT_COMPRESSION (100)
#define TDIGEST_NORMALIZER_K4      (4)
#define TDIGEST_NORMALIZER_K21     (21)

typedef struct tdigest_centroid {
    double mean;
    uint32_t weight;
} tdigest_centroid_t;

typedef struct tdigest_centroid_list {
    tdigest_centroid_t *centroids;
    size_t capacity;
    size_t size;
    uint64_t total_weight;
} tdigest_centroid_list_t;

typedef enum tdigest_insert_result {
    TDIGEST_NO_NEED_COMPRESS = 0,
    TDIGEST_OVERFLOW         = 1
} tdigest_insert_result_e;

typedef struct tdigest {
    tdigest_centroid_list_t one;
    tdigest_centroid_list_t two;
    tdigest_centroid_list_t buffer;
    tdigest_centroid_list_t *active;
    int compression;
    uint64_t min_value;
    uint64_t max_value;
    bool forward;
} tdigest_t;

tdigest_t *tdigest_create(int compression);
void tdigest_destroy(tdigest_t *td);
void tdigest_reset(tdigest_t *td);
void tdigest_insert(tdigest_t *td, double value, uint32_t weight);
double tdigest_quantile(tdigest_t *td, double p);
void tdigest_merge(tdigest_t *dst, tdigest_t *src);

#ifdef __cplusplus
}
#endif

#endif
