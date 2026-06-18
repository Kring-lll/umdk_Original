/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: urpc tdigest for IO latency percentile estimation
 * Create: 2026-06-18
 */
#include <math.h>
#include <string.h>

#include "urpc_util.h"
#include "urpc_dbuf_stat.h"
#include "util_log.h"

#include "urpc_tdigest.h"

/* Minimum max_centroids to produce meaningful results */
#define TDIGEST_MIN_MAX_CENTROIDS 10
/* Default max_centroids when 0 is passed */
#define TDIGEST_DEFAULT_MAX_CENTROIDS 100

/*
 * Scaling function (k1 from the t-digest paper):
 *   k(q, delta) = (delta / 2) * asin(2q - 1) / pi + delta / 2
 *
 * This maps quantile q ∈ [0,1] to a "k-space" where centroids near q=0 and
 * q=1 are given more weight (smaller k-range), producing higher accuracy at
 * extreme quantiles like p99/p999.
 */
static inline double tdigest_k_scale(double q, double delta)
{
    return (delta / 2.0) * asin(2.0 * q - 1.0) / M_PI + delta / 2.0;
}

/* Inverse of k_scale: maps k-space back to quantile */
static inline double tdigest_k_scale_inv(double k, double delta)
{
    double p = (k / delta - 0.5) * 2.0;
    return (sin(p * M_PI) + 1.0) / 2.0;
}

/*
 * Compute the maximum allowable weight for a centroid at quantile position q,
 * such that its k-range does not exceed 1. This ensures no single centroid
 * spans more than one unit in k-space.
 */
static inline double tdigest_max_weight(double q, double delta)
{
    if (URPC_UNLIKELY(delta <= 0)) {
        return 1.0;
    }
    double k_left = tdigest_k_scale(q - 1.0 / delta, delta);
    double k_right = tdigest_k_scale(q + 1.0 / delta, delta);
    /* Allow slightly more weight for stability during compression */
    return 4.0 * delta * (k_right - k_left);
}

/* ---- Sorting helpers ---- */

static int centroid_compare(const void *a, const void *b)
{
    double ma = ((const urpc_tdigest_centroid_t *)a)->mean;
    double mb = ((const urpc_tdigest_centroid_t *)b)->mean;
    if (ma < mb) {
        return -1;
    }
    if (ma > mb) {
        return 1;
    }
    return 0;
}

static int double_compare(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) {
        return -1;
    }
    if (da > db) {
        return 1;
    }
    return 0;
}

/* ---- Core compression ---- */

/*
 * Compress all centroids + buffered samples into a new set of centroids.
 * Uses the t-digest merging algorithm: sort all inputs, then greedily merge
 * adjacent centroids as long as the merged centroid's k-range ≤ 1.
 * Start from extremes (min/max) for best accuracy at tail quantiles.
 */
static void tdigest_do_compress(urpc_tdigest_t *td)
{
    if (URPC_UNLIKELY(td == NULL)) {
        return;
    }

    uint32_t total = td->centroid_count + td->buffer_count;
    if (total == 0) {
        return;
    }

    /* Build temporary array: existing centroids + buffered samples */
    urpc_tdigest_centroid_t *input = urpc_dbuf_malloc(
        URPC_DBUF_TYPE_UTIL, sizeof(urpc_tdigest_centroid_t) * total);
    if (URPC_UNLIKELY(input == NULL)) {
        UTIL_LOG_ERR("tdigest compress: malloc input array failed\n");
        return;
    }

    /* Copy existing centroids */
    if (td->centroid_count > 0) {
        (void)memcpy(input, td->centroids, sizeof(urpc_tdigest_centroid_t) * td->centroid_count);
    }

    /* Convert buffered samples to weight-1 centroids */
    for (uint32_t i = 0; i < td->buffer_count; i++) {
        input[td->centroid_count + i].mean = td->buffer[i];
        input[td->centroid_count + i].weight = 1;
    }

    /* Sort by mean */
    qsort(input, total, sizeof(urpc_tdigest_centroid_t), centroid_compare);

    /* Allocate output array */
    urpc_tdigest_centroid_t *output = urpc_dbuf_malloc(
        URPC_DBUF_TYPE_UTIL, sizeof(urpc_tdigest_centroid_t) * td->max_centroids);
    if (URPC_UNLIKELY(output == NULL)) {
        urpc_dbuf_free(input);
        UTIL_LOG_ERR("tdigest compress: malloc output array failed\n");
        return;
    }

    double delta = (double)td->max_centroids;
    uint64_t total_weight = td->total_weight;

    /*
     * Greedy merge from left to right (starting from minimum).
     * For each centroid, check if it can be merged with the current
     * accumulator without exceeding the max_weight constraint.
     */
    urpc_tdigest_centroid_t accum = input[0];
    uint32_t out_idx = 0;

    for (uint32_t i = 1; i < total; i++) {
        double proposed_weight = (double)(accum.weight + input[i].weight);
        double q = (double)(accum.weight + input[i].weight / 2.0) / (double)total_weight;
        double max_w = tdigest_max_weight(q, delta);

        if (proposed_weight <= max_w) {
            /* Merge: weighted average of means */
            double new_mean = (accum.mean * (double)accum.weight + input[i].mean * (double)input[i].weight)
                            / proposed_weight;
            accum.mean = new_mean;
            accum.weight += input[i].weight;
        } else {
            /* Cannot merge — flush accumulator to output */
            output[out_idx++] = accum;
            accum = input[i];
        }
    }

    /* Flush last accumulator */
    output[out_idx++] = accum;

    /* Swap centroid arrays */
    urpc_dbuf_free(td->centroids);
    td->centroids = output;
    td->centroid_count = out_idx;

    /* Clear buffer */
    td->buffer_count = 0;

    /* Free input */
    urpc_dbuf_free(input);

    /* Preserve exact min/max */
    if (td->buffer_count == 0 && td->centroid_count > 0) {
        /* After compression, ensure min/max are still exact */
        td->min_val = MIN(td->min_val, output[0].mean);
        td->max_val = MAX(td->max_val, output[out_idx - 1].mean);
    }
}

/* ---- Public API ---- */

urpc_tdigest_t *urpc_tdigest_create(uint32_t max_centroids)
{
    if (max_centroids == 0) {
        max_centroids = TDIGEST_DEFAULT_MAX_CENTROIDS;
    }
    if (max_centroids < TDIGEST_MIN_MAX_CENTROIDS) {
        UTIL_LOG_ERR("tdigest create: max_centroids %u too small, minimum is %u\n",
            max_centroids, TDIGEST_MIN_MAX_CENTROIDS);
        return NULL;
    }

    urpc_tdigest_t *td = urpc_dbuf_malloc(URPC_DBUF_TYPE_UTIL, sizeof(urpc_tdigest_t));
    if (URPC_UNLIKELY(td == NULL)) {
        UTIL_LOG_ERR("tdigest create: malloc td failed\n");
        return NULL;
    }

    td->max_centroids = max_centroids;
    td->buffer_cap = MAX(max_centroids / 2, 5);
    td->centroid_count = 0;
    td->buffer_count = 0;
    td->total_weight = 0;
    td->min_val = INFINITY;
    td->max_val = -INFINITY;

    td->centroids = urpc_dbuf_malloc(URPC_DBUF_TYPE_UTIL,
        sizeof(urpc_tdigest_centroid_t) * max_centroids);
    if (URPC_UNLIKELY(td->centroids == NULL)) {
        urpc_dbuf_free(td);
        UTIL_LOG_ERR("tdigest create: malloc centroids failed\n");
        return NULL;
    }

    td->buffer = urpc_dbuf_malloc(URPC_DBUF_TYPE_UTIL, sizeof(double) * td->buffer_cap);
    if (URPC_UNLIKELY(td->buffer == NULL)) {
        urpc_dbuf_free(td->centroids);
        urpc_dbuf_free(td);
        UTIL_LOG_ERR("tdigest create: malloc buffer failed\n");
        return NULL;
    }

    return td;
}

void urpc_tdigest_destroy(urpc_tdigest_t *td)
{
    if (URPC_UNLIKELY(td == NULL)) {
        return;
    }

    if (td->centroids != NULL) {
        urpc_dbuf_free(td->centroids);
    }
    if (td->buffer != NULL) {
        urpc_dbuf_free(td->buffer);
    }
    urpc_dbuf_free(td);
}

void urpc_tdigest_add(urpc_tdigest_t *td, double value)
{
    if (URPC_UNLIKELY(td == NULL)) {
        return;
    }

    /* Track exact min/max */
    if (value < td->min_val) {
        td->min_val = value;
    }
    if (value > td->max_val) {
        td->max_val = value;
    }

    /* Buffer the sample — compression deferred until buffer is full */
    td->buffer[td->buffer_count++] = value;
    td->total_weight++;

    /* Trigger compression when buffer is full */
    if (td->buffer_count >= td->buffer_cap) {
        tdigest_do_compress(td);
    }
}

void urpc_tdigest_add_n(urpc_tdigest_t *td, const double *values, uint32_t count)
{
    if (URPC_UNLIKELY(td == NULL || values == NULL || count == 0)) {
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        urpc_tdigest_add(td, values[i]);
    }
}

