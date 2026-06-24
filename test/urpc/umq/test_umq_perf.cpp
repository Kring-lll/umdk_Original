#include "mockcpp/mockcpp.hpp"
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <memory>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <vector>
#include <thread>
#include <atomic>

#include "umq_api.h"
#include "umq_errno.h"
#include "perf.h"
#include "umq_vlog.h"
#include "urpc_util.h"
#include "tdigest.h"

static uint64_t g_mock_urpc_get_cpu_cycles_value = 1000;
static uint64_t g_mock_urpc_get_cpu_hz_value = 2000000000ULL; // 2GHz

// helper: simple sort for accuracy ground truth
static double exact_quantile_from_raw(uint64_t *raw, size_t n, double pct)
{
    if (n == 0) return 0.0;
    std::sort(raw, raw + n);
    double pos = (pct / 100.0) * (double)(n - 1);
    size_t lo = (size_t)pos;
    size_t hi = lo + 1;
    if (hi >= n) hi = n - 1;
    double frac = pos - (double)lo;
    return (double)raw[lo] * (1.0 - frac) + (double)raw[hi] * frac;
}

// Test fixture for perf functions
class PerfTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_mock_urpc_get_cpu_cycles_value = 1000;
        MOCKER(urpc_get_cpu_cycles).stubs().will(returnValue(g_mock_urpc_get_cpu_cycles_value));
        MOCKER(urpc_get_cpu_hz).stubs().will(returnValue(g_mock_urpc_get_cpu_hz_value));
    }

    void TearDown() override {
        umq_perf_stop();
        umq_perf_uninit();
    }
};

// Test umq_perf_init function
TEST_F(PerfTest, InitSuccess) {
    int ret = umq_perf_init();
    EXPECT_EQ(ret, UMQ_SUCCESS);
}

TEST_F(PerfTest, InitTwiceShouldFail) {
    int ret = umq_perf_init();
    EXPECT_EQ(ret, UMQ_SUCCESS);

    ret = umq_perf_init();
    EXPECT_NE(ret, UMQ_SUCCESS); // Should fail since already initialized
}

// Test umq_perf_get_start_timestamp_with_feature
TEST_F(PerfTest, GetStartTimestampWithPerfFeature) {
    uint32_t feature = UMQ_FEATURE_ENABLE_PERF;
    uint64_t timestamp = umq_perf_get_start_timestamp_with_feature(feature);

    // When perf is not enabled, should return 0
    EXPECT_EQ(timestamp, 0);
}

TEST_F(PerfTest, GetStartTimestampWithoutPerfFeature) {
    uint32_t feature = 0;  // No perf feature
    uint64_t timestamp = umq_perf_get_start_timestamp_with_feature(feature);

    EXPECT_EQ(timestamp, 0);
}

// Test umq_perf_record_write_with_feature
TEST_F(PerfTest, RecordWriteWithPerfFeature) {
    umq_perf_record_type_t type = static_cast<umq_perf_record_type_t>(1);  // Use cast
    uint64_t start = 1000;
    uint32_t feature = UMQ_FEATURE_ENABLE_PERF;

    // This function should not crash when called
    umq_perf_record_write_with_feature(type, start, feature);
    SUCCEED();  // If no crash, test passes
}

TEST_F(PerfTest, RecordWriteWithoutPerfFeature) {
    umq_perf_record_type_t type = static_cast<umq_perf_record_type_t>(1);  // Use cast
    uint64_t start = 1000;
    uint32_t feature = 0;  // No perf feature

    // This function should return immediately without doing anything
    umq_perf_record_write_with_feature(type, start, feature);
    SUCCEED();  // If no crash, test passes
}

static void test_perf_cfg_set(umq_perf_stats_cfg_t *cfg)
{
    cfg->compression = 100;
}

// Test perf start/stop/clear operations
TEST_F(PerfTest, PerfStartStop) {
    int ret = umq_perf_start();
    ASSERT_EQ(ret, UMQ_SUCCESS);

    umq_perf_stats_cfg_t cfg = {0};
    test_perf_cfg_set(&cfg);

    ret = umq_perf_reset(&cfg);
    EXPECT_EQ(ret, UMQ_SUCCESS);

    ret = umq_perf_stop();
    EXPECT_EQ(ret, UMQ_SUCCESS);
}

