/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: urpc tdigest unit test (integer arithmetic)
 */
#include "gtest/gtest.h"
#include "urpc_tdigest.h"
#include "urpc_dbuf_stat.h"

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <vector>
#include <random>

/* ─── Helper: compute exact quantile from a sorted array (uint64_t) ─── */
static uint64_t exact_quantile(const std::vector<uint64_t> &data, double q)
{
    if (data.empty()) {
        return 0;
    }
    std::vector<uint64_t> sorted = data;
    std::sort(sorted.begin(), sorted.end());

    double pos = q * (sorted.size() - 1);
    uint64_t lo = (uint64_t)std::floor(pos);
    uint64_t hi = (uint64_t)std::ceil(pos);
    if (lo == hi) {
        return sorted[lo];
    }
    double frac = pos - (double)lo;
    /* Integer linear interpolation, rounded */
    return sorted[lo] + (uint64_t)(frac * (double)(sorted[hi] - sorted[lo]));
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

    urpc_tdigest_add(td, 42);
    EXPECT_EQ(urpc_tdigest_count(td), 1u);

    /* Min/max should track exactly */
    EXPECT_EQ(td->min_val, 42u);
    EXPECT_EQ(td->max_val, 42u);

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

TEST(UrpcTdigestTest, AddMultipleValues)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(100);
    ASSERT_NE(td, nullptr);

    uint64_t values[] = {10, 20, 30, 40, 50};
    urpc_tdigest_add_n(td, values, 5);
    EXPECT_EQ(urpc_tdigest_count(td), 5u);
    EXPECT_EQ(td->min_val, 10u);
    EXPECT_EQ(td->max_val, 50u);

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

TEST(UrpcTdigestTest, AddNullTdigest)
{
    /* Adding to NULL td should be a no-op */
    urpc_tdigest_add(nullptr, 42);
    urpc_tdigest_add_n(nullptr, nullptr, 0);
}

/* ─── 3. Quantile on small datasets ─── */

TEST(UrpcTdigestTest, QuantileSingleValue)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(100);
    ASSERT_NE(td, nullptr);

    urpc_tdigest_add(td, 42);

    /* Any quantile on a single-value dataset should return that value */
    EXPECT_EQ(urpc_tdigest_quantile(td, 0.0), 42u);
    EXPECT_EQ(urpc_tdigest_quantile(td, 0.5), 42u);
    EXPECT_EQ(urpc_tdigest_quantile(td, 1.0), 42u);

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

TEST(UrpcTdigestTest, QuantileEmptyTdigest)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(100);
    ASSERT_NE(td, nullptr);

    /* Empty tdigest should return 0 */
    EXPECT_EQ(urpc_tdigest_quantile(td, 0.5), 0u);

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

TEST(UrpcTdigestTest, QuantileBoundaryValues)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(100);
    ASSERT_NE(td, nullptr);

    uint64_t values[] = {1, 2, 3, 4, 5};
    urpc_tdigest_add_n(td, values, 5);

    /* q <= 0 → min */
    EXPECT_EQ(urpc_tdigest_quantile(td, 0.0), 1u);
    EXPECT_EQ(urpc_tdigest_quantile(td, -0.5), 1u);

    /* q >= 1 → max */
    EXPECT_EQ(urpc_tdigest_quantile(td, 1.0), 5u);
    EXPECT_EQ(urpc_tdigest_quantile(td, 1.5), 5u);

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

/* ─── 4. Quantile accuracy — uniform distribution ─── */

TEST(UrpcTdigestTest, QuantileUniformDistribution)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(200);
    ASSERT_NE(td, nullptr);

    /* Generate 10000 uniform samples in [0, 10000] ns */
    std::vector<uint64_t> samples;
    samples.reserve(10000);
    for (uint32_t i = 0; i < 10000; i++) {
        uint64_t v = i;
        urpc_tdigest_add(td, v);
        samples.push_back(v);
    }
    EXPECT_EQ(urpc_tdigest_count(td), 10000u);

    /* p50: exact median = 5000, tolerance ~5% (250 ns) */
    uint64_t p50 = urpc_tdigest_quantile(td, 0.5);
    uint64_t exact_p50 = exact_quantile(samples, 0.5);
    EXPECT_LE(p50, exact_p50 + exact_p50 / 20);
    EXPECT_GE(p50, exact_p50 - exact_p50 / 20);

    /* p90: exact = 9000, tolerance ~3% (270 ns) */
    uint64_t p90 = urpc_tdigest_quantile(td, 0.9);
    uint64_t exact_p90 = exact_quantile(samples, 0.9);
    EXPECT_LE(p90, exact_p90 + exact_p90 / 33);
    EXPECT_GE(p90, exact_p90 - exact_p90 / 33);

    /* p99: exact = 9900, tolerance ~2% (198 ns) */
    uint64_t p99 = urpc_tdigest_quantile(td, 0.99);
    uint64_t exact_p99 = exact_quantile(samples, 0.99);
    EXPECT_LE(p99, exact_p99 + exact_p99 / 50);
    EXPECT_GE(p99, exact_p99 - exact_p99 / 50);

    /* min/max should be exact */
    EXPECT_EQ(td->min_val, 0u);
    EXPECT_EQ(td->max_val, 9999u);

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

/* ─── 5. Quantile accuracy — IO latency-like distribution ─── */

TEST(UrpcTdigestTest, QuantileIoLatencyDistribution)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(100);
    ASSERT_NE(td, nullptr);

    /*
     * Simulate IO latency: most ops are fast (100-500ns), but some are slow
     * (tail latencies up to 100us). This is the typical use case for tdigest.
     */
    std::mt19937_64 rng(123);
    std::vector<uint64_t> samples;
    samples.reserve(50000);
    for (uint32_t i = 0; i < 50000; i++) {
        uint64_t v;
        uint64_t r = rng() % 100;
        if (r < 95) {
            /* 95% fast ops: 100-500 ns */
            v = 100 + rng() % 400;
        } else if (r < 99) {
            /* 4% medium ops: 500-5000 ns */
            v = 500 + rng() % 4500;
        } else {
            /* 1% slow ops: 5000-100000 ns */
            v = 5000 + rng() % 95000;
        }
        urpc_tdigest_add(td, v);
        samples.push_back(v);
    }
    EXPECT_EQ(urpc_tdigest_count(td), 50000u);

    /* p50 — should be in the fast range (100-500 ns) */
    uint64_t p50 = urpc_tdigest_quantile(td, 0.5);
    uint64_t exact_p50 = exact_quantile(samples, 0.5);
    /* 10% tolerance */
    EXPECT_LE(p50, exact_p50 + MAX(exact_p50 / 10, 50));
    EXPECT_GE(p50, exact_p50 - MAX(exact_p50 / 10, 50));

    /* p99 — critical for latency monitoring */
    uint64_t p99 = urpc_tdigest_quantile(td, 0.99);
    uint64_t exact_p99 = exact_quantile(samples, 0.99);
    /* 10% tolerance, but at least 100 ns floor for small values */
    EXPECT_LE(p99, exact_p99 + MAX(exact_p99 / 10, 100));
    EXPECT_GE(p99, exact_p99 - MAX(exact_p99 / 10, 100));

    /* min/max exact */
    EXPECT_EQ(td->min_val, *std::min_element(samples.begin(), samples.end()));
    EXPECT_EQ(td->max_val, *std::max_element(samples.begin(), samples.end()));

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

/* ─── 6. Merge ─── */

TEST(UrpcTdigestTest, MergeTwoTdigests)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td1 = urpc_tdigest_create(100);
    urpc_tdigest_t *td2 = urpc_tdigest_create(100);
    ASSERT_NE(td1, nullptr);
    ASSERT_NE(td2, nullptr);

    /* td1: values 1-500 */
    std::vector<uint64_t> all_samples;
    for (uint32_t i = 1; i <= 500; i++) {
        urpc_tdigest_add(td1, i);
        all_samples.push_back(i);
    }

    /* td2: values 501-1000 */
    for (uint32_t i = 501; i <= 1000; i++) {
        urpc_tdigest_add(td2, i);
        all_samples.push_back(i);
    }

    /* Merge td2 into td1 */
    urpc_tdigest_merge(td1, td2);
    EXPECT_EQ(urpc_tdigest_count(td1), 1000u);

    /* After merge, td1 should approximate the combined distribution */
    uint64_t merged_p50 = urpc_tdigest_quantile(td1, 0.5);
    uint64_t exact_p50 = exact_quantile(all_samples, 0.5);
    EXPECT_LE(merged_p50, exact_p50 + exact_p50 / 20);
    EXPECT_GE(merged_p50, exact_p50 - exact_p50 / 20);

    uint64_t merged_p99 = urpc_tdigest_quantile(td1, 0.99);
    uint64_t exact_p99 = exact_quantile(all_samples, 0.99);
    EXPECT_LE(merged_p99, exact_p99 + exact_p99 / 33);
    EXPECT_GE(merged_p99, exact_p99 - exact_p99 / 33);

    /* min/max should be exact after merge */
    EXPECT_EQ(td1->min_val, 1u);
    EXPECT_EQ(td1->max_val, 1000u);

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
        urpc_tdigest_add(src, i);
    }

    /* Merge non-empty src into empty dst */
    urpc_tdigest_merge(dst, src);
    EXPECT_EQ(urpc_tdigest_count(dst), 100u);

    uint64_t p50 = urpc_tdigest_quantile(dst, 0.5);
    /* Exact median of 0..99 is 49 or 50, allow ±5 */
    EXPECT_LE(p50, 55u);
    EXPECT_GE(p50, 44u);

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

    urpc_tdigest_add(dst, 42);

    /* Merge empty src into non-empty dst — should not change dst */
    urpc_tdigest_merge(dst, src);
    EXPECT_EQ(urpc_tdigest_count(dst), 1u);
    EXPECT_EQ(urpc_tdigest_quantile(dst, 0.5), 42u);

    urpc_tdigest_destroy(dst);
    urpc_tdigest_destroy(src);

    urpc_dbuf_stat_record_disable();
}

/* ─── 7. Reset ─── */

TEST(UrpcTdigestTest, ResetClearsAllData)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(100);
    ASSERT_NE(td, nullptr);

    for (uint32_t i = 0; i < 100; i++) {
        urpc_tdigest_add(td, i);
    }
    EXPECT_EQ(urpc_tdigest_count(td), 100u);

    urpc_tdigest_reset(td);
    EXPECT_EQ(urpc_tdigest_count(td), 0u);
    EXPECT_EQ(td->centroid_count, 0u);
    EXPECT_EQ(td->buffer_count, 0u);
    /* min_val reset to UINT64_MAX, max_val to 0 */
    EXPECT_EQ(td->min_val, UINT64_MAX);
    EXPECT_EQ(td->max_val, 0u);

    /* After reset, quantile on empty tdigest returns 0 */
    EXPECT_EQ(urpc_tdigest_quantile(td, 0.5), 0u);

    /* After reset, we can add new data */
    urpc_tdigest_add(td, 100);
    EXPECT_EQ(urpc_tdigest_count(td), 1u);
    EXPECT_EQ(urpc_tdigest_quantile(td, 0.5), 100u);

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

TEST(UrpcTdigestTest, ResetNull)
{
    urpc_tdigest_reset(nullptr);
}

/* ─── 8. Compress ─── */

TEST(UrpcTdigestTest, CompressFlushesBuffer)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(100);
    ASSERT_NE(td, nullptr);

    /* Add a few samples (< buffer_cap, so auto-compress won't trigger) */
    urpc_tdigest_add(td, 10);
    urpc_tdigest_add(td, 20);
    urpc_tdigest_add(td, 30);

    /* Before manual compress, buffer should hold the samples */
    EXPECT_EQ(td->buffer_count, 3u);
    EXPECT_EQ(td->centroid_count, 0u);

    /* After manual compress, buffer is flushed to centroids */
    urpc_tdigest_compress(td);
    EXPECT_EQ(td->buffer_count, 0u);
    EXPECT_GT(td->centroid_count, 0u);

    /* Quantile should work after compress */
    uint64_t p50 = urpc_tdigest_quantile(td, 0.5);
    /* For {10, 20, 30}, median ≈ 20, allow ±5 */
    EXPECT_LE(p50, 25u);
    EXPECT_GE(p50, 15u);

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

TEST(UrpcTdigestTest, CompressNull)
{
    urpc_tdigest_compress(nullptr);
}

/* ─── 9. Serialization / Deserialization ─── */

TEST(UrpcTdigestTest, SerialiseDeserialiseRoundtrip)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(100);
    ASSERT_NE(td, nullptr);

    /* Add some data */
    std::vector<uint64_t> samples;
    for (uint32_t i = 1; i <= 1000; i++) {
        urpc_tdigest_add(td, i);
        samples.push_back(i);
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

    /* Quantiles should be close (integer tolerance: 5%) */
    uint64_t p50_orig = urpc_tdigest_quantile(td, 0.5);
    uint64_t p50_restored = urpc_tdigest_quantile(td2, 0.5);
    EXPECT_LE(p50_restored, p50_orig + p50_orig / 20);
    EXPECT_GE(p50_restored, p50_orig - p50_orig / 20);

    uint64_t p99_orig = urpc_tdigest_quantile(td, 0.99);
    uint64_t p99_restored = urpc_tdigest_quantile(td2, 0.99);
    EXPECT_LE(p99_restored, p99_orig + p99_orig / 20);
    EXPECT_GE(p99_restored, p99_orig - p99_orig / 20);

    /* min/max should match exactly */
    EXPECT_EQ(td2->min_val, td->min_val);
    EXPECT_EQ(td2->max_val, td->max_val);

    urpc_tdigest_destroy(td);
    urpc_tdigest_destroy(td2);

    urpc_dbuf_stat_record_disable();
}

TEST(UrpcTdigestTest, SerialiseBufferTooSmall)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(100);
    ASSERT_NE(td, nullptr);

    urpc_tdigest_add(td, 42);

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

/* ─── 10. Stress: large number of samples ─── */

TEST(UrpcTdigestTest, StressLargeDataset)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(500);
    ASSERT_NE(td, nullptr);

    /* 100K samples */
    std::mt19937_64 rng(999);
    std::vector<uint64_t> samples;
    samples.reserve(100000);
    for (uint32_t i = 0; i < 100000; i++) {
        uint64_t v = rng() % 100000;
        urpc_tdigest_add(td, v);
        samples.push_back(v);
    }
    EXPECT_EQ(urpc_tdigest_count(td), 100000u);

    /* Centroid count should be bounded by max_centroids */
    urpc_tdigest_compress(td);
    EXPECT_LE(td->centroid_count, td->max_centroids);

    /* Quantile accuracy on large dataset */
    uint64_t p99 = urpc_tdigest_quantile(td, 0.99);
    uint64_t exact_p99 = exact_quantile(samples, 0.99);
    /* 2% tolerance */
    EXPECT_LE(p99, exact_p99 + MAX(exact_p99 / 50, 200));
    EXPECT_GE(p99, exact_p99 - MAX(exact_p99 / 50, 200));

    uint64_t p999 = urpc_tdigest_quantile(td, 0.999);
    uint64_t exact_p999 = exact_quantile(samples, 0.999);
    /* 2% tolerance */
    EXPECT_LE(p999, exact_p999 + MAX(exact_p999 / 50, 200));
    EXPECT_GE(p999, exact_p999 - MAX(exact_p999 / 50, 200));

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

/* ─── 11. Merge then quantile (per-thread → global pattern) ─── */

TEST(UrpcTdigestTest, MergePerThreadToGlobal)
{
    urpc_dbuf_stat_record_enable();

    const uint32_t num_threads = 4;
    const uint32_t samples_per_thread = 2500;
    urpc_tdigest_t *global = urpc_tdigest_create(200);
    ASSERT_NE(global, nullptr);

    std::vector<uint64_t> all_samples;
    all_samples.reserve(num_threads * samples_per_thread);

    std::mt19937_64 rng(777);
    for (uint32_t t = 0; t < num_threads; t++) {
        urpc_tdigest_t *local = urpc_tdigest_create(100);
        ASSERT_NE(local, nullptr);

        for (uint32_t i = 0; i < samples_per_thread; i++) {
            uint64_t v = rng() % 10000;
            urpc_tdigest_add(local, v);
            all_samples.push_back(v);
        }

        urpc_tdigest_merge(global, local);
        urpc_tdigest_destroy(local);
    }

    EXPECT_EQ(urpc_tdigest_count(global), num_threads * samples_per_thread);

    uint64_t global_p50 = urpc_tdigest_quantile(global, 0.5);
    uint64_t exact_p50 = exact_quantile(all_samples, 0.5);
    /* 10% tolerance */
    EXPECT_LE(global_p50, exact_p50 + MAX(exact_p50 / 10, 500));
    EXPECT_GE(global_p50, exact_p50 - MAX(exact_p50 / 10, 500));

    uint64_t global_p99 = urpc_tdigest_quantile(global, 0.99);
    uint64_t exact_p99 = exact_quantile(all_samples, 0.99);
    /* 5% tolerance */
    EXPECT_LE(global_p99, exact_p99 + MAX(exact_p99 / 20, 500));
    EXPECT_GE(global_p99, exact_p99 - MAX(exact_p99 / 20, 500));

    urpc_tdigest_destroy(global);

    urpc_dbuf_stat_record_disable();
}

/* ─── 12. Edge cases ─── */

TEST(UrpcTdigestTest, AllSameValues)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(100);
    ASSERT_NE(td, nullptr);

    /* All samples are identical */
    for (uint32_t i = 0; i < 10000; i++) {
        urpc_tdigest_add(td, 42);
    }

    EXPECT_EQ(urpc_tdigest_quantile(td, 0.5), 42u);
    EXPECT_EQ(urpc_tdigest_quantile(td, 0.99), 42u);
    EXPECT_EQ(td->min_val, 42u);
    EXPECT_EQ(td->max_val, 42u);

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

TEST(UrpcTdigestTest, VeryLargeValues)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td = urpc_tdigest_create(100);
    ASSERT_NE(td, nullptr);

    /* Simulate nanosecond latencies up to 1 second */
    uint64_t values[] = {100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};
    urpc_tdigest_add_n(td, values, 8);
    EXPECT_EQ(urpc_tdigest_count(td), 8u);

    EXPECT_EQ(td->min_val, 100u);
    EXPECT_EQ(td->max_val, 1000000000u);

    urpc_tdigest_destroy(td);

    urpc_dbuf_stat_record_disable();
}

/* ─── 13. Merge + Serialise roundtrip ─── */

TEST(UrpcTdigestTest, MergeThenSerialiseDeserialise)
{
    urpc_dbuf_stat_record_enable();

    urpc_tdigest_t *td1 = urpc_tdigest_create(100);
    urpc_tdigest_t *td2 = urpc_tdigest_create(100);
    ASSERT_NE(td1, nullptr);
    ASSERT_NE(td2, nullptr);

    for (uint32_t i = 0; i < 100; i++) {
        urpc_tdigest_add(td1, i);
    }
    for (uint32_t i = 100; i < 200; i++) {
        urpc_tdigest_add(td2, i);
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

    uint64_t p50 = urpc_tdigest_quantile(td3, 0.5);
    /* Exact median of 0..199 ≈ 100, allow ±10 */
    EXPECT_LE(p50, 110u);
    EXPECT_GE(p50, 90u);

    urpc_tdigest_destroy(td1);
    urpc_tdigest_destroy(td2);
    urpc_tdigest_destroy(td3);

    urpc_dbuf_stat_record_disable();
}
