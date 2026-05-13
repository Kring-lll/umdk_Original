/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: umq api test
 */

#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "umq_ub_flow_control.h"

#define TEST_QUEUE_RX_DEPTH 128

class UmqFlowControlTest : public ::testing::Test {
  public:
    // SetUP 在每一个 TEST_F 测试开始前执行一次
    void SetUp() override
    {
    }

    // TearDown 在每一个 TEST_F 测试完成后执行一次
    void TearDown() override
    {
        GlobalMockObject::verify();
    }

    // SetUpTestCase 在所有 TEST_F 测试开始前执行一次
    static void SetUpTestCase()
    {
    }

    // TearDownTestCase 在所有 TEST_F 测试完成后执行一次
    static void TearDownTestCase()
    {
    }
};

TEST_F(UmqFlowControlTest, test_umq_ub_flow_control_init_non_atomic)
{
    ub_queue_t queue = {0};
    queue.create_flag = 0;
    queue.flow_control.local_rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx =  {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    umq_flow_control_cfg_t cfg = {0};
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);
    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(UmqFlowControlTest, test_umq_ub_flow_control_init_atomic)
{
    ub_queue_t queue = {0};
    queue.create_flag = 0;
    queue.flow_control.local_rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx =  {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    umq_flow_control_cfg_t cfg = {0};
    cfg.use_atomic_window = true;
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);
    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(UmqFlowControlTest, test_umq_ub_shared_credit_recharge)
{
    ub_queue_t queue = {0};
    queue.create_flag = 0;
    queue.flow_control.local_rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx =  {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    umq_flow_control_cfg_t cfg = {0};
    cfg.use_atomic_window = true;
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);
    umq_ub_shared_credit_recharge(&queue, 1);
    ASSERT_EQ(io_jfr_ctx.credit.stats_u16[CREDIT_POOL_IDLE], 1);
    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(UmqFlowControlTest, test_umq_ub_idle_credit_flush)
{
    ub_queue_t queue = {0};
    queue.create_flag = 0;
    umq_ub_ctx_t dev_ctx;
    queue.dev_ctx = &dev_ctx;
    queue.flow_control.local_rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx =  {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    umq_flow_control_cfg_t cfg = {0};
    cfg.use_atomic_window = true;
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);
    io_jfr_ctx.credit.stats_u16[CREDIT_POOL_IDLE] = 1;
    umq_ub_idle_credit_flush(&queue, 1);
    ASSERT_EQ(io_jfr_ctx.credit.stats_u16[CREDIT_POOL_IDLE], 0);
    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(UmqFlowControlTest, test_umq_ub_idle_credit_flush_non_atomic)
{
    ub_queue_t queue = {0};
    queue.create_flag = 0;
    umq_ub_ctx_t dev_ctx;
    queue.dev_ctx = &dev_ctx;
    dev_ctx.flow_control.use_atomic_window = true;
    queue.flow_control.local_rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx =  {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    umq_flow_control_cfg_t cfg = {0};
    cfg.use_atomic_window = false;
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);
    io_jfr_ctx.credit.stats_u16[CREDIT_POOL_IDLE] = 1;
    umq_ub_idle_credit_flush(&queue, 1);
    ASSERT_EQ(io_jfr_ctx.credit.stats_u16[CREDIT_POOL_IDLE], 0);
    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(UmqFlowControlTest, test_umq_ub_rx_consumed_inc_atomic)
{
    volatile uint64_t var = 0;
    (void)umq_ub_rx_consumed_inc(true, &var, 1);
    ASSERT_EQ(var, 1);
}

TEST_F(UmqFlowControlTest, test_umq_ub_rx_consumed_inc_non_atomic)
{
    volatile uint64_t var = 0;
    (void)umq_ub_rx_consumed_inc(false, &var, 1);
    ASSERT_EQ(var, 1);
}

TEST_F(UmqFlowControlTest, test_umq_ub_rx_consumed_exchange_atomic)
{
    volatile uint64_t var = 10;
    uint64_t exchanged = umq_ub_rx_consumed_exchange(true, &var, 0);
    ASSERT_EQ(exchanged, 10);
    ASSERT_EQ(var, 0);
}

TEST_F(UmqFlowControlTest, test_umq_ub_rx_consumed_exchange_non_atomic)
{
    volatile uint64_t var = 20;
    uint64_t exchanged = umq_ub_rx_consumed_exchange(false, &var, 0);
    ASSERT_EQ(exchanged, 20);
    ASSERT_EQ(var, 0);
}

TEST_F(UmqFlowControlTest, test_umq_ub_fc_threashold_modify_basic)
{
    uint16_t threshold = 10;
    uint8_t ratio = 5;
    uint16_t modified = umq_ub_fc_threashold_modify(threshold, ratio);
    ASSERT_GT(modified, 0);
}

TEST_F(UmqFlowControlTest, test_umq_ub_fc_threashold_modify_zero_threshold)
{
    uint16_t threshold = 0;
    uint8_t ratio = 5;
    uint16_t modified = umq_ub_fc_threashold_modify(threshold, ratio);
    ASSERT_EQ(modified, 2);
}

TEST_F(UmqFlowControlTest, test_umq_ub_window_inc_enabled)
{
    ub_flow_control_t fc = {0};
    fc.enabled = true;
    fc.remote_rx_window = 0;
    fc.remote_rx_depth = TEST_QUEUE_RX_DEPTH;
    
    umq_flow_control_cfg_t cfg = {0};
    cfg.use_atomic_window = false;
    ub_queue_t queue = {0};
    jfr_ctx_t io_jfr_ctx = {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    
    int ret = umq_ub_flow_control_init(&fc, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);
    
    umq_ub_window_inc(&fc, 5);
    ASSERT_EQ(fc.remote_rx_window, 5);
    
    umq_ub_flow_control_uninit(&fc);
}

TEST_F(UmqFlowControlTest, test_umq_ub_window_inc_disabled)
{
    ub_flow_control_t fc = {0};
    fc.enabled = false;
    fc.remote_rx_window = 0;
    
    umq_ub_window_inc(&fc, 5);
    ASSERT_EQ(fc.remote_rx_window, 0);
}

TEST_F(UmqFlowControlTest, test_umq_ub_window_dec_enabled)
{
    ub_flow_control_t fc = {0};
    fc.enabled = true;
    fc.remote_rx_window = 10;
    fc.remote_rx_depth = TEST_QUEUE_RX_DEPTH;
    
    umq_flow_control_cfg_t cfg = {0};
    cfg.use_atomic_window = false;
    ub_queue_t queue = {0};
    jfr_ctx_t io_jfr_ctx = {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    
    int ret = umq_ub_flow_control_init(&fc, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);
    
    fc.remote_rx_window = 10;
    uint16_t consumed = umq_ub_window_dec(&fc, &queue, 3);
    ASSERT_EQ(consumed, 3);
    ASSERT_EQ(fc.remote_rx_window, 7);
    
    umq_ub_flow_control_uninit(&fc);
}

TEST_F(UmqFlowControlTest, test_umq_ub_window_dec_disabled)
{
    ub_flow_control_t fc = {0};
    fc.enabled = false;
    ub_queue_t queue = {0};
    
    uint16_t result = umq_ub_window_dec(&fc, &queue, 5);
    ASSERT_EQ(result, 5);
}

TEST_F(UmqFlowControlTest, test_umq_ub_permission_acquire_success)
{
    ub_flow_control_t fc = {0};
    fc.is_credit_applying = false;
    
    bool acquired = umq_ub_permission_acquire(&fc);
    ASSERT_TRUE(acquired);
    ASSERT_TRUE(fc.is_credit_applying);
    
    umq_ub_permission_release(&fc);
    ASSERT_FALSE(fc.is_credit_applying);
}

TEST_F(UmqFlowControlTest, test_umq_ub_permission_acquire_fail)
{
    ub_flow_control_t fc = {0};
    fc.is_credit_applying = true;
    
    bool acquired = umq_ub_permission_acquire(&fc);
    ASSERT_FALSE(acquired);
}

TEST_F(UmqFlowControlTest, test_umq_ub_flow_control_init_with_custom_config)
{
    ub_queue_t queue = {0};
    queue.create_flag = 0;
    queue.rx_depth = TEST_QUEUE_RX_DEPTH;
    queue.flow_control.local_rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx = {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    
    umq_flow_control_cfg_t cfg = {0};
    cfg.initial_credit = 10;
    cfg.max_credits_request = 50;
    cfg.credit_multiple = 2.0f;
    cfg.return_ratio = 3;
    cfg.min_reserved_credit = 5;
    cfg.timeout_ms = 1000;
    cfg.use_atomic_window = true;
    
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(queue.flow_control.initial_credit, 10);
    ASSERT_EQ(queue.flow_control.max_credits_request, 50);
    ASSERT_EQ(queue.flow_control.credit_multiple, 2.0f);
    
    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(UmqFlowControlTest, test_umq_ub_flow_control_init_invalid_config)
{
    ub_queue_t queue = {0};
    queue.create_flag = 0;
    queue.rx_depth = TEST_QUEUE_RX_DEPTH;
    queue.flow_control.local_rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx = {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    
    umq_flow_control_cfg_t cfg = {0};
    cfg.initial_credit = 0;
    cfg.max_credits_request = 1;
    cfg.credit_multiple = 0;
    cfg.return_ratio = 0;
    cfg.min_reserved_credit = 0;
    
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);
    
    ASSERT_EQ(queue.flow_control.initial_credit, TEST_QUEUE_RX_DEPTH >> 4);
    ASSERT_EQ(queue.flow_control.max_credits_request, 512);
    
    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(UmqFlowControlTest, test_umq_ub_flow_control_disabled)
{
    ub_flow_control_t fc = {0};
    ub_queue_t queue = {0};
    umq_flow_control_cfg_t cfg = {0};
    
    int ret = umq_ub_flow_control_init(&fc, &queue, 0, &cfg);
    ASSERT_EQ(ret, 0);
    ASSERT_FALSE(fc.enabled);
    
    umq_ub_flow_control_uninit(&fc);
}

TEST_F(UmqFlowControlTest, test_umq_ub_shared_credit_recharge_multiple_times)
{
    ub_queue_t queue = {0};
    queue.create_flag = 0;
    queue.flow_control.local_rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx = {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    
    umq_flow_control_cfg_t cfg = {0};
    cfg.use_atomic_window = true;
    
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);
    
    umq_ub_shared_credit_recharge(&queue, 5);
    ASSERT_EQ(io_jfr_ctx.credit.stats_u16[CREDIT_POOL_IDLE], 5);
    
    umq_ub_shared_credit_recharge(&queue, 3);
    ASSERT_EQ(io_jfr_ctx.credit.stats_u16[CREDIT_POOL_IDLE], 8);
    
    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(UmqFlowControlTest, test_umq_ub_shared_credit_recharge_zero)
{
    ub_queue_t queue = {0};
    queue.create_flag = 0;
    queue.flow_control.local_rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx = {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    
    umq_flow_control_cfg_t cfg = {0};
    cfg.use_atomic_window = true;
    
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);
    
    umq_ub_shared_credit_recharge(&queue, 0);
    ASSERT_EQ(io_jfr_ctx.credit.stats_u16[CREDIT_POOL_IDLE], 0);
    
    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(UmqFlowControlTest, test_umq_ub_idle_credit_flush_multiple)
{
    ub_queue_t queue = {0};
    queue.create_flag = 0;
    umq_ub_ctx_t dev_ctx = {0};
    queue.dev_ctx = &dev_ctx;
    queue.flow_control.local_rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx = {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    
    umq_flow_control_cfg_t cfg = {0};
    cfg.use_atomic_window = true;
    
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);
    
    io_jfr_ctx.credit.stats_u16[CREDIT_POOL_IDLE] = 10;
    umq_ub_idle_credit_flush(&queue, 3);
    ASSERT_EQ(io_jfr_ctx.credit.stats_u16[CREDIT_POOL_IDLE], 7);
    
    umq_ub_idle_credit_flush(&queue, 2);
    ASSERT_EQ(io_jfr_ctx.credit.stats_u16[CREDIT_POOL_IDLE], 5);
    
    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(UmqFlowControlTest, test_umq_ub_idle_credit_flush_exceed_available)
{
    ub_queue_t queue = {0};
    queue.create_flag = 0;
    umq_ub_ctx_t dev_ctx = {0};
    queue.dev_ctx = &dev_ctx;
    queue.flow_control.local_rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx = {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    
    umq_flow_control_cfg_t cfg = {0};
    cfg.use_atomic_window = true;
    
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);
    
    io_jfr_ctx.credit.stats_u16[CREDIT_POOL_IDLE] = 2;
    umq_ub_idle_credit_flush(&queue, 5);
    ASSERT_EQ(io_jfr_ctx.credit.stats_u16[CREDIT_POOL_IDLE], 0);
    
    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(UmqFlowControlTest, test_umq_ub_idle_credit_flush_zero_count)
{
    ub_queue_t queue = {0};
    queue.create_flag = 0;
    umq_ub_ctx_t dev_ctx = {0};
    queue.dev_ctx = &dev_ctx;
    queue.flow_control.local_rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx = {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    
    umq_flow_control_cfg_t cfg = {0};
    cfg.use_atomic_window = true;
    
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);
    
    io_jfr_ctx.credit.stats_u16[CREDIT_POOL_IDLE] = 10;
    umq_ub_idle_credit_flush(&queue, 0);
    ASSERT_EQ(io_jfr_ctx.credit.stats_u16[CREDIT_POOL_IDLE], 10);
    
    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(UmqFlowControlTest, test_umq_ub_rx_consumed_inc_multiple)
{
    volatile uint64_t var = 0;
    
    umq_ub_rx_consumed_inc(true, &var, 5);
    ASSERT_EQ(var, 5);
    
    umq_ub_rx_consumed_inc(true, &var, 3);
    ASSERT_EQ(var, 8);
    
    umq_ub_rx_consumed_inc(true, &var, 10);
    ASSERT_EQ(var, 18);
}

TEST_F(UmqFlowControlTest, test_umq_ub_rx_consumed_exchange_with_value)
{
    volatile uint64_t var = 100;
    
    uint64_t exchanged = umq_ub_rx_consumed_exchange(true, &var, 0);
    ASSERT_EQ(exchanged, 100);
    ASSERT_EQ(var, 0);
}

TEST_F(UmqFlowControlTest, test_umq_ub_fc_threashold_modify_various_ratios)
{
    uint16_t threshold = 20;
    
    uint16_t modified1 = umq_ub_fc_threashold_modify(threshold, 1);
    ASSERT_EQ(modified1, 2);
    
    uint16_t modified3 = umq_ub_fc_threashold_modify(threshold, 3);
    ASSERT_EQ(modified3, 6);
    
    uint16_t modified5 = umq_ub_fc_threashold_modify(threshold, 5);
    ASSERT_EQ(modified5, 10);
    
    uint16_t modified7 = umq_ub_fc_threashold_modify(threshold, 7);
    ASSERT_EQ(modified7, 14);
}

TEST_F(UmqFlowControlTest, test_umq_ub_fc_threashold_modify_large_threshold)
{
    uint16_t threshold = 1000;
    uint8_t ratio = 5;
    
    uint16_t modified = umq_ub_fc_threashold_modify(threshold, ratio);
    ASSERT_GT(modified, 2);
    ASSERT_LE(modified, threshold);
}

TEST_F(UmqFlowControlTest, test_umq_ub_window_inc_zero)
{
    ub_flow_control_t fc = {0};
    fc.enabled = true;
    fc.remote_rx_window = 10;
    
    umq_ub_window_inc(&fc, 0);
    ASSERT_EQ(fc.remote_rx_window, 10);
}

TEST_F(UmqFlowControlTest, test_umq_ub_window_dec_zero)
{
    ub_flow_control_t fc = {0};
    fc.enabled = true;
    fc.remote_rx_window = 10;
    ub_queue_t queue = {0};
    
    uint16_t result = umq_ub_window_dec(&fc, &queue, 0);
    ASSERT_EQ(result, 0);
    ASSERT_EQ(fc.remote_rx_window, 10);
}

TEST_F(UmqFlowControlTest, test_umq_ub_window_dec_exceed_available)
{
    ub_flow_control_t fc = {0};
    fc.enabled = true;
    fc.remote_rx_window = 5;
    fc.remote_rx_depth = TEST_QUEUE_RX_DEPTH;
    
    umq_flow_control_cfg_t cfg = {0};
    cfg.use_atomic_window = false;
    ub_queue_t queue = {0};
    jfr_ctx_t io_jfr_ctx = {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    
    int ret = umq_ub_flow_control_init(&fc, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);
    
    fc.remote_rx_window = 5;
    uint16_t consumed = umq_ub_window_dec(&fc, &queue, 10);
    ASSERT_EQ(consumed, 5);
    ASSERT_EQ(fc.remote_rx_window, 0);
    
    umq_ub_flow_control_uninit(&fc);
}

TEST_F(UmqFlowControlTest, test_umq_ub_permission_acquire_release_cycle)
{
    ub_flow_control_t fc = {0};
    fc.is_credit_applying = false;
    
    bool acquired1 = umq_ub_permission_acquire(&fc);
    ASSERT_TRUE(acquired1);
    
    bool acquired2 = umq_ub_permission_acquire(&fc);
    ASSERT_FALSE(acquired2);
    
    umq_ub_permission_release(&fc);
    
    bool acquired3 = umq_ub_permission_acquire(&fc);
    ASSERT_TRUE(acquired3);
    
    umq_ub_permission_release(&fc);
}

TEST_F(UmqFlowControlTest, test_umq_ub_flow_control_init_boundary_initial_credit)
{
    ub_queue_t queue = {0};
    queue.create_flag = 0;
    queue.rx_depth = TEST_QUEUE_RX_DEPTH;
    queue.flow_control.local_rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx = {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    
    umq_flow_control_cfg_t cfg = {0};
    cfg.initial_credit = TEST_QUEUE_RX_DEPTH + 1;
    cfg.use_atomic_window = true;
    
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);
    ASSERT_LE(queue.flow_control.initial_credit, TEST_QUEUE_RX_DEPTH);
    
    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(UmqFlowControlTest, test_umq_ub_flow_control_init_boundary_max_credits)
{
    ub_queue_t queue = {0};
    queue.create_flag = 0;
    queue.rx_depth = TEST_QUEUE_RX_DEPTH;
    queue.flow_control.local_rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx = {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    
    umq_flow_control_cfg_t cfg = {0};
    cfg.max_credits_request = TEST_QUEUE_RX_DEPTH + 100;
    cfg.use_atomic_window = true;
    
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);
    ASSERT_LE(queue.flow_control.max_credits_request, TEST_QUEUE_RX_DEPTH);
    
    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(UmqFlowControlTest, test_umq_ub_flow_control_init_small_rx_depth)
{
    ub_queue_t queue = {0};
    queue.create_flag = 0;
    queue.rx_depth = 4;
    queue.flow_control.local_rx_depth = 4;
    jfr_ctx_t io_jfr_ctx = {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    
    umq_flow_control_cfg_t cfg = {0};
    cfg.use_atomic_window = true;
    
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(queue.flow_control.initial_credit, 1);
    
    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(UmqFlowControlTest, test_umq_ub_shared_credit_recharge_disabled)
{
    ub_queue_t queue = {0};
    queue.create_flag = 0;
    queue.flow_control.enabled = false;
    jfr_ctx_t io_jfr_ctx = {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    
    umq_flow_control_cfg_t cfg = {0};
    cfg.use_atomic_window = true;
    
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, 0, &cfg);
    ASSERT_EQ(ret, 0);
    
    umq_ub_shared_credit_recharge(&queue, 10);
    ASSERT_EQ(io_jfr_ctx.credit.stats_u16[CREDIT_POOL_IDLE], 0);
    
    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(UmqFlowControlTest, test_umq_ub_window_inc_with_initialized_fc)
{
    ub_queue_t queue = {0};
    queue.create_flag = 0;
    queue.rx_depth = TEST_QUEUE_RX_DEPTH;
    queue.flow_control.local_rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx = {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    
    umq_flow_control_cfg_t cfg = {0};
    cfg.initial_credit = 10;
    cfg.use_atomic_window = false;
    
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);
    
    umq_ub_window_inc(&queue.flow_control, 5);
    ASSERT_EQ(queue.flow_control.remote_rx_window, 5);
    
    umq_ub_window_inc(&queue.flow_control, 3);
    ASSERT_EQ(queue.flow_control.remote_rx_window, 8);
    
    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(UmqFlowControlTest, test_umq_ub_window_dec_exact_amount)
{
    ub_queue_t queue = {0};
    queue.create_flag = 0;
    queue.rx_depth = TEST_QUEUE_RX_DEPTH;
    queue.flow_control.local_rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx = {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    
    umq_flow_control_cfg_t cfg = {0};
    cfg.use_atomic_window = false;
    
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);
    
    queue.flow_control.remote_rx_window = 10;
    
    uint16_t consumed1 = umq_ub_window_dec(&queue.flow_control, &queue, 5);
    ASSERT_EQ(consumed1, 5);
    ASSERT_EQ(queue.flow_control.remote_rx_window, 5);
    
    uint16_t consumed2 = umq_ub_window_dec(&queue.flow_control, &queue, 5);
    ASSERT_EQ(consumed2, 5);
    ASSERT_EQ(queue.flow_control.remote_rx_window, 0);
    
    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(UmqFlowControlTest, test_umq_ub_fc_threashold_modify_various_thresholds)
{
    uint8_t ratio = 5;
    
    uint16_t mod1 = umq_ub_fc_threashold_modify(1, ratio);
    ASSERT_EQ(mod1, 2);
    
    uint16_t mod2 = umq_ub_fc_threashold_modify(10, ratio);
    ASSERT_EQ(mod2, 5);
    
    uint16_t mod3 = umq_ub_fc_threashold_modify(100, ratio);
    ASSERT_EQ(mod3, 50);
}

TEST_F(UmqFlowControlTest, test_umq_ub_flow_control_uninit_disabled)
{
    ub_flow_control_t fc = {0};
    fc.enabled = false;
    
    umq_ub_flow_control_uninit(&fc);
}

TEST_F(UmqFlowControlTest, test_umq_ub_rx_consumed_inc_large_count)
{
    volatile uint64_t var = 0;
    
    umq_ub_rx_consumed_inc(true, &var, 1000);
    ASSERT_EQ(var, 1000);
    
    umq_ub_rx_consumed_inc(false, &var, 2000);
    ASSERT_EQ(var, 3000);
}

TEST_F(UmqFlowControlTest, test_umq_ub_rx_consumed_exchange_atomic_zero)
{
    volatile uint64_t var = 0;
    
    uint64_t exchanged = umq_ub_rx_consumed_exchange(true, &var, 0);
    ASSERT_EQ(exchanged, 0);
    ASSERT_EQ(var, 0);
}

TEST_F(UmqFlowControlTest, test_umq_ub_rx_consumed_exchange_non_atomic_zero)
{
    volatile uint64_t var = 0;
    
    uint64_t exchanged = umq_ub_rx_consumed_exchange(false, &var, 0);
    ASSERT_EQ(exchanged, 0);
    ASSERT_EQ(var, 0);
}

TEST_F(UmqFlowControlTest, test_umq_ub_flow_control_init_sub_umq)
{
    ub_queue_t queue = {0};
    queue.create_flag = UMQ_CREATE_FLAG_SUB_UMQ;
    queue.rx_depth = TEST_QUEUE_RX_DEPTH;
    queue.flow_control.local_rx_depth = TEST_QUEUE_RX_DEPTH;
    
    umq_flow_control_cfg_t cfg = {0};
    cfg.use_atomic_window = true;
    
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);
    
    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(UmqFlowControlTest, test_umq_ub_permission_release_multiple_times)
{
    ub_flow_control_t fc = {0};
    fc.is_credit_applying = true;
    
    umq_ub_permission_release(&fc);
    ASSERT_FALSE(fc.is_credit_applying);
    
    umq_ub_permission_release(&fc);
    ASSERT_FALSE(fc.is_credit_applying);
}

TEST_F(UmqFlowControlTest, test_umq_ub_flow_control_init_max_rx_depth)
{
    ub_queue_t queue = {0};
    queue.create_flag = 0;
    queue.rx_depth = 1024;
    queue.flow_control.local_rx_depth = 1024;
    jfr_ctx_t io_jfr_ctx = {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    
    umq_flow_control_cfg_t cfg = {0};
    cfg.use_atomic_window = true;
    
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);
    ASSERT_GT(queue.flow_control.initial_credit, 0);
    ASSERT_LE(queue.flow_control.initial_credit, 1024);
    
    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(UmqFlowControlTest, test_umq_ub_window_operations_atomic_vs_non_atomic)
{
    ub_queue_t queue1 = {0};
    queue1.create_flag = 0;
    queue1.rx_depth = TEST_QUEUE_RX_DEPTH;
    queue1.flow_control.local_rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx1 = {0};
    queue1.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx1;
    
    umq_flow_control_cfg_t cfg_atomic = {0};
    cfg_atomic.use_atomic_window = true;
    
    int ret1 = umq_ub_flow_control_init(&queue1.flow_control, &queue1, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg_atomic);
    ASSERT_EQ(ret1, 0);
    
    umq_ub_window_inc(&queue1.flow_control, 10);
    ASSERT_EQ(queue1.flow_control.remote_rx_window, 10);
    
    umq_ub_flow_control_uninit(&queue1.flow_control);
    
    ub_queue_t queue2 = {0};
    queue2.create_flag = 0;
    queue2.rx_depth = TEST_QUEUE_RX_DEPTH;
    queue2.flow_control.local_rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx2 = {0};
    queue2.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx2;
    
    umq_flow_control_cfg_t cfg_non_atomic = {0};
    cfg_non_atomic.use_atomic_window = false;
    
    int ret2 = umq_ub_flow_control_init(&queue2.flow_control, &queue2, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg_non_atomic);
    ASSERT_EQ(ret2, 0);
    
    umq_ub_window_inc(&queue2.flow_control, 10);
    ASSERT_EQ(queue2.flow_control.remote_rx_window, 10);
    
    umq_ub_flow_control_uninit(&queue2.flow_control);
}