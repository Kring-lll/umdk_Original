/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: urpc tdigest unit test
 */
#include "gtest/gtest.h"
#include "urpc_tdigest.h"
#include "urpc_dbuf_stat.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <vector>
#include <random>

/* ─── Helper: compute exact quantile from a sorted array ─── */
static double exact_quantile(const std::vector<double> &data, double q)
{
    if (data.empty()) {
        return 0.0;
    }
    std::vector<double> sorted = data;
    std::sort(sorted.begin(), sorted.end());

    double pos = q * (sorted.size() - 1);
    uint32_t lo = (uint32_t)std::floor(pos);
    uint32_t hi = (uint32_t)std::ceil(pos);
    if (lo == hi) {
        return sorted[lo];
    }
    double frac = pos - lo;
    return sorted[lo] + frac * (sorted[hi] - sorted[lo]);
}

/* ─── 1. Create / Destroy ─── */

TEST(UrpcTdigestTest, CreateDestroyBasic)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(100);
    ASSERT_NE(td, nullptr);
    EXPECT_EQ(urpc_tdigest_count(td), 0u);

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

TEST(UrpcTdigestTest, CreateWithDefaultMaxCentroids)
{
    /* max_centroids = 0 should use default (100) */
    urpc_tdigest_t *td = urpc_tdigest_create(0);
    ASSERT_NE(td, nullptr);
    EXPECT_EQ(td->max_centroids, 100u);

    urpc_tdigest_destroy(td);
}

TEST(UrpcTdigestTest, CreateTooSmallMaxCentroids)
{
    /* max_centroids < 10 should fail */
    urpc_tdigest_t *td = urpc_tdigest_create(5);
    EXPECT_EQ(td, nullptr);
}

TEST(UrpcTdigestTest, DestroyNull)
{
    /* Destroy NULL should be a no-op */
    urpc_tdigest_destroy(nullptr);
}

/* ─── 2. Add / Count ─── */

TEST(UrpcTdigestTest, AddSingleValue)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(100);
    ASSERT_NE(td, nullptr);

    urpc_tdigest_add(td, 42.0);
    EXPECT_EQ(urpc_tdigest_count(td), 1u);

    /* Min/max should track exactly */
    EXPECT_DOUBLE_EQ(td->min_val, 42.0);
    EXPECT_DOUBLE_EQ(td->max_val, 42.0);

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

TEST(UrpcTdigestTest, AddMultipleValues)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(100);
    ASSERT_NE(td, nullptr);

    double values[] = {10.0, 20.0, 30.0, 40.0, 50.0};
    urpc_tdigest_add_n(td, values, 5);
    EXPECT_EQ(urpc_tdigest_count(td), 5u);
    EXPECT_DOUBLE_EQ(td->min_val, 10.0);
    EXPECT_DOUBLE_EQ(td->max_val, 50.0);

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

TEST(UrpcTdigestTest, AddNullTdigest)
{
    /* Adding to NULL td should be a no-op */
    urpc_tdigest_add(nullptr, 42.0);
    urpc_tdigest_add_n(nullptr, nullptr, 0);
}

/* ─── 3. Quantile on small datasets ─── */

TEST(UrpcTdigestTest, QuantileSingleValue)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(100);
    ASSERT_NE(td, nullptr);

    urpc_tdigest_add(td, 42.0);

    /* Any quantile on a single-value dataset should return that value */
    EXPECT_DOUBLE_EQ(urpc_tdigest_quantile(td, 0.0), 42.0);
    EXPECT_DOUBLE_EQ(urpc_tdigest_quantile(td, 0.5), 42.0);
    EXPECT_DOUBLE_EQ(urpc_tdigest_quantile(td, 1.0), 42.0);

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

TEST(UrpcTdigestTest, QuantileEmptyTdigest)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(100);
    ASSERT_NE(td, nullptr);

    /* Empty tdigest should return 0 */
    EXPECT_DOUBLE_EQ(urpc_tdigest_quantile(td, 0.5), 0.0);

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