double urpc_tdigest_quantile(urpc_tdigest_t *td, double q)
{
    if (URPC_UNLIKELY(td == NULL)) {
        return 0.0;
    }
    if (URPC_UNLIKELY(td->total_weight == 0)) {
        return 0.0;
    }
    if (URPC_UNLIKELY(q <= 0.0)) {
        return td->min_val;
    }
    if (URPC_UNLIKELY(q >= 1.0)) {
        return td->max_val;
    }

    /* Ensure buffered samples are compressed before querying */
    if (td->buffer_count > 0) {
        tdigest_do_compress(td);
    }

    if (td->centroid_count == 0) {
        return 0.0;
    }

    double delta = (double)td->max_centroids;
    double target_q = q;

    /* Walk through centroids, accumulating weight until we reach target_q */
    double cum_weight = 0.0;
    for (uint32_t i = 0; i < td->centroid_count; i++) {
        double w = (double)td->centroids[i].weight;
        double cum_weight_next = cum_weight + w;

        double q_left = cum_weight / (double)td->total_weight;
        double q_right = cum_weight_next / (double)td->total_weight;

        if (target_q <= q_right) {
            /*
             * The target quantile falls within this centroid's range.
             * Interpolate within the centroid using k-space for accuracy.
             */
            double k_target = tdigest_k_scale(target_q, delta);
            double k_left = tdigest_k_scale(q_left, delta);
            double k_right = tdigest_k_scale(q_right, delta);

            if (k_right - k_left <= 0) {
                /* Degenerate case — return centroid mean */
                return td->centroids[i].mean;
            }

            double fraction = (k_target - k_left) / (k_right - k_left);
            if (URPC_UNLIKELY(i == 0 && td->centroids[0].mean == td->min_val)) {
                /* First centroid represents exact min — interpolate towards min */
                double right_mean = (td->centroid_count > 1) ? td->centroids[1].mean : td->max_val;
                return td->min_val + fraction * (right_mean - td->min_val);
            }
            if (URPC_UNLIKELY(i == td->centroid_count - 1 && td->centroids[i].mean == td->max_val)) {
                /* Last centroid represents exact max */
                double left_mean = (td->centroid_count > 1) ? td->centroids[i - 1].mean : td->min_val;
                return left_mean + fraction * (td->max_val - left_mean);
            }

            /* Standard interpolation between adjacent centroid means */
            if (i == 0) {
                return td->min_val + fraction * (td->centroids[0].mean - td->min_val);
            }
            if (i == td->centroid_count - 1) {
                return td->centroids[i - 1].mean + fraction * (td->max_val - td->centroids[i - 1].mean);
            }

            return td->centroids[i - 1].mean + fraction * (td->centroids[i].mean - td->centroids[i - 1].mean);
        }

        cum_weight = cum_weight_next;
    }

    /* Should not reach here, but return max as fallback */
    return td->max_val;
}

void urpc_tdigest_merge(urpc_tdigest_t *dst, const urpc_tdigest_t *src)
{
    if (URPC_UNLIKELY(dst == NULL || src == NULL)) {
        return;
    }
    if (URPC_UNLIKELY(src->total_weight == 0)) {
        return;
    }

    /* Ensure both are compressed before merge */
    if (dst->buffer_count > 0) {
        tdigest_do_compress(dst);
    }

    /* Create a temporary tdigest with src's data and merge via compression */
    /* Add all src centroids as samples to dst's buffer */
    for (uint32_t i = 0; i < src->centroid_count; i++) {
        /* Grow buffer if needed */
        if (dst->buffer_count >= dst->buffer_cap) {
            tdigest_do_compress(dst);
        }
        dst->buffer[dst->buffer_count++] = src->centroids[i].mean;
        dst->total_weight += src->centroids[i].weight;
    }

    /* Add any remaining src buffered samples */
    for (uint32_t i = 0; i < src->buffer_count; i++) {
        if (dst->buffer_count >= dst->buffer_cap) {
            tdigest_do_compress(dst);
        }
        dst->buffer[dst->buffer_count++] = src->buffer[i];
        dst->total_weight++;
    }

    /* Update min/max */
    dst->min_val = MIN(dst->min_val, src->min_val);
    dst->max_val = MAX(dst->max_val, src->max_val);

    /* Final compression */
    tdigest_do_compress(dst);
}

void urpc_tdigest_compress(urpc_tdigest_t *td)
{
    if (URPC_UNLIKELY(td == NULL)) {
        return;
    }
    if (td->buffer_count > 0) {
        tdigest_do_compress(td);
    }
}

void urpc_tdigest_reset(urpc_tdigest_t *td)
{
    if (URPC_UNLIKELY(td == NULL)) {
        return;
    }

    td->centroid_count = 0;
    td->buffer_count = 0;
    td->total_weight = 0;
    td->min_val = INFINITY;
    td->max_val = -INFINITY;
}