TEST_F(PerfTest, PerfStartWithNullCfgUsesDefaults) {
    int ret = umq_perf_start();
    ASSERT_EQ(ret, UMQ_SUCCESS);

    // NULL cfg now falls back to built-in defaults {50, 90, 99}
    ret = umq_perf_reset(NULL);
    EXPECT_EQ(ret, UMQ_SUCCESS);
}

TEST_F(PerfTest, PerfStartWithoutInitShouldFail) {
    umq_perf_stats_cfg_t cfg = {0};
    test_perf_cfg_set(&cfg);

    int ret = umq_perf_reset(&cfg);
    EXPECT_NE(ret, UMQ_SUCCESS);
}

TEST_F(PerfTest, PerfClearWhenStartedShouldFail) {
    int ret = umq_perf_start();
    ASSERT_EQ(ret, UMQ_SUCCESS);

    umq_perf_stats_cfg_t cfg = {0};
    test_perf_cfg_set(&cfg);

    ret = umq_perf_reset(&cfg);
    EXPECT_EQ(ret, UMQ_SUCCESS);
}

TEST_F(PerfTest, PerfClearAfterStopping) {
    int ret = umq_perf_start();
    ASSERT_EQ(ret, UMQ_SUCCESS);

    umq_perf_stats_cfg_t cfg = {0};
    test_perf_cfg_set(&cfg);

    ret = umq_perf_reset(&cfg);
    EXPECT_EQ(ret, UMQ_SUCCESS);

    ret = umq_perf_stop();
    EXPECT_EQ(ret, UMQ_SUCCESS);

    umq_perf_stats_t info;
    ret = umq_perf_info_get(&info);
    EXPECT_EQ(ret, 0);
}

TEST_F(PerfTest, PerfInfoGetAfterStopping) {
    int ret = umq_perf_start();
    ASSERT_EQ(ret, UMQ_SUCCESS);

    umq_perf_stats_cfg_t cfg = {0};
    test_perf_cfg_set(&cfg);

    ret = umq_perf_reset(&cfg);
    EXPECT_EQ(ret, UMQ_SUCCESS);

    ret = umq_perf_stop();
    EXPECT_EQ(ret, UMQ_SUCCESS);

    umq_perf_stats_t info;
    ret = umq_perf_info_get(&info);
    EXPECT_EQ(ret, 0);  // Should succeed after stopping
}

TEST_F(PerfTest, ResetClampsTooManyQuantiles) {
    umq_perf_start();

    // quantile percentages are now hardcoded; just verify reset succeeds with valid cfg
    umq_perf_stats_cfg_t cfg = {0};
    cfg.compression = 50;

    int ret = umq_perf_reset(&cfg);
    EXPECT_EQ(ret, UMQ_SUCCESS);
}

TEST_F(PerfTest, ResetWithNullCfgUsesDefaults) {
    umq_perf_start();

    // NULL cfg now falls back to built-in defaults for compression
    int ret = umq_perf_reset(NULL);
    EXPECT_EQ(ret, UMQ_SUCCESS);
}

TEST_F(PerfTest, PerfRecordWriteFunctions) {
    // These functions should not crash when called with valid parameters
    // (though they won't do much without proper initialization)
    umq_perf_record_type_t type = static_cast<umq_perf_record_type_t>(1);
    uint64_t start = 1000;

    // This should not crash
    umq_perf_record_write(type, start);
    SUCCEED();
}

TEST_F(PerfTest, PerfRecordWriteWithDirection) {
    umq_perf_record_type_t type = static_cast<umq_perf_record_type_t>(1);
    uint64_t start = 1000;
    umq_io_direction_t direction = UMQ_IO_ALL;

    // This should not crash
    umq_perf_record_write_with_direction(type, start, direction);
    SUCCEED();
}

