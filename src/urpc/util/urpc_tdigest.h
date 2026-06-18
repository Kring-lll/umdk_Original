/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: urpc tdigest for IO latency percentile estimation
 * Create: 2026-06-18
 */
#ifndef URPC_TDIGEST_H
#define URPC_TDIGEST_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A centroid in the tdigest, representing a cluster of nearby samples.
 */
typedef struct urpc_tdigest_centroid {
    double mean;
    uint64_t weight;
} urpc_tdigest_centroid_t;

/**
 * @brief TDigest instance for streaming percentile estimation.
 *
 * Maintains a compressed representation of a data distribution using ordered
 * centroids. Provides accurate estimates for extreme quantiles (p99, p999)
 * with bounded memory usage.
 *
 * Usage in IO path:
 *   1. Create: urpc_tdigest_t *td = urpc_tdigest_create(100);
 *   2. Record: urpc_tdigest_add(td, latency_ns);
 *   3. Query:  p99 = urpc_tdigest_quantile(td, 0.99);
 *   4. Merge:  urpc_tdigest_merge(global_td, per_thread_td);
 *   5. Destroy: urpc_tdigest_destroy(td);
 */
typedef struct urpc_tdigest {
    urpc_tdigest_centroid_t *centroids;   /* compressed centroids (sorted by mean) */
    double *buffer;                       /* pending samples before compression */
    uint32_t max_centroids;               /* max number of centroids (controls accuracy/memory) */
    uint32_t centroid_count;              /* current number of centroids */
    uint32_t buffer_count;                /* current number of pending samples */
    uint32_t buffer_cap;                  /* buffer capacity (= max_centroids / 2) */
    uint64_t total_weight;                /* total weight of all samples */
    double min_val;                       /* exact minimum (not approximated) */
    double max_val;                       /* exact maximum (not approximated) */
} urpc_tdigest_t;

/**
 * @brief Create a tdigest instance.
 * @param max_centroids Maximum number of centroids. Larger values give higher
 *        accuracy at the cost of more memory. Typical values: 100-1000.
 *        For IO latency statistics, 100 provides ~1% error at p99.
 * @return Pointer to new tdigest, or NULL on allocation failure.
 */
urpc_tdigest_t *urpc_tdigest_create(uint32_t max_centroids);

/**
 * @brief Destroy a tdigest instance and free all memory.
 * @param td TDigest to destroy. May be NULL (no-op).
 */
void urpc_tdigest_destroy(urpc_tdigest_t *td);

/**
 * @brief Add a single sample to the tdigest.
 *
 * Designed for IO path: the sample is buffered internally. Compression is
 * triggered only when the buffer is full, so per-call overhead is minimal
 * (just storing one double).
 *
 * @param td   TDigest instance.
 * @param value Sample value (e.g., latency in nanoseconds).
 */
void urpc_tdigest_add(urpc_tdigest_t *td, double value);

/**
 * @brief Batch-add multiple samples to the tdigest.
 * @param td     TDigest instance.
 * @param values Array of sample values.
 * @param count  Number of values in the array.
 */
void urpc_tdigest_add_n(urpc_tdigest_t *td, const double *values, uint32_t count);

/**
 * @brief Estimate the value at a given quantile.
 *
 * Must be called after any pending buffer samples are compressed.
 * Call urpc_tdigest_compress() first if add() was recently called.
 *
 * @param td  TDigest instance.
 * @param q   Quantile in [0, 1]. E.g., 0.5 for median, 0.99 for p99.
 * @return Estimated value at quantile q. Returns td->min_val for q<=0,
 *         td->max_val for q>=1, 0 if td has no samples.
 */
double urpc_tdigest_quantile(urpc_tdigest_t *td, double q);

/**
 * @brief Merge src tdigest into dst.
 *
 * After merging, dst contains the combined distribution of both.
 * src is not modified. This enables per-thread tdigest collection
 * followed by a merge into a global tdigest for aggregate statistics.
 *
 * @param dst Destination tdigest (receives merged result).
 * @param src Source tdigest (unchanged after merge).
 */
void urpc_tdigest_merge(urpc_tdigest_t *dst, const urpc_tdigest_t *src);

/**
 * @brief Compress any buffered samples into centroids.
 *
 * Normally called automatically when the buffer is full. Call explicitly
 * before quantile() if you need up-to-date results from recent add() calls.
 *
 * @param td TDigest instance.
 */
void urpc_tdigest_compress(urpc_tdigest_t *td);

/**
 * @brief Reset the tdigest, discarding all data.
 * @param td TDigest instance.
 */
void urpc_tdigest_reset(urpc_tdigest_t *td);

/**
 * @brief Get the total number of samples recorded.
 * @param td TDigest instance.
 * @return Total weight (total number of samples added).
 */
uint64_t urpc_tdigest_count(const urpc_tdigest_t *td);

/**
 * @brief Serialize a tdigest into a byte buffer.
 *
 * The serialized format is portable and can be used for cross-process
 * or network transmission of latency statistics.
 *
 * @param td  TDigest instance.
 * @param buf Output buffer. If NULL, only the required size is computed.
 * @param len Buffer capacity in bytes.
 * @return Number of bytes written, or required size if buf is NULL.
 *         Returns 0 on error (td is NULL or buffer too small).
 */
uint32_t urpc_tdigest_serialise(const urpc_tdigest_t *td, uint8_t *buf, uint32_t len);

/**
 * @brief Deserialize a tdigest from a byte buffer.
 *
 * @param buf Input buffer containing serialized tdigest data.
 * @param len Buffer length in bytes.
 * @return New tdigest instance from the serialized data, or NULL on error.
 */
urpc_tdigest_t *urpc_tdigest_deserialise(const uint8_t *buf, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif
