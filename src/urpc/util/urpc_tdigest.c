/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: urpc tdigest for IO latency percentile estimation (integer arithmetic)
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
/* Initial min_val (uint64_t analog of +INFINITY) */
#define TDIGEST_MIN_INIT UINT64_MAX
/* Initial max_val (uint64_t analog of -INFINITY) */
#define TDIGEST_MAX_INIT 0

/*
 * k1 scaling function (t-digest paper):
 *   k(q, delta) = (delta / 2) * asin(2q - 1) / pi + delta / 2
 *
 * This is the ONLY place where double arithmetic is used.
 * The function maps quantile q ∈ [0,1] to a "k-space" where centroids
 * near q=0 and q=1 are given more weight (smaller k-range), producing
 * higher accuracy at extreme quantiles like p99/p999.
 *
 * Input q is derived from integer weight ratios; output k is used only
 * for integer weight comparisons after conversion back to weight units.
 */
static inline double tdigest_k_scale(double q, double delta)
{
    return (delta / 2.0) * asin(2.0 * q - 1.0) / M_PI + delta / 2.0;
}

/*
 * Compute the maximum allowable weight for a centroid at quantile position q,
 * such that its k-range does not exceed 1. Returns the result as uint64_t
 * (rounded down), suitable for direct comparison with integer centroid weights.
 *
 * The k_scale function uses double internally, but the output is converted
 * to integer for the compression decision: proposed_weight <= max_weight.
 */
static inline uint64_t tdigest_max_weight(uint64_t cum_weight, uint64_t cur_weight,
    uint64_t total_weight, uint32_t delta)
{
    if (URPC_UNLIKELY(delta == 0 || total_weight == 0)) {
        return 1;
    }
    /* Compute q from integer weights — this division is exact enough for k_scale */
    double q = (double)(cum_weight + cur_weight / 2) / (double)total_weight;
    double k_left = tdigest_k_scale(q - 1.0 / (double)delta, (double)delta);
    double k_right = tdigest_k_scale(q + 1.0 / (double)delta, (double)delta);
    /* Scale k-range back to weight units; 4*delta factor for stability */
    double max_w = 4.0 * (double)delta * (k_right - k_left);
    /* Clamp to total_weight and convert to integer */
    if (max_w > (double)total_weight) {
        return total_weight;
    }
    return (uint64_t)max_w;
}

/* ---- Sorting helpers ---- */

static int centroid_compare(const void *a, const void *b)
{
    uint64_t ma = ((const urpc_tdigest_centroid_t *)a)->mean;
    uint64_t mb = ((const urpc_tdigest_centroid_t *)b)->mean;
    if (ma < mb) {
        return -1;
    }
    if (ma > mb) {
        return 1;
    }
    return 0;
}

static int uint64_compare(const void *a, const void *b)
{
    uint64_t da = *(const uint64_t *)a;
    uint64_t db = *(const uint64_t *)b;
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
 *
 * All arithmetic is integer. The k1 scaling function is the only component
 * that internally uses double, but its output is converted to integer weight
 * constraints for the greedy merge decision.
 *
 * Weighted mean merging uses integer arithmetic:
 *   new_mean = (mean1 * weight1 + mean2 * weight2) / (weight1 + weight2)
 * This is exact when mean values are integers (nanoseconds).
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

    uint32_t delta = td->max_centroids;
    uint64_t total_weight = td->total_weight;

    /*
     * Greedy merge from left to right (starting from minimum).
     * For each centroid, check if it can be merged with the current
     * accumulator without exceeding the max_weight constraint.
     */
    urpc_tdigest_centroid_t accum = input[0];
    uint32_t out_idx = 0;
    uint64_t cum_weight_so_far = 0;

    for (uint32_t i = 1; i < total; i++) {
        uint64_t proposed_weight = accum.weight + input[i].weight;
        uint64_t max_w = tdigest_max_weight(cum_weight_so_far, accum.weight, total_weight, delta);

        if (proposed_weight <= max_w) {
            /* Merge: integer weighted average of means */
            /* new_mean = (mean1 * w1 + mean2 * w2) / (w1 + w2) */
            uint64_t new_mean = (accum.mean * accum.weight + input[i].mean * input[i].weight)
                              / proposed_weight;
            accum.mean = new_mean;
            accum.weight = proposed_weight;
        } else {
            /* Cannot merge — flush accumulator to output */
            cum_weight_so_far += accum.weight;
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

    /* Preserve exact min/max from uncompressed extremes */
    td->min_val = MIN(td->min_val, output[0].mean);
    td->max_val = MAX(td->max_val, output[out_idx - 1].mean);
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
    td->min_val = TDIGEST_MIN_INIT;
    td->max_val = TDIGEST_MAX_INIT;

    td->centroids = urpc_dbuf_malloc(URPC_DBUF_TYPE_UTIL,
        sizeof(urpc_tdigest_centroid_t) * max_centroids);
    if (URPC_UNLIKELY(td->centroids == NULL)) {
        urpc_dbuf_free(td);
        UTIL_LOG_ERR("tdigest create: malloc centroids failed\n");
        return NULL;
    }

    td->buffer = urpc_dbuf_malloc(URPC_DBUF_TYPE_UTIL, sizeof(uint64_t) * td->buffer_cap);
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

void urpc_tdigest_add(urpc_tdigest_t *td, uint64_t value)
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

void urpc_tdigest_add_n(urpc_tdigest_t *td, const uint64_t *values, uint32_t count)
{
    if (URPC_UNLIKELY(td == NULL || values == NULL || count == 0)) {
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        urpc_tdigest_add(td, values[i]);
    }
}

uint64_t urpc_tdigest_quantile(urpc_tdigest_t *td, double q)
{
    if (URPC_UNLIKELY(td == NULL)) {
        return 0;
    }
    if (URPC_UNLIKELY(td->total_weight == 0)) {
        return 0;
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
        return 0;
    }

    double delta = (double)td->max_centroids;
    uint64_t total_weight = td->total_weight;

    /*
     * Walk through centroids, accumulating weight until we reach the target
     * quantile. Interpolation uses k-space (double) for positioning but
     * returns an integer (ns) result via linear interpolation of integer means.
     */
    uint64_t cum_weight = 0;
    for (uint32_t i = 0; i < td->centroid_count; i++) {
        uint64_t w = td->centroids[i].weight;
        uint64_t cum_weight_next = cum_weight + w;

        double q_left = (double)cum_weight / (double)total_weight;
        double q_right = (double)cum_weight_next / (double)total_weight;

        if (q <= q_right) {
            /*
             * Target quantile falls within this centroid's range.
             * Interpolate position using k-space, then map back to integer
             * value by linear interpolation between adjacent centroid means.
             */
            double k_target = tdigest_k_scale(q, delta);
            double k_left = tdigest_k_scale(q_left, delta);
            double k_right = tdigest_k_scale(q_right, delta);

            double k_range = k_right - k_left;
            if (k_range <= 0) {
                /* Degenerate case — return centroid mean */
                return td->centroids[i].mean;
            }

            double fraction = (k_target - k_left) / k_range;

            /*
             * Linear interpolation between integer mean values.
             * result = left_mean + fraction * (right_mean - left_mean)
             * Computed in double for the fractional part, rounded to uint64_t.
             * This is the ONLY place where we convert a double result back
             * to uint64_t — the interpolation fraction is inherently fractional.
             */
            uint64_t left_mean, right_mean;
            if (i == 0) {
                left_mean = td->min_val;
                right_mean = td->centroids[0].mean;
                if (td->centroid_count > 1) {
                    right_mean = td->centroids[1].mean;
                }
            } else if (i == td->centroid_count - 1) {
                left_mean = td->centroids[i - 1].mean;
                right_mean = td->max_val;
                if (td->centroid_count > 1) {
                    right_mean = td->centroids[i].mean;
                }
            } else {
                left_mean = td->centroids[i - 1].mean;
                right_mean = td->centroids[i].mean;
            }

            double interp = (double)left_mean + fraction * ((double)right_mean - (double)left_mean);
            return (uint64_t)interp;
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

    /* Ensure dst is compressed before merge */
    if (dst->buffer_count > 0) {
        tdigest_do_compress(dst);
    }

    /* Add all src centroids as samples to dst's buffer */
    for (uint32_t i = 0; i < src->centroid_count; i++) {
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
    td->min_val = TDIGEST_MIN_INIT;
    td->max_val = TDIGEST_MAX_INIT;
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
 * Serialization format (all fields native endian):
 *   [4 bytes]  magic: 0x54444753 ("TDGS")
 *   [4 bytes]  max_centroids
 *   [8 bytes]  total_weight
 *   [8 bytes]  min_val (uint64_t, ns)
 *   [8 bytes]  max_val (uint64_t, ns)
 *   [4 bytes]  centroid_count
 *   [centroid_count * 16 bytes] centroids (mean:uint64_t, weight:uint64_t)
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
    *(uint64_t *)(buf + off) = td->min_val;
    off += sizeof(uint64_t);

    /* max_val */
    *(uint64_t *)(buf + off) = td->max_val;
    off += sizeof(uint64_t);

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
    uint64_t min_val = *(const uint64_t *)(buf + off);
    off += sizeof(uint64_t);

    /* max_val */
    uint64_t max_val = *(const uint64_t *)(buf + off);
    off += sizeof(uint64_t);

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