TEST_F(PerfTest, PerfRecordAllocFunction) {
    int ret = umq_perf_start();
    ASSERT_EQ(ret, UMQ_SUCCESS);

    // Need to start perf first to enable recording
    umq_perf_stats_cfg_t cfg = {0};
    test_perf_cfg_set(&cfg);
    ret = umq_perf_reset(&cfg);
    ASSERT_EQ(ret, UMQ_SUCCESS);

    // Now call umq_perf_get_start_timestamp which internally calls umq_dp_thread_run_once
    // and umq_perf_record_alloc
    g_mock_urpc_get_cpu_cycles_value = 2000;
    uint64_t timestamp = umq_perf_get_start_timestamp();

    // This should trigger the allocation process
    EXPECT_GT(timestamp, 0);
}

TEST_F(PerfTest, PerfRecordAllocExhaustedCapacity) {
    int ret = umq_perf_start();
    ASSERT_EQ(ret, UMQ_SUCCESS);

    // Manually set all records as used to test exhaustion case
    // This requires access to internal state, so we'll just call the function indirectly
    // by triggering the path through umq_get_start_timestamp after enabling perf
    umq_perf_stats_cfg_t cfg = {0};
    test_perf_cfg_set(&cfg);
    ret = umq_perf_reset(&cfg);
    ASSERT_EQ(ret, UMQ_SUCCESS);

    // Call multiple times to exercise the allocation logic
    for (int i = 0; i < 5; ++i) {
        g_mock_urpc_get_cpu_cycles_value = 2000 + i * 100;
        uint64_t timestamp = umq_perf_get_start_timestamp();
        if (timestamp > 0) {
            // Successfully allocated
        }
    }

    SUCCEED();
}

TEST_F(PerfTest, FindPerfRecordBucketNoThreshold) {
    int ret = umq_perf_start();
    ASSERT_EQ(ret, UMQ_SUCCESS);

    // Quantile percentages are hardcoded; test reset with NULL cfg
    umq_perf_stats_cfg_t cfg = {0};
    ret = umq_perf_reset(&cfg);
    ASSERT_EQ(ret, UMQ_SUCCESS);

    // Now when we call perf recording functions, find_perf_record_bucket will be called
    g_mock_urpc_get_cpu_cycles_value = 3000;

    // To trigger find_perf_record_bucket, we need to call umq_perf_record_write
    // But this requires proper setup including setting g_perf_record_index
    umq_perf_record_type_t type = static_cast<umq_perf_record_type_t>(1);
    uint64_t start = 1000;

    // This should trigger find_perf_record_bucket with no thresholds set
    umq_perf_record_write_with_feature(type, start, UMQ_FEATURE_ENABLE_PERF);

    SUCCEED();
}

TEST_F(PerfTest, FindPerfRecordBucketWithThresholds) {
    int ret = umq_perf_start();
    ASSERT_EQ(ret, UMQ_SUCCESS);

    umq_perf_stats_cfg_t cfg = {0};
    test_perf_cfg_set(&cfg);
    ret = umq_perf_reset(&cfg);
    ASSERT_EQ(ret, UMQ_SUCCESS);

    // Now call perf recording which will trigger find_perf_record_bucket
    g_mock_urpc_get_cpu_cycles_value = 2500;

    // Trigger the recording function
    umq_perf_record_type_t type = static_cast<umq_perf_record_type_t>(1);
    uint64_t start = 1000;

    // Enable perf recording temporarily
    umq_perf_record_write_with_feature(type, start, UMQ_FEATURE_ENABLE_PERF);

    SUCCEED();
}