TEST(UrpcTdigestTest, QuantileBoundaryValues)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(100);
    ASSERT_NE(td, nullptr);

    double values[] = {1.0, 2.0, 3.0, 4.0, 5.0};
    urpc_tdigest_add_n(td, values, 5);

    /* q ≤ 0 → min */
    EXPECT_DOUBLE_EQ(urpc_tdigest_quantile(td, 0.0), 1.0);
    EXPECT_DOUBLE_EQ(urpc_tdigest_quantile(td, -0.5), 1.0);

    /* q ≥ 1 → max */
    EXPECT_DOUBLE_EQ(urpc_tdigest_quantile(td, 1.0), 5.0);
    EXPECT_DOUBLE_EQ(urpc_tdigest_quantile(td, 1.5), 5.0);

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

/* ─── 4. Quantile accuracy — uniform distribution ─── */

TEST(UrpcTdigestTest, QuantileUniformDistribution)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(200);
    ASSERT_NE(td, nullptr);

    /* Generate 10000 uniform samples in [0, 1000) */
    std::vector<double> samples;
    samples.reserve(10000);
    for (uint32_t i = 0; i < 10000; i++) {
        double v = (double)i / 10.0;
        urpc_tdigest_add(td, v);
        samples.push_back(v);
    }
    EXPECT_EQ(urpc_tdigest_count(td), 10000u);

    /* Median: should be close to 500 */
    double median = urpc_tdigest_quantile(td, 0.5);
    double exact_med = exact_quantile(samples, 0.5);
    EXPECT_NEAR(median, exact_med, exact_med * 0.05);  /* 5% tolerance for median */

    /* p90: should be close to 900 */
    double p90 = urpc_tdigest_quantile(td, 0.9);
    double exact_p90 = exact_quantile(samples, 0.9);
    EXPECT_NEAR(p90, exact_p90, exact_p90 * 0.03);  /* 3% tolerance for p90 */

    /* p99: should be close to 990 */
    double p99 = urpc_tdigest_quantile(td, 0.99);
    double exact_p99 = exact_quantile(samples, 0.99);
    EXPECT_NEAR(p99, exact_p99, exact_p99 * 0.02);  /* 2% tolerance for p99 */

    /* min/max should be exact */
    EXPECT_NEAR(td->min_val, 0.0, 1e-9);
    EXPECT_NEAR(td->max_val, 999.9, 1e-9);

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

/* ─── 5. Quantile accuracy — skewed distribution (lognormal-like) ─── */

TEST(UrpcTdigestTest, QuantileSkewedDistribution)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(200);
    ASSERT_NE(td, nullptr);

    /* Generate 10000 samples: mostly small values with a long tail */
    std::mt19937 rng(42);
    std::vector<double> samples;
    samples.reserve(10000);
    for (uint32_t i = 0; i < 10000; i++) {
        double v = std::exp(std::normal_distribution<double>(5.0, 1.0)(rng));
        urpc_tdigest_add(td, v);
        samples.push_back(v);
    }
    EXPECT_EQ(urpc_tdigest_count(td), 10000u);

    /* p99 — tdigest should be accurate at extreme quantiles */
    double td_p99 = urpc_tdigest_quantile(td, 0.99);
    double exact_p99 = exact_quantile(samples, 0.99);
    EXPECT_NEAR(td_p99, exact_p99, exact_p99 * 0.05);  /* 5% tolerance */

    /* p999 */
    double td_p999 = urpc_tdigest_quantile(td, 0.999);
    double exact_p999 = exact_quantile(samples, 0.999);
    EXPECT_NEAR(td_p999, exact_p999, exact_p999 * 0.10);  /* 10% tolerance for p999 */

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

/* ─── 6. Quantile accuracy — IO latency-like distribution ─── */