uint64_t urpc_tdigest_count(const urpc_tdigest_t *td)
{
    if (URPC_UNLIKELY(td == NULL)) {
        return 0;
    }
    return td->total_weight;
}

/* ---- Serialization ---- */

/*
 * Serialization format (all fields little-endian):
 *   [4 bytes]  magic: 0x54444753 ("TDGS")
 *   [4 bytes]  max_centroids
 *   [8 bytes]  total_weight
 *   [8 bytes]  min_val (double)
 *   [8 bytes]  max_val (double)
 *   [4 bytes]  centroid_count
 *   [centroid_count * 16 bytes] centroids (mean:double, weight:uint64_t)
 *
 * Total size = 32 + centroid_count * 16
 */

#define TDIGEST_SERIAL_MAGIC 0x54444753u

uint32_t urpc_tdigest_serialise(const urpc_tdigest_t *td, uint8_t *buf, uint32_t len)
{
    if (URPC_UNLIKELY(td == NULL)) {
        return 0;
    }

    /* Ensure data is compressed */
    if (td->buffer_count > 0) {
        tdigest_do_compress((urpc_tdigest_t *)td);
    }

    uint32_t required = 32 + td->centroid_count * (uint32_t)sizeof(urpc_tdigest_centroid_t);
    if (buf == NULL) {
        return required;
    }
    if (len < required) {
        UTIL_LOG_ERR("tdigest serialise: buffer too small, need %u have %u\n", required, len);
        return 0;
    }

    uint32_t off = 0;

    /* Magic */
    *(uint32_t *)(buf + off) = TDIGEST_SERIAL_MAGIC;
    off += sizeof(uint32_t);

    /* max_centroids */
    *(uint32_t *)(buf + off) = td->max_centroids;
    off += sizeof(uint32_t);

    /* total_weight */
    *(uint64_t *)(buf + off) = td->total_weight;
    off += sizeof(uint64_t);

    /* min_val */
    *(double *)(buf + off) = td->min_val;
    off += sizeof(double);

    /* max_val */
    *(double *)(buf + off) = td->max_val;
    off += sizeof(double);

    /* centroid_count */
    *(uint32_t *)(buf + off) = td->centroid_count;
    off += sizeof(uint32_t);

    /* centroids */
    if (td->centroid_count > 0) {
        (void)memcpy(buf + off, td->centroids, td->centroid_count * sizeof(urpc_tdigest_centroid_t));
        off += td->centroid_count * sizeof(urpc_tdigest_centroid_t);
    }

    return off;
}

urpc_tdigest_t *urpc_tdigest_deserialise(const uint8_t *buf, uint32_t len)
{
    if (URPC_UNLIKELY(buf == NULL || len < 32)) {
        return NULL;
    }

    uint32_t off = 0;

    /* Magic */
    uint32_t magic = *(const uint32_t *)(buf + off);
    off += sizeof(uint32_t);
    if (magic != TDIGEST_SERIAL_MAGIC) {
        UTIL_LOG_ERR("tdigest deserialise: bad magic 0x%08x\n", magic);
        return NULL;
    }

    /* max_centroids */
    uint32_t max_centroids = *(const uint32_t *)(buf + off);
    off += sizeof(uint32_t);
    if (max_centroids < TDIGEST_MIN_MAX_CENTROIDS) {
        UTIL_LOG_ERR("tdigest deserialise: max_centroids %u too small\n", max_centroids);
        return NULL;
    }

    /* total_weight */
    uint64_t total_weight = *(const uint64_t *)(buf + off);
    off += sizeof(uint64_t);

    /* min_val */
    double min_val = *(const double *)(buf + off);
    off += sizeof(double);

    /* max_val */
    double max_val = *(const double *)(buf + off);
    off += sizeof(double);

    /* centroid_count */
    uint32_t centroid_count = *(const uint32_t *)(buf + off);
    off += sizeof(uint32_t);

    /* Validate size */
    uint32_t required = 32 + centroid_count * (uint32_t)sizeof(urpc_tdigest_centroid_t);
    if (len < required) {
        UTIL_LOG_ERR("tdigest deserialise: buffer too small, need %u have %u\n", required, len);
        return NULL;
    }

    /* Create tdigest */
    urpc_tdigest_t *td = urpc_tdigest_create(max_centroids);
    if (URPC_UNLIKELY(td == NULL)) {
        return NULL;
    }

    td->total_weight = total_weight;
    td->min_val = min_val;
    td->max_val = max_val;
    td->centroid_count = centroid_count;

    /* Copy centroids */
    if (centroid_count > 0) {
        (void)memcpy(td->centroids, buf + off, centroid_count * sizeof(urpc_tdigest_centroid_t));
    }

    return td;
}