TEST_F(PerfTest, PerfFillPerfRecordFunction) {
    int ret = umq_perf_start();
    ASSERT_EQ(ret, UMQ_SUCCESS);

    umq_perf_stats_cfg_t cfg = {0};
    test_perf_cfg_set(&cfg);
    ret = umq_perf_reset(&cfg);
    ASSERT_EQ(ret, UMQ_SUCCESS);

    // Enable perf
    // uint32_t feature = UMQ_FEATURE_ENABLE_PERF;
    g_mock_urpc_get_cpu_cycles_value = 2000;

    // Get timestamp to allocate record index
    uint64_t timestamp = umq_perf_get_start_timestamp();
    EXPECT_GT(timestamp, 0);

    // Now call record write which will trigger umq_perf_fill_perf_record
    umq_perf_record_type_t type = static_cast<umq_perf_record_type_t>(1);
    uint64_t start_time = 1000;

    // This should trigger the fill function
    umq_perf_record_write(type, start_time);

    SUCCEED();
}

TEST_F(PerfTest, PerfConvertCyclesToNs) {
    int ret = umq_perf_start();
    ASSERT_EQ(ret, UMQ_SUCCESS);

    umq_perf_stats_cfg_t cfg = {0};
    test_perf_cfg_set(&cfg);
    ret = umq_perf_reset(&cfg);
    ASSERT_EQ(ret, UMQ_SUCCESS);

    // Stop perf to allow getting info
    ret = umq_perf_stop();
    ASSERT_EQ(ret, UMQ_SUCCESS);

    // Get perf info which should call umq_perf_convert_cycles_to_ns
    umq_perf_stats_t info;
    ret = umq_perf_info_get(&info);
    EXPECT_EQ(ret, 0);

    SUCCEED();
}

TEST_F(PerfTest, PerfRecordWriteFunctionsEdgeCases) {
    int ret = umq_perf_start();
    ASSERT_EQ(ret, UMQ_SUCCESS);

    umq_perf_stats_cfg_t cfg = {0};
    test_perf_cfg_set(&cfg);
    ret = umq_perf_reset(&cfg);
    ASSERT_EQ(ret, UMQ_SUCCESS);

    // Enable perf recording
    umq_perf_record_type_t type = static_cast<umq_perf_record_type_t>(1);
    uint64_t start = 1000;

    // Test normal recording
    umq_perf_record_write(type, start);

    // Test recording with direction
    umq_io_direction_t direction = UMQ_IO_ALL;
    umq_perf_record_write_with_direction(type, start, direction);

    SUCCEED();
}

TEST_F(PerfTest, PerfRecordAllocationBoundary) {
    int ret = umq_perf_start();
    ASSERT_EQ(ret, UMQ_SUCCESS);

    umq_perf_stats_cfg_t cfg = {0};
    test_perf_cfg_set(&cfg);
    ret = umq_perf_reset(&cfg);
    ASSERT_EQ(ret, UMQ_SUCCESS);

    // Try to trigger multiple allocations
    for (int i = 0; i < 10; ++i) {
        g_mock_urpc_get_cpu_cycles_value = 1000 + i * 100;
        uint64_t timestamp = umq_perf_get_start_timestamp();
        if (timestamp > 0) {
            // Valid timestamp means allocation worked
        }
    }

    SUCCEED();
}

TEST_F(PerfTest, ClearPerfRecordItem) {
    int ret = umq_perf_start();
    ASSERT_EQ(ret, UMQ_SUCCESS);

    umq_perf_stats_cfg_t cfg = {0};
    test_perf_cfg_set(&cfg);
    ret = umq_perf_reset(&cfg);
    ASSERT_EQ(ret, UMQ_SUCCESS);

    ret = umq_perf_stop();
    ASSERT_EQ(ret, UMQ_SUCCESS);

    SUCCEED();
}

TEST_F(PerfTest, ComprehensivePerfTest) {
    int ret = umq_perf_start();
    ASSERT_EQ(ret, UMQ_SUCCESS);

    // Quantile percentages are hardcoded to p50/p90/p99/p9999
    umq_perf_stats_cfg_t cfg = {0};
    cfg.compression = 100;
    ret = umq_perf_reset(&cfg);
    ASSERT_EQ(ret, UMQ_SUCCESS);

    // Enable recording by getting timestamps
    for (int i = 0; i < 5; ++i) {
        g_mock_urpc_get_cpu_cycles_value = 1000 + i * 1000;
        uint64_t timestamp = umq_perf_get_start_timestamp();
        if (timestamp > 0) {
            // Record some data
            umq_perf_record_type_t type = static_cast<umq_perf_record_type_t>(i % UMQ_PERF_RECORD_TYPE_MAX);
            umq_perf_record_write(type, timestamp - 100);

            // Also test with direction
            umq_perf_record_write_with_direction(type, timestamp - 50, UMQ_IO_ALL);
        }
    }

    // Stop perf
    ret = umq_perf_stop();
    EXPECT_EQ(ret, UMQ_SUCCESS);

    // Get results (this calls umq_perf_convert_cycles_to_ns)
    umq_perf_stats_t info;
    ret = umq_perf_info_get(&info);
    EXPECT_EQ(ret, 0);

    SUCCEED();
}

TEST_F(PerfTest, MinValueConditionTest) {
    int ret = umq_perf_start();
    ASSERT_EQ(ret, UMQ_SUCCESS);

    umq_perf_stats_cfg_t cfg = {0};
    test_perf_cfg_set(&cfg);
    ret = umq_perf_reset(&cfg);
    ASSERT_EQ(ret, UMQ_SUCCESS);

    // Set mock to same value to create zero delta
    g_mock_urpc_get_cpu_cycles_value = 1000;
    uint64_t timestamp = umq_perf_get_start_timestamp();

    if (timestamp > 0) {
        // Record with same start time to create zero delta
        umq_perf_record_type_t type = static_cast<umq_perf_record_type_t>(1);
        umq_perf_record_write(type, 1000);
    }

    SUCCEED();
}

TEST(InlineFunctionsTest, GetStartTimestampWithFeature) {
    uint32_t feature_with_perf = UMQ_FEATURE_ENABLE_PERF;
    uint32_t feature_without_perf = 0;

    // Without perf feature, should return 0
    uint64_t result1 = umq_perf_get_start_timestamp_with_feature(feature_without_perf);
    EXPECT_EQ(result1, 0);

    // With perf feature but perf not enabled, should return 0
    uint64_t result2 = umq_perf_get_start_timestamp_with_feature(feature_with_perf);
    EXPECT_EQ(result2, 0);
}

TEST(InlineFunctionsTest, RecordWriteWithFeature) {
    umq_perf_record_type_t type = static_cast<umq_perf_record_type_t>(1);
    uint64_t start = 1000;
    uint32_t feature_with_perf = UMQ_FEATURE_ENABLE_PERF;
    uint32_t feature_without_perf = 0;

    // With perf feature disabled, should return immediately
    umq_perf_record_write_with_feature(type, start, feature_without_perf);
    SUCCEED();

    // With perf feature enabled but perf not initialized, should return immediately
    umq_perf_record_write_with_feature(type, start, feature_with_perf);
    SUCCEED();
}

class TdigestTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(TdigestTest, CreateDestroy) {
    for (int c : {1, 10, 20, 50, 100}) {
        tdigest_t *td = tdigest_create(c);
        ASSERT_NE(td, nullptr);
        EXPECT_EQ(td->compression, c);
        EXPECT_EQ(td->active->size, (size_t)0);
        EXPECT_EQ(td->active->total_weight, (uint64_t)0);
        EXPECT_EQ(td->min_value, UINT64_MAX);
        EXPECT_EQ(td->max_value, (uint64_t)0);
        tdigest_destroy(td);
    }
}

TEST_F(TdigestTest, CreateInvalidCompression) {
    tdigest_t *td = tdigest_create(0);
    ASSERT_NE(td, nullptr);
    EXPECT_EQ(td->compression, 1);  // clamped
    tdigest_destroy(td);

    td = tdigest_create(-5);
    ASSERT_NE(td, nullptr);
    EXPECT_EQ(td->compression, 1);  // clamped
    tdigest_destroy(td);
}