TEST(UrpcTdigestTest, QuantileIoLatencyDistribution)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(100);
    ASSERT_NE(td, nullptr);

    /*
     * Simulate IO latency: most ops are fast (100-500ns), but some are slow
     * (tail latencies up to 100us). This is the typical use case for tdigest.
     */
    std::mt19937 rng(123);
    std::vector<double> samples;
    samples.reserve(50000);
    for (uint32_t i = 0; i < 50000; i++) {
        double v;
        if (std::uniform_real_distribution<double>(0, 1)(rng) < 0.95) {
            /* 95% fast ops: 100-500 ns */
            v = std::uniform_real_distribution<double>(100, 500)(rng);
        } else if (std::uniform_real_distribution<double>(0, 1)(rng) < 0.9) {
            /* 4.5% medium ops: 500-5000 ns */
            v = std::uniform_real_distribution<double>(500, 5000)(rng);
        } else {
            /* 0.5% slow ops: 5000-100000 ns */
            v = std::uniform_real_distribution<double>(5000, 100000)(rng);
        }
        urpc_tdigest_add(td, v);
        samples.push_back(v);
    }
    EXPECT_EQ(urpc_tdigest_count(td), 50000u);

    /* p50 (median) — should be in the fast range */
    double p50 = urpc_tdigest_quantile(td, 0.5);
    double exact_p50 = exact_quantile(samples, 0.5);
    EXPECT_NEAR(p50, exact_p50, exact_p50 * 0.10);

    /* p99 — critical for latency monitoring */
    double p99 = urpc_tdigest_quantile(td, 0.99);
    double exact_p99 = exact_quantile(samples, 0.99);
    EXPECT_NEAR(p99, exact_p99, exact_p99 * 0.10);

    /* min/max exact */
    EXPECT_NEAR(td->min_val, *std::min_element(samples.begin(), samples.end()), 1e-9);
    EXPECT_NEAR(td->max_val, *std::max_element(samples.begin(), samples.end()), 1e-9);

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

/* ─── 7. Merge ─── */

TEST(UrpcTdigestTest, MergeTwoTdigests)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td1 = urpc_tdigest_create(100);
    urpc_tdigest_t *td2 = urpc_tdigest_create(100);
    ASSERT_NE(td1, nullptr);
    ASSERT_NE(td2, nullptr);

    /* td1: values 1-500 */
    std::vector<double> all_samples;
    for (uint32_t i = 1; i <= 500; i++) {
        urpc_tdigest_add(td1, (double)i);
        all_samples.push_back((double)i);
    }

    /* td2: values 501-1000 */
    for (uint32_t i = 501; i <= 1000; i++) {
        urpc_tdigest_add(td2, (double)i);
        all_samples.push_back((double)i);
    }

    /* Merge td2 into td1 */
    urpc_tdigest_merge(td1, td2);
    EXPECT_EQ(urpc_tdigest_count(td1), 1000u);

    /* After merge, td1 should approximate the combined distribution */
    double merged_p50 = urpc_tdigest_quantile(td1, 0.5);
    double exact_p50 = exact_quantile(all_samples, 0.5);
    EXPECT_NEAR(merged_p50, exact_p50, exact_p50 * 0.05);

    double merged_p99 = urpc_tdigest_quantile(td1, 0.99);
    double exact_p99 = exact_quantile(all_samples, 0.99);
    EXPECT_NEAR(merged_p99, exact_p99, exact_p99 * 0.03);

    /* min/max should be exact after merge */
    EXPECT_DOUBLE_EQ(td1->min_val, 1.0);
    EXPECT_DOUBLE_EQ(td1->max_val, 1000.0);

    urpc_tdigest_destroy(td1);
    urpc_tdigest_destroy(td2);

    urpc_dbuf_stat_record_disable();
}

TEST(UrpcTdigestTest, MergeIntoEmptyTdigest)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *dst = urpc_tdigest_create(100);
    urpc_tdigest_t *src = urpc_tdigest_create(100);
    ASSERT_NE(dst, nullptr);
    ASSERT_NE(src, nullptr);

    for (uint32_t i = 0; i < 100; i++) {
        urpc_tdigest_add(src, (double)i);
    }

    /* Merge non-empty src into empty dst */
    urpc_tdigest_merge(dst, src);
    EXPECT_EQ(urpc_tdigest_count(dst), 100u);

    double p50 = urpc_tdigest_quantile(dst, 0.5);
    EXPECT_NEAR(p50, 50.0, 5.0);

    urpc_tdigest_destroy(dst);
    urpc_tdigest_destroy(src);

    urpc_dbuf_stat_record_disable();
}

TEST(UrpcTdigestTest, MergeNullTdigests)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(100);
    ASSERT_NE(td, nullptr);

    /* Merge with NULL should be no-op */
    urpc_tdigest_merge(nullptr, nullptr);
    urpc_tdigest_merge(td, nullptr);
    urpc_tdigest_merge(nullptr, td);
    EXPECT_EQ(urpc_tdigest_count(td), 0u);

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

TEST(UrpcTdigestTest, MergeEmptySrc)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *dst = urpc_tdigest_create(100);
    urpc_tdigest_t *src = urpc_tdigest_create(100);
    ASSERT_NE(dst, nullptr);
    ASSERT_NE(src, nullptr);

    urpc_tdigest_add(dst, 42.0);

    /* Merge empty src into non-empty dst — should not change dst */
    urpc_tdigest_merge(dst, src);
    EXPECT_EQ(urpc_tdigest_count(dst), 1u);
    EXPECT_DOUBLE_EQ(urpc_tdigest_quantile(dst, 0.5), 42.0);

    urpc_tdigest_destroy(dst);
    urpc_tdigest_destroy(src);

    urpc_dbuf_stat_record_disable();
}

/* ─── 8. Reset ─── */

TEST(UrpcTdigestTest, ResetClearsAllData)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(100);
    ASSERT_NE(td, nullptr);

    for (uint32_t i = 0; i < 100; i++) {
        urpc_tdigest_add(td, (double)i);
    }
    EXPECT_EQ(urpc_tdigest_count(td), 100u);

    urpc_tdigest_reset(td);
    EXPECT_EQ(urpc_tdigest_count(td), 0u);
    EXPECT_EQ(td->centroid_count, 0u);
    EXPECT_EQ(td->buffer_count, 0u);
    EXPECT_EQ(td->min_val, INFINITY);
    EXPECT_EQ(td->max_val, -INFINITY);

    /* After reset, quantile on empty tdigest returns 0 */
    EXPECT_DOUBLE_EQ(urpc_tdigest_quantile(td, 0.5), 0.0);

    /* After reset, we can add new data */
    urpc_tdigest_add(td, 100.0);
    EXPECT_EQ(urpc_tdigest_count(td), 1u);
    EXPECT_DOUBLE_EQ(urpc_tdigest_quantile(td, 0.5), 100.0);

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

TEST(UrpcTdigestTest, ResetNull)
{
    urpc_tdigest_reset(nullptr);
}

/* ─── 9. Compress ─── */

TEST(UrpcTdigestTest, CompressFlushesBuffer)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(100);
    ASSERT_NE(td, nullptr);

    /* Add a few samples (< buffer_cap, so auto-compress won't trigger) */
    urpc_tdigest_add(td, 10.0);
    urpc_tdigest_add(td, 20.0);
    urpc_tdigest_add(td, 30.0);

    /* Before manual compress, buffer should hold the samples */
    EXPECT_EQ(td->buffer_count, 3u);
    EXPECT_EQ(td->centroid_count, 0u);

    /* After manual compress, buffer is flushed to centroids */
    urpc_tdigest_compress(td);
    EXPECT_EQ(td->buffer_count, 0u);
    EXPECT_GT(td->centroid_count, 0u);

    /* Quantile should work after compress */
    double p50 = urpc_tdigest_quantile(td, 0.5);
    EXPECT_NEAR(p50, 20.0, 5.0);

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

TEST(UrpcTdigestTest, CompressNull)
{
    urpc_tdigest_compress(nullptr);
}

/* ─── 10. Serialization / Deserialization ─── */

TEST(UrpcTdigestTest, SerialiseDeserialiseRoundtrip)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(100);
    ASSERT_NE(td, nullptr);

    /* Add some data */
    std::vector<double> samples;
    for (uint32_t i = 1; i <= 1000; i++) {
        urpc_tdigest_add(td, (double)i);
        samples.push_back((double)i);
    }

    /* Query size first */
    uint32_t required = urpc_tdigest_serialise(td, nullptr, 0);
    EXPECT_GT(required, 0u);

    /* Serialize */
    std::vector<uint8_t> buf(required);
    uint32_t written = urpc_tdigest_serialise(td, buf.data(), required);
    EXPECT_EQ(written, required);

    /* Deserialize */
    urpc_tdigest_t *td2 = urpc_tdigest_deserialise(buf.data(), written);
    ASSERT_NE(td2, nullptr);

    /* Count should match */
    EXPECT_EQ(urpc_tdigest_count(td2), urpc_tdigest_count(td));

    /* Quantiles should be close */
    double p50_orig = urpc_tdigest_quantile(td, 0.5);
    double p50_restored = urpc_tdigest_quantile(td2, 0.5);
    EXPECT_NEAR(p50_restored, p50_orig, p50_orig * 0.05);

    double p99_orig = urpc_tdigest_quantile(td, 0.99);
    double p99_restored = urpc_tdigest_quantile(td2, 0.99);
    EXPECT_NEAR(p99_restored, p99_orig, p99_orig * 0.05);

    /* min/max should match */
    EXPECT_DOUBLE_EQ(td2->min_val, td->min_val);
    EXPECT_DOUBLE_EQ(td2->max_val, td->max_val);

    urpc_tdigest_destroy(td);
    urpc_tdigest_destroy(td2);

    urpc_dbuf_stat_record_disable();
}

TEST(UrpcTdigestTest, SerialiseBufferTooSmall)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(100);
    ASSERT_NE(td, nullptr);

    urpc_tdigest_add(td, 42.0);

    uint8_t small_buf[1];
    uint32_t written = urpc_tdigest_serialise(td, small_buf, 1);
    EXPECT_EQ(written, 0u);  /* too small → returns 0 */

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

TEST(UrpcTdigestTest, SerialiseNull)
{
    uint32_t result = urpc_tdigest_serialise(nullptr, nullptr, 0);
    EXPECT_EQ(result, 0u);
}

TEST(UrpcTdigestTest, DeserialiseBadMagic)
{
    uint8_t buf[64] = {0};
    /* Write a wrong magic */
    *(uint32_t *)buf = 0xDEADBEEF;

    urpc_tdigest_t *td = urpc_tdigest_deserialise(buf, 64);
    EXPECT_EQ(td, nullptr);
}

TEST(UrpcTdigestTest, DeserialiseNullBuffer)
{
    urpc_tdigest_t *td = urpc_tdigest_deserialise(nullptr, 0);
    EXPECT_EQ(td, nullptr);
}

TEST(UrpcTdigestTest, DeserialiseBufferTooSmall)
{
    uint8_t buf[16] = {0};
    *(uint32_t *)buf = 0x54444753u;  /* valid magic */

    urpc_tdigest_t *td = urpc_tdigest_deserialise(buf, 16);  /* < 32 minimum */
    EXPECT_EQ(td, nullptr);
}

/* ─── 11. Stress: large number of samples ─── */

TEST(UrpcTdigestTest, StressLargeDataset)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(500);
    ASSERT_NE(td, nullptr);

    /* 1M samples */
    std::mt19937 rng(999);
    std::vector<double> samples;
    samples.reserve(1000000);
    for (uint32_t i = 0; i < 1000000; i++) {
        double v = std::uniform_real_distribution<double>(0, 100000)(rng);
        urpc_tdigest_add(td, v);
        samples.push_back(v);
    }
    EXPECT_EQ(urpc_tdigest_count(td), 1000000u);

    /* Centroid count should be bounded by max_centroids */
    urpc_tdigest_compress(td);
    EXPECT_LE(td->centroid_count, td->max_centroids);

    /* Quantile accuracy on large uniform dataset */
    double p99 = urpc_tdigest_quantile(td, 0.99);
    double exact_p99 = exact_quantile(samples, 0.99);
    EXPECT_NEAR(p99, exact_p99, exact_p99 * 0.02);

    double p999 = urpc_tdigest_quantile(td, 0.999);
    double exact_p999 = exact_quantile(samples, 0.999);
    EXPECT_NEAR(p999, exact_p999, exact_p999 * 0.02);

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

/* ─── 12. Merge then quantile (per-thread → global pattern) ─── */

TEST(UrpcTdigestTest, MergePerThreadToGlobal)
{
    urpc_dbuf_stat_record_enable();

    const uint32_t num_threads = 4;
    const uint32_t samples_per_thread = 2500;
    urpc_tdigest_t *global = urpc_tdigest_create(200);
    ASSERT_NE(global, nullptr);

    std::vector<double> all_samples;
    all_samples.reserve(num_threads * samples_per_thread);

    std::mt19937 rng(777);
    for (uint32_t t = 0; t < num_threads; t++) {
        urpc_tdigest_t *local = urpc_tdigest_create(100);
        ASSERT_NE(local, nullptr);

        for (uint32_t i = 0; i < samples_per_thread; i++) {
            double v = std::uniform_real_distribution<double>(0, 10000)(rng);
            urpc_tdigest_add(local, v);
            all_samples.push_back(v);
        }

        urpc_tdigest_merge(global, local);
        urpc_tdigest_destroy(local);
    }

    EXPECT_EQ(urpc_tdigest_count(global), num_threads * samples_per_thread);

    double global_p50 = urpc_tdigest_quantile(global, 0.5);
    double exact_p50 = exact_quantile(all_samples, 0.5);
    EXPECT_NEAR(global_p50, exact_p50, exact_p50 * 0.10);

    double global_p99 = urpc_tdigest_quantile(global, 0.99);
    double exact_p99 = exact_quantile(all_samples, 0.99);
    EXPECT_NEAR(global_p99, exact_p99, exact_p99 * 0.05);

    urpc_tdigest_destroy(global);

    urpc_dbuf_stat_record_disable();
}

/* ─── 13. Edge cases ─── */

TEST(UrpcTdigestTest, AllSameValues)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(100);
    ASSERT_NE(td, nullptr);

    /* All samples are identical */
    for (uint32_t i = 0; i < 10000; i++) {
        urpc_tdigest_add(td, 42.0);
    }

    EXPECT_DOUBLE_EQ(urpc_tdigest_quantile(td, 0.5), 42.0);
    EXPECT_DOUBLE_EQ(urpc_tdigest_quantile(td, 0.99), 42.0);
    EXPECT_DOUBLE_EQ(td->min_val, 42.0);
    EXPECT_DOUBLE_EQ(td->max_val, 42.0);

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

TEST(UrpcTdigestTest, NegativeValues)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(100);
    ASSERT_NE(td, nullptr);

    for (int32_t i = -500; i <= 500; i++) {
        urpc_tdigest_add(td, (double)i);
    }
    EXPECT_EQ(urpc_tdigest_count(td), 1001u);

    /* p50 should be near 0 */
    double p50 = urpc_tdigest_quantile(td, 0.5);
    EXPECT_NEAR(p50, 0.0, 25.0);

    /* min/max should be exact */
    EXPECT_DOUBLE_EQ(td->min_val, -500.0);
    EXPECT_DOUBLE_EQ(td->max_val, 500.0);

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

TEST(UrpcTdigestTest, VeryLargeValues)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(100);
    ASSERT_NE(td, nullptr);

    /* Simulate nanosecond latencies up to 1 second */
    double values[] = {100.0, 1000.0, 10000.0, 100000.0, 1000000.0, 10000000.0, 100000000.0, 1000000000.0};
    urpc_tdigest_add_n(td, values, 8);
    EXPECT_EQ(urpc_tdigest_count(td), 8u);

    EXPECT_DOUBLE_EQ(td->min_val, 100.0);
    EXPECT_DOUBLE_EQ(td->max_val, 1000000000.0);

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

/* ─── 14. Merge + Serialise roundtrip ─── */

TEST(UrpcTdigestTest, MergeThenSerialiseDeserialise)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td1 = urpc_tdigest_create(100);
    urpc_tdigest_t *td2 = urpc_tdigest_create(100);
    ASSERT_NE(td1, nullptr);
    ASSERT_NE(td2, nullptr);

    for (uint32_t i = 0; i < 100; i++) {
        urpc_tdigest_add(td1, (double)i);
    }
    for (uint32_t i = 100; i < 200; i++) {
        urpc_tdigest_add(td2, (double)i);
    }

    urpc_tdigest_merge(td1, td2);

    /* Serialize merged td1 */
    uint32_t required = urpc_tdigest_serialise(td1, nullptr, 0);
    std::vector<uint8_t> buf(required);
    uint32_t written = urpc_tdigest_serialise(td1, buf.data(), required);
    EXPECT_EQ(written, required);

    /* Deserialize and verify */
    urpc_tdigest_t *td3 = urpc_tdigest_deserialise(buf.data(), written);
    ASSERT_NE(td3, nullptr);
    EXPECT_EQ(urpc_tdigest_count(td3), 200u);

    double p50 = urpc_tdigest_quantile(td3, 0.5);
    EXPECT_NEAR(p50, 100.0, 10.0);

    urpc_tdigest_destroy(td1);
    urpc_tdigest_destroy(td2);
    urpc_tdigest_destroy(td3);

    urpc_dbuf_stat_record_disable();
}
