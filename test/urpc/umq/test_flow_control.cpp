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
#include "umq_ub_private.h"
#include "urma_api.h"

#define TEST_QUEUE_RX_DEPTH 128
#define TEST_RX_CONSUMED_TABLE_SIZE 1024

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

TEST_F(UmqFlowControlTest, test_umq_ub_shared_credit_return_req_handle_normal)
{
    ub_queue_t queue = {0};
    queue.create_flag = 0;
    queue.rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx = {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    umq_ub_ctx_t dev_ctx = {0};
    queue.dev_ctx = &dev_ctx;
    dev_ctx.io_lock_free = true;
    uint64_t rx_consumed_table[TEST_RX_CONSUMED_TABLE_SIZE] = {0};
    dev_ctx.rx_consumed_jetty_table = rx_consumed_table;
    urma_jetty_t io_jetty = {0};
    io_jetty.jetty_id.id = 0;
    queue.jetty[UB_QUEUE_JETTY_IO] = &io_jetty;
    queue.bind_ctx = NULL;
    umq_flow_control_cfg_t cfg = {0};
    cfg.use_atomic_window = false;
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);

    uint64_t allocated_credit = 100;
    uint64_t consumed_credit = 20;
    queue.flow_control.stats_u64[ALLOCATED_SUCCESS] = allocated_credit;
    rx_consumed_table[io_jetty.jetty_id.id] = consumed_credit;

    umq_ub_flow_control_data_t flow_control_data = {0};
    flow_control_data.bs.window = 50;
    flow_control_data.bs.seq = 1;

    ret = umq_ub_shared_credit_return_req_handle(&queue, &flow_control_data);
    ASSERT_EQ(ret, UMQ_SUCCESS);
    ASSERT_EQ(queue.flow_control.stats_u64[ALLOCATED_SUCCESS], allocated_credit - 50);
    ASSERT_EQ(io_jfr_ctx.credit.stats_u16[CREDIT_POOL_IDLE], 50);

    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(UmqFlowControlTest, test_umq_ub_shared_credit_return_req_handle_allocated_less_than_consumed)
{
    ub_queue_t queue = {0};
    queue.create_flag = 0;
    queue.rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx = {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    umq_ub_ctx_t dev_ctx = {0};
    queue.dev_ctx = &dev_ctx;
    dev_ctx.io_lock_free = true;
    uint64_t rx_consumed_table[TEST_RX_CONSUMED_TABLE_SIZE] = {0};
    dev_ctx.rx_consumed_jetty_table = rx_consumed_table;
    urma_jetty_t io_jetty = {0};
    io_jetty.jetty_id.id = 0;
    queue.jetty[UB_QUEUE_JETTY_IO] = &io_jetty;
    queue.bind_ctx = NULL;
    umq_flow_control_cfg_t cfg = {0};
    cfg.use_atomic_window = false;
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);

    uint64_t allocated_credit = 10;
    uint64_t consumed_credit = 20;
    queue.flow_control.stats_u64[ALLOCATED_SUCCESS] = allocated_credit;
    rx_consumed_table[io_jetty.jetty_id.id] = consumed_credit;

    umq_ub_flow_control_data_t flow_control_data = {0};
    flow_control_data.bs.window = 50;
    flow_control_data.bs.seq = 1;

    ret = umq_ub_shared_credit_return_req_handle(&queue, &flow_control_data);
    ASSERT_EQ(ret, UMQ_SUCCESS);
    ASSERT_EQ(queue.flow_control.stats_u64[ALLOCATED_SUCCESS], allocated_credit);
    ASSERT_EQ(io_jfr_ctx.credit.stats_u16[CREDIT_POOL_IDLE], 0);

    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(UmqFlowControlTest, test_umq_ub_shared_credit_return_req_handle_return_exceeds_unconsumed)
{
    ub_queue_t queue = {0};
    queue.create_flag = 0;
    queue.rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx = {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    umq_ub_ctx_t dev_ctx = {0};
    queue.dev_ctx = &dev_ctx;
    dev_ctx.io_lock_free = true;
    uint64_t rx_consumed_table[TEST_RX_CONSUMED_TABLE_SIZE] = {0};
    dev_ctx.rx_consumed_jetty_table = rx_consumed_table;
    urma_jetty_t io_jetty = {0};
    io_jetty.jetty_id.id = 0;
    queue.jetty[UB_QUEUE_JETTY_IO] = &io_jetty;
    queue.bind_ctx = NULL;
    umq_flow_control_cfg_t cfg = {0};
    cfg.use_atomic_window = false;
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);

    uint64_t allocated_credit = 100;
    uint64_t consumed_credit = 90;
    queue.flow_control.stats_u64[ALLOCATED_SUCCESS] = allocated_credit;
    rx_consumed_table[io_jetty.jetty_id.id] = consumed_credit;

    umq_ub_flow_control_data_t flow_control_data = {0};
    flow_control_data.bs.window = 50;
    flow_control_data.bs.seq = 1;

    uint64_t unconsumed = allocated_credit - consumed_credit;
    ret = umq_ub_shared_credit_return_req_handle(&queue, &flow_control_data);
    ASSERT_EQ(ret, UMQ_SUCCESS);
    ASSERT_EQ(queue.flow_control.stats_u64[ALLOCATED_SUCCESS], allocated_credit - unconsumed);
    ASSERT_EQ(io_jfr_ctx.credit.stats_u16[CREDIT_POOL_IDLE], (uint16_t)unconsumed);

    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(UmqFlowControlTest, test_umq_ub_shared_credit_return_req_handle_return_zero)
{
    ub_queue_t queue = {0};
    queue.create_flag = 0;
    queue.rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx = {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    umq_ub_ctx_t dev_ctx = {0};
    queue.dev_ctx = &dev_ctx;
    dev_ctx.io_lock_free = true;
    uint64_t rx_consumed_table[TEST_RX_CONSUMED_TABLE_SIZE] = {0};
    dev_ctx.rx_consumed_jetty_table = rx_consumed_table;
    urma_jetty_t io_jetty = {0};
    io_jetty.jetty_id.id = 0;
    queue.jetty[UB_QUEUE_JETTY_IO] = &io_jetty;
    queue.bind_ctx = NULL;
    umq_flow_control_cfg_t cfg = {0};
    cfg.use_atomic_window = false;
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);

    uint64_t allocated_credit = 100;
    uint64_t consumed_credit = 20;
    queue.flow_control.stats_u64[ALLOCATED_SUCCESS] = allocated_credit;
    rx_consumed_table[io_jetty.jetty_id.id] = consumed_credit;

    umq_ub_flow_control_data_t flow_control_data = {0};
    flow_control_data.bs.window = 0;
    flow_control_data.bs.seq = 1;

    ret = umq_ub_shared_credit_return_req_handle(&queue, &flow_control_data);
    ASSERT_EQ(ret, UMQ_SUCCESS);
    ASSERT_EQ(queue.flow_control.stats_u64[ALLOCATED_SUCCESS], allocated_credit);
    ASSERT_EQ(io_jfr_ctx.credit.stats_u16[CREDIT_POOL_IDLE], 0);

    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(UmqFlowControlTest, test_umq_ub_shared_credit_return_req_handle_atomic_mode)
{
    ub_queue_t queue = {0};
    queue.create_flag = 0;
    queue.rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx = {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    umq_ub_ctx_t dev_ctx = {0};
    queue.dev_ctx = &dev_ctx;
    dev_ctx.io_lock_free = false;
    uint64_t rx_consumed_table[TEST_RX_CONSUMED_TABLE_SIZE] = {0};
    dev_ctx.rx_consumed_jetty_table = rx_consumed_table;
    urma_jetty_t io_jetty = {0};
    io_jetty.jetty_id.id = 0;
    queue.jetty[UB_QUEUE_JETTY_IO] = &io_jetty;
    queue.bind_ctx = NULL;
    umq_flow_control_cfg_t cfg = {0};
    cfg.use_atomic_window = true;
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);

    uint64_t allocated_credit = 100;
    uint64_t consumed_credit = 20;
    __atomic_store_n(&queue.flow_control.stats_u64[ALLOCATED_SUCCESS], allocated_credit, __ATOMIC_RELAXED);
    __atomic_store_n(&rx_consumed_table[io_jetty.jetty_id.id], consumed_credit, __ATOMIC_RELAXED);

    umq_ub_flow_control_data_t flow_control_data = {0};
    flow_control_data.bs.window = 30;
    flow_control_data.bs.seq = 2;

    ret = umq_ub_shared_credit_return_req_handle(&queue, &flow_control_data);
    ASSERT_EQ(ret, UMQ_SUCCESS);
    uint64_t remaining = __atomic_load_n(&queue.flow_control.stats_u64[ALLOCATED_SUCCESS], __ATOMIC_RELAXED);
    ASSERT_EQ(remaining, allocated_credit - 30);
    uint16_t idle = __atomic_load_n(&io_jfr_ctx.credit.stats_u16[CREDIT_POOL_IDLE], __ATOMIC_RELAXED);
    ASSERT_EQ(idle, 30);

    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(UmqFlowControlTest, test_umq_ub_shared_credit_return_req_handle_unconsumed_equals_return)
{
    ub_queue_t queue = {0};
    queue.create_flag = 0;
    queue.rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx = {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    umq_ub_ctx_t dev_ctx = {0};
    queue.dev_ctx = &dev_ctx;
    dev_ctx.io_lock_free = true;
    uint64_t rx_consumed_table[TEST_RX_CONSUMED_TABLE_SIZE] = {0};
    dev_ctx.rx_consumed_jetty_table = rx_consumed_table;
    urma_jetty_t io_jetty = {0};
    io_jetty.jetty_id.id = 0;
    queue.jetty[UB_QUEUE_JETTY_IO] = &io_jetty;
    queue.bind_ctx = NULL;
    umq_flow_control_cfg_t cfg = {0};
    cfg.use_atomic_window = false;
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);

    uint64_t allocated_credit = 100;
    uint64_t consumed_credit = 50;
    queue.flow_control.stats_u64[ALLOCATED_SUCCESS] = allocated_credit;
    rx_consumed_table[io_jetty.jetty_id.id] = consumed_credit;

    umq_ub_flow_control_data_t flow_control_data = {0};
    flow_control_data.bs.window = 50;
    flow_control_data.bs.seq = 1;

    ret = umq_ub_shared_credit_return_req_handle(&queue, &flow_control_data);
    ASSERT_EQ(ret, UMQ_SUCCESS);
    ASSERT_EQ(queue.flow_control.stats_u64[ALLOCATED_SUCCESS], allocated_credit - 50);
    ASSERT_EQ(io_jfr_ctx.credit.stats_u16[CREDIT_POOL_IDLE], 50);

    umq_ub_flow_control_uninit(&queue.flow_control);
}