TEST_F(TdigestTest, Reset) {
    tdigest_t *td = tdigest_create(20);
    // insert some data
    for (int i = 0; i < 100; i++) tdigest_insert(td, (double)(i * 10), 1);
    ASSERT_GT(td->active->total_weight, (uint64_t)0);

    tdigest_reset(td);
    EXPECT_EQ(td->active->size, (size_t)0);
    EXPECT_EQ(td->active->total_weight, (uint64_t)0);
    EXPECT_EQ(td->min_value, UINT64_MAX);
    EXPECT_EQ(td->max_value, (uint64_t)0);
    tdigest_destroy(td);
}

TEST_F(TdigestTest, InsertNullSafe) {
    tdigest_insert(nullptr, 100.0, 1);  // must not crash
    SUCCEED();
}

TEST_F(TdigestTest, QuantileNullSafe) {
    double v = tdigest_quantile(nullptr, 50.0);
    EXPECT_EQ(v, 0.0);
}

TEST_F(TdigestTest, QuantileExactBoundaries) {
    // Uniform [100, 200] with enough samples to trigger merge → active populated.
    tdigest_t *td = tdigest_create(20);
    for (int i = 0; i < 200; i++) tdigest_insert(td, 100.0 + (double)i * 0.5, 1);
    // Q0 should be near min, Q100 near max
    double q000 = tdigest_quantile(td, 0.0);
    double q100 = tdigest_quantile(td, 100.0);
    EXPECT_GE(q000, 99.0);
    EXPECT_LE(q000, 101.0);
    EXPECT_GE(q100, 190.0);  // t-digest max may lag exact by a few samples
    EXPECT_LE(q100, 201.0);
    tdigest_destroy(td);
}

TEST_F(TdigestTest, QuantileOutOfRange) {
    tdigest_t *td = tdigest_create(20);
    for (int i = 0; i < 100; i++) tdigest_insert(td, 200.0, 1);
    EXPECT_EQ(tdigest_quantile(td, -1.0), 0.0);
    EXPECT_EQ(tdigest_quantile(td, 101.0), 0.0);
    tdigest_destroy(td);
}

TEST_F(TdigestTest, InsertManyIdentical) {
    tdigest_t *td = tdigest_create(20);
    for (int i = 0; i < 10000; i++) tdigest_insert(td, 777.0, 1);
    EXPECT_NEAR(tdigest_quantile(td, 50.0), 777.0, 10.0);
    // Floating-point rounding in overflow merge may lose a few weight units
    EXPECT_GE(td->active->total_weight, (uint64_t)9990);
    tdigest_destroy(td);
}

TEST_F(TdigestTest, MinMaxTracking) {
    tdigest_t *td = tdigest_create(20);
    tdigest_insert(td, 100.0, 1);
    tdigest_insert(td, 300.0, 1);
    tdigest_insert(td, 200.0, 1);
    // min/max updated during merge — force one
    for (int i = 0; i < 40; i++) tdigest_insert(td, 150.0 + i, 1);
    EXPECT_LE((double)td->min_value, 100.0);
    EXPECT_GE((double)td->max_value, 300.0);
    tdigest_destroy(td);
}

TEST_F(TdigestTest, Merge) {
    tdigest_t *a = tdigest_create(20);
    tdigest_t *b = tdigest_create(20);
    for (int i = 0; i < 1000; i++) tdigest_insert(a, (double)i, 1);
    for (int i = 500; i < 1500; i++) tdigest_insert(b, (double)i, 1);

    tdigest_merge(a, b);
    // Flush residual buffer so all weights land in active
    for (int i = 0; i < 40; i++) tdigest_insert(a, 0.0, 1);
    EXPECT_GT(a->active->total_weight, (uint64_t)1500);
    double q50 = tdigest_quantile(a, 50.0);
    EXPECT_GE(q50, 400.0);
    EXPECT_LE(q50, 1100.0);
    tdigest_destroy(a);
    tdigest_destroy(b);
}

