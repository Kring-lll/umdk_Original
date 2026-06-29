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

TEST_F(UmqFlowControlTest, test_pending_queue_init_uninit)
{
    ub_credit_pending_queue_t pq;
    int ret = umq_ub_credit_pending_queue_init(&pq, 4);
    ASSERT_EQ(ret, UMQ_SUCCESS);
    ASSERT_EQ(pq.pending_count, 0);
    ASSERT_EQ(pq.pending_credit_threshold, 4);
    ASSERT_TRUE(urpc_list_is_empty(&pq.pending_list));
    umq_ub_credit_pending_queue_uninit(&pq);
}

TEST_F(UmqFlowControlTest, test_pending_req_enqueue_dequeue)
{
    ub_queue_t queue = {0};
    umq_ub_ctx_t dev_ctx = {0};
    dev_ctx.io_lock_free = true;
    queue.dev_ctx = &dev_ctx;
    queue.ref_cnt = 1;

    ub_credit_pending_queue_t pq;
    int ret = umq_ub_credit_pending_queue_init(&pq, 4);
    ASSERT_EQ(ret, UMQ_SUCCESS);

    ret = umq_ub_credit_pending_req_enqueue(&pq, &queue, 8, 1);
    ASSERT_EQ(ret, UMQ_SUCCESS);
    ASSERT_EQ(pq.pending_count, 1);

    ret = umq_ub_credit_pending_req_enqueue(&pq, &queue, 4, 2);
    ASSERT_EQ(ret, UMQ_SUCCESS);
    ASSERT_EQ(pq.pending_count, 2);

    umq_ub_credit_pending_req_remove_by_queue(&pq, &queue);
    ASSERT_EQ(pq.pending_count, 0);

    umq_ub_credit_pending_queue_uninit(&pq);
}

TEST_F(UmqFlowControlTest, test_pending_queue_threshold)
{
    ub_queue_t queue = {0};
    queue.create_flag = 0;
    queue.flow_control.local_rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx = {0};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    umq_ub_ctx_t dev_ctx = {0};
    dev_ctx.io_lock_free = true;
    queue.dev_ctx = &dev_ctx;
    queue.ref_cnt = 1;
    dev_ctx.feature = UMQ_FEATURE_ENABLE_FLOW_CONTROL;

    umq_flow_control_cfg_t cfg = {0};
    cfg.use_atomic_window = true;
    cfg.pending_credit_threshold = 8;
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);

    ub_credit_pool_t *pool = &io_jfr_ctx.credit;
    ASSERT_EQ(pool->pending_queue.pending_credit_threshold, 8);

    umq_ub_credit_pending_req_remove_by_queue(&pool->pending_queue, &queue);
    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(UmqFlowControlTest, test_pending_req_max_overflow)
{
    ub_queue_t queue = {0};
    umq_ub_ctx_t dev_ctx = {0};
    dev_ctx.io_lock_free = true;
    queue.dev_ctx = &dev_ctx;
    queue.ref_cnt = 1;

    ub_credit_pending_queue_t pq;
    int ret = umq_ub_credit_pending_queue_init(&pq, 4);
    ASSERT_EQ(ret, UMQ_SUCCESS);
    pq.max_pending = 2;

    ret = umq_ub_credit_pending_req_enqueue(&pq, &queue, 4, 1);
    ASSERT_EQ(ret, UMQ_SUCCESS);
    ret = umq_ub_credit_pending_req_enqueue(&pq, &queue, 4, 2);
    ASSERT_EQ(ret, UMQ_SUCCESS);
    ret = umq_ub_credit_pending_req_enqueue(&pq, &queue, 4, 3);
    ASSERT_NE(ret, UMQ_SUCCESS);

    umq_ub_credit_pending_req_remove_by_queue(&pq, &queue);
    umq_ub_credit_pending_queue_uninit(&pq);
}

TEST_F(UmqFlowControlTest, test_pending_req_remove_by_queue)
{
    ub_queue_t queue_a = {0};
    ub_queue_t queue_b = {0};
    umq_ub_ctx_t dev_ctx = {0};
    dev_ctx.io_lock_free = true;
    queue_a.dev_ctx = &dev_ctx;
    queue_a.ref_cnt = 1;
    queue_b.dev_ctx = &dev_ctx;
    queue_b.ref_cnt = 1;

    ub_credit_pending_queue_t pq;
    int ret = umq_ub_credit_pending_queue_init(&pq, 4);
    ASSERT_EQ(ret, UMQ_SUCCESS);

    ret = umq_ub_credit_pending_req_enqueue(&pq, &queue_a, 8, 1);
    ASSERT_EQ(ret, UMQ_SUCCESS);
    ret = umq_ub_credit_pending_req_enqueue(&pq, &queue_b, 4, 2);
    ASSERT_EQ(ret, UMQ_SUCCESS);
    ASSERT_EQ(pq.pending_count, 2);

    umq_ub_credit_pending_req_remove_by_queue(&pq, &queue_a);
    ASSERT_EQ(pq.pending_count, 1);

    umq_ub_credit_pending_req_remove_by_queue(&pq, &queue_b);
    ASSERT_EQ(pq.pending_count, 0);

    umq_ub_credit_pending_queue_uninit(&pq);
}

TEST_F(UmqFlowControlTest, test_pending_queue_replenish_accumulated)
{
    ub_credit_pending_queue_t pq;
    int ret = umq_ub_credit_pending_queue_init(&pq, 8);
    ASSERT_EQ(ret, UMQ_SUCCESS);

    __atomic_add_fetch(&pq.replenish_accumulated, 4, __ATOMIC_RELAXED);
    ASSERT_EQ(__atomic_load_n(&pq.replenish_accumulated, __ATOMIC_RELAXED), 4);

    __atomic_add_fetch(&pq.replenish_accumulated, 4, __ATOMIC_RELAXED);
    ASSERT_EQ(__atomic_load_n(&pq.replenish_accumulated, __ATOMIC_RELAXED), 8);

    umq_ub_credit_pending_queue_uninit(&pq);
}