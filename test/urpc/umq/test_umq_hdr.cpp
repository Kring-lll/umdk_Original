#include <gtest/gtest.h>
#include <vector>
#include <thread>
#include <cstdint>

#include "hdr.h"

class HdrTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// basic create / record / min / max / sum
TEST_F(HdrTest, CreateRecordBasic)
{
    hdr_histogram_t *h = hdr_create();
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(h->total_count, 0u);

    hdr_record(h, 100);
    hdr_record(h, 200);
    hdr_record(h, 300);
    EXPECT_EQ(h->total_count, 3u);
    EXPECT_EQ(h->min_value, 100u);
    EXPECT_EQ(h->max_value, 300u);
    EXPECT_EQ(h->sum, 600u);
    hdr_destroy(h);
}

// empty histogram quantile returns 0
TEST_F(HdrTest, EmptyQuantileZero)
{
    hdr_histogram_t *h = hdr_create();
    EXPECT_EQ(hdr_value_at_quantile(h, 50.0), 0u);
    EXPECT_EQ(hdr_value_at_quantile(h, 99.0), 0u);
    EXPECT_EQ(hdr_value_at_quantile(h, 99.99), 0u);
    hdr_destroy(h);
}

// null pointers must not crash
TEST_F(HdrTest, NullSafe)
{
    hdr_destroy(nullptr);
    hdr_reset(nullptr);
    hdr_record(nullptr, 100);
    EXPECT_EQ(hdr_value_at_quantile(nullptr, 50.0), 0u);
    hdr_merge(nullptr, nullptr);
}

// reset clears all state
TEST_F(HdrTest, ResetClears)
{
    hdr_histogram_t *h = hdr_create();
    hdr_record(h, 100);
    hdr_record(h, 200);
    hdr_reset(h);
    EXPECT_EQ(h->total_count, 0u);
    EXPECT_EQ(hdr_value_at_quantile(h, 50.0), 0u);

    hdr_record(h, 50);
    EXPECT_EQ(h->total_count, 1u);
    EXPECT_EQ(h->min_value, 50u);
    EXPECT_EQ(h->max_value, 50u);
    hdr_destroy(h);
}

// boundary values map to valid indices without corruption
TEST_F(HdrTest, BoundaryValuesRecorded)
{
    std::vector<uint64_t> vals = {0, 1, 127, 128, 255, 256, 511, 512, 1023, 1024, 4095, 4096,
                                  65535, 65536, 1000000, HDR_HIGHEST_TRACKABLE - 1, HDR_HIGHEST_TRACKABLE,
                                  HDR_HIGHEST_TRACKABLE + 1000, HDR_HIGHEST_TRACKABLE * 4};
    hdr_histogram_t *h = hdr_create();
    for (uint64_t v : vals) {
        hdr_record(h, v);
    }
    EXPECT_EQ(h->total_count, vals.size());
    EXPECT_EQ(h->max_value, HDR_HIGHEST_TRACKABLE * 4);
    // p100 returns the real max
    EXPECT_EQ(hdr_value_at_quantile(h, 100.0), HDR_HIGHEST_TRACKABLE * 4);
    hdr_destroy(h);
}

// out-of-range values are clamped into the last bucket
TEST_F(HdrTest, ClampOutOfRange)
{
    hdr_histogram_t *h = hdr_create();
    hdr_record(h, HDR_HIGHEST_TRACKABLE * 2);
    hdr_record(h, HDR_HIGHEST_TRACKABLE * 3);
    EXPECT_EQ(h->total_count, 2u);
    uint64_t q = hdr_value_at_quantile(h, 50.0);
    EXPECT_GT(q, 0u);
    hdr_destroy(h);
}

// precision: uniform 1..N, each quantile within 1% of nearest-rank
TEST_F(HdrTest, PrecisionWithinOnePercent)
{
    const uint64_t N = 10000;
    hdr_histogram_t *h = hdr_create();
    for (uint64_t v = 1; v <= N; v++) {
        hdr_record(h, v);
    }
    double pcts[] = {50.0, 90.0, 99.0, 99.99};
    for (double pct : pcts) {
        // ceil((pct/100)*N) without libm
        double target = (pct / 100.0) * (double)N;
        uint64_t expected = (uint64_t)target;
        if ((double)expected < target) {
            expected++;
        }
        if (expected < 1) {
            expected = 1;
        }
        if (expected > N) {
            expected = N;
        }
        uint64_t got = hdr_value_at_quantile(h, pct);
        int64_t diff = (int64_t)got - (int64_t)expected;
        if (diff < 0) {
            diff = -diff;
        }
        double rel = (double)diff / (double)expected;
        EXPECT_LT(rel, 0.01) << "pct=" << pct << " got=" << got << " exp=" << expected;
    }
    hdr_destroy(h);
}

// monotonicity: p50 <= p90 <= p99 <= p9999 <= max
TEST_F(HdrTest, QuantileMonotonic)
{
    hdr_histogram_t *h = hdr_create();
    for (uint64_t v = 1; v <= 5000; v++) {
        hdr_record(h, v * 3); // spread values across buckets
    }
    uint64_t p50 = hdr_value_at_quantile(h, 50.0);
    uint64_t p90 = hdr_value_at_quantile(h, 90.0);
    uint64_t p99 = hdr_value_at_quantile(h, 99.0);
    uint64_t p9999 = hdr_value_at_quantile(h, 99.99);
    EXPECT_LE(p50, p90);
    EXPECT_LE(p90, p99);
    EXPECT_LE(p99, p9999);
    EXPECT_LE(p9999, h->max_value);
    hdr_destroy(h);
}

// memory budget: counts array <= 20KB, exactly 2048 buckets
TEST_F(HdrTest, MemoryBudgetUnder20KB)
{
    EXPECT_EQ(hdr_counts_size(), 2048u);
    size_t counts_bytes = hdr_counts_size() * sizeof(uint64_t);
    EXPECT_LE(counts_bytes, 20480u); // 20KB
    EXPECT_LE(sizeof(hdr_histogram_t), 64u); // struct header is tiny
}

// merge combines counts and extremes
TEST_F(HdrTest, MergeCombines)
{
    hdr_histogram_t *a = hdr_create();
    hdr_histogram_t *b = hdr_create();
    for (uint64_t v = 1; v <= 1000; v++) {
        hdr_record(a, v);
    }
    for (uint64_t v = 1001; v <= 2000; v++) {
        hdr_record(b, v);
    }
    hdr_merge(a, b);
    EXPECT_EQ(a->total_count, 2000u);
    EXPECT_EQ(a->min_value, 1u);
    EXPECT_EQ(a->max_value, 2000u);
    // p50 of 1..2000 ~ 1000
    uint64_t p50 = hdr_value_at_quantile(a, 50.0);
    EXPECT_GE(p50, 990u);
    EXPECT_LE(p50, 1010u);
    hdr_destroy(a);
    hdr_destroy(b);
}

// thread safety: per-thread histograms recorded concurrently, then merged (perf.c model)
TEST_F(HdrTest, ThreadSafetyPerThreadMerge)
{
    const int T = 8;
    const uint64_t M = 10000;
    std::vector<hdr_histogram_t *> hs(T);
    for (auto &h : hs) {
        h = hdr_create();
        ASSERT_NE(h, nullptr);
    }
    std::vector<std::thread> threads;
    for (int t = 0; t < T; t++) {
        threads.emplace_back([&hs, M, t]() {
            for (uint64_t v = 1; v <= M; v++) {
                hdr_record(hs[t], v);
            }
        });
    }
    for (auto &th : threads) {
        th.join();
    }
    hdr_histogram_t *total = hdr_create();
    for (auto h : hs) {
        hdr_merge(total, h);
    }
    EXPECT_EQ(total->total_count, (uint64_t)T * M);
    EXPECT_EQ(total->min_value, 1u);
    EXPECT_EQ(total->max_value, M);
    uint64_t p50 = hdr_value_at_quantile(total, 50.0);
    EXPECT_GE(p50, M / 2 - M / 100);
    EXPECT_LE(p50, M / 2 + M / 100);
    for (auto h : hs) {
        hdr_destroy(h);
    }
    hdr_destroy(total);
}