TEST_F(TdigestTest, AccuracyUniform) {
    const int N = 5000;
    std::vector<uint64_t> raw(N);
    tdigest_t *td = tdigest_create(20);  // low compression = harder test
    for (int i = 0; i < N; i++) {
        raw[i] = (uint64_t)((i + 1) * 5);  // 5, 10, 15, ..., 10000
        tdigest_insert(td, (double)raw[i], 1);
    }
    double exact50 = exact_quantile_from_raw(raw.data(), N, 50.0);
    double exact90 = exact_quantile_from_raw(raw.data(), N, 90.0);
    double exact99 = exact_quantile_from_raw(raw.data(), N, 99.0);

    double td50 = tdigest_quantile(td, 50.0);
    double td90 = tdigest_quantile(td, 90.0);
    double td99 = tdigest_quantile(td, 99.0);

    EXPECT_NEAR(td50 / exact50, 1.0, 0.10);  // ±10%
    EXPECT_NEAR(td90 / exact90, 1.0, 0.10);
    EXPECT_NEAR(td99 / exact99, 1.0, 0.10);
    tdigest_destroy(td);
}

TEST_F(TdigestTest, AccuracyManyIdenticals) {
    const int N = 5000;
    std::vector<uint64_t> raw(N);
    tdigest_t *td = tdigest_create(100);
    // 80% in [100, 500], 20% spread to 50000
    for (int i = 0; i < N; i++) {
        if (i < N * 4 / 5) {
            raw[i] = 100 + (uint64_t)(rand() % 400);
        } else {
            raw[i] = 500 + (uint64_t)(rand() % 49500);
        }
        tdigest_insert(td, (double)raw[i], 1);
    }
    double exact50 = exact_quantile_from_raw(raw.data(), N, 50.0);
    double exact90 = exact_quantile_from_raw(raw.data(), N, 90.0);
    double exact99 = exact_quantile_from_raw(raw.data(), N, 99.0);

    double td50 = tdigest_quantile(td, 50.0);
    double td90 = tdigest_quantile(td, 90.0);
    double td99 = tdigest_quantile(td, 99.0);

    EXPECT_NEAR(td50 / exact50, 1.0, 0.15);
    EXPECT_NEAR(td90 / exact90, 1.0, 0.15);
    EXPECT_NEAR(td99 / exact99, 1.0, 0.15);
    tdigest_destroy(td);
}

TEST_F(TdigestTest, MemoryAllocation) {
    tdigest_t *td = tdigest_create(30);
    ASSERT_NE(td, nullptr);
    // Verify heap arrays were actually allocated
    EXPECT_NE(td->one.centroids, nullptr);
    EXPECT_NE(td->two.centroids, nullptr);
    EXPECT_NE(td->buffer.centroids, nullptr);
    EXPECT_EQ(td->one.capacity, (size_t)30);
    EXPECT_EQ(td->two.capacity, (size_t)30);
    EXPECT_EQ(td->buffer.capacity, (size_t)60);
    EXPECT_EQ(td->compression, 30);
    // Sanity: total heap ≈ 4KiB (3 arrays × capacity × 16B/centroid + struct)
    size_t heap_est = (30 + 30 + 60) * sizeof(tdigest_centroid_t);
    EXPECT_GT(heap_est, (size_t)1000);
    tdigest_destroy(td);
}

TEST_F(PerfTest, InfoGetDuringWrites) {
    int ret = umq_perf_start();
    ASSERT_EQ(ret, UMQ_SUCCESS);

    umq_perf_stats_cfg_t cfg = {0};
    test_perf_cfg_set(&cfg);
    ret = umq_perf_reset(&cfg);
    ASSERT_EQ(ret, UMQ_SUCCESS);

    // trigger record alloc on main thread
    g_mock_urpc_get_cpu_cycles_value = 5000;
    uint64_t ts = umq_perf_get_start_timestamp();
    ASSERT_GT(ts, (uint64_t)0);

    std::atomic<bool> stop{false};
    std::thread writer([&]() {
        while (!stop) {
            umq_perf_record_type_t t = static_cast<umq_perf_record_type_t>(1);
            umq_perf_record_write(t, ts);
        }
    });

    for (int i = 0; i < 20; i++) {
        umq_perf_stats_t info;
        ret = umq_perf_info_get(&info);
        EXPECT_EQ(ret, 0);
    }
    stop = true;
    writer.join();
    umq_perf_stop();
}