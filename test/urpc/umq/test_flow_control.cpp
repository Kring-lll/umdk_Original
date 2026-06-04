/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: umq flow control test
 */

#define UMQ_UB_FC_DEDUP_UT

#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "umq_ub_flow_control.h"
#include "umq_ub_private.h"
#include "umq_ub_imm_data.h"

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

class UmqFcDedupTest : public ::testing::Test {
  public:
    void SetUp() override
    {
        memset(&queue, 0, sizeof(ub_queue_t));
        memset(&fc, 0, sizeof(ub_flow_control_t));
        queue.flow_control = fc;
    }

    void TearDown() override
    {
        GlobalMockObject::verify();
    }

    static void SetUpTestCase()
    {
    }

    static void TearDownTestCase()
    {
    }

    ub_queue_t queue;
    ub_flow_control_t fc;
};

TEST_F(UmqFcDedupTest, test_req_first_packet_not_duplicate)
{
    umq_ub_imm_t imm = {0};
    imm.bs.type = IMM_TYPE_FC_CREDIT_REQ;
    imm.flow_control.seq = 1;

    bool ret = umq_ub_fc_msg_dedup_check_ut(&queue, imm);
    EXPECT_EQ(ret, false);
    EXPECT_EQ(queue.flow_control.fc_remote_req_first, true);
    EXPECT_EQ(queue.flow_control.fc_remote_req_seq, 1);
}

TEST_F(UmqFcDedupTest, test_req_new_sequence_not_duplicate)
{
    umq_ub_imm_t imm = {0};
    imm.bs.type = IMM_TYPE_FC_CREDIT_REQ;
    imm.flow_control.seq = 1;
    
    queue.flow_control.fc_remote_req_first = true;
    queue.flow_control.fc_remote_req_seq = 1;

    imm.flow_control.seq = 2;
    bool ret = umq_ub_fc_msg_dedup_check_ut(&queue, imm);
    EXPECT_EQ(ret, false);
    EXPECT_EQ(queue.flow_control.fc_remote_req_seq, 2);
}

TEST_F(UmqFcDedupTest, test_req_duplicate_packet)
{
    umq_ub_imm_t imm = {0};
    imm.bs.type = IMM_TYPE_FC_CREDIT_REQ;
    imm.flow_control.seq = 1;
    
    queue.flow_control.fc_remote_req_first = true;
    queue.flow_control.fc_remote_req_seq = 1;

    bool ret = umq_ub_fc_msg_dedup_check_ut(&queue, imm);
    EXPECT_EQ(ret, true);
    EXPECT_EQ(queue.flow_control.fc_remote_req_seq, 1);
}

TEST_F(UmqFcDedupTest, test_req_wrap_around_not_duplicate)
{
    umq_ub_imm_t imm = {0};
    imm.bs.type = IMM_TYPE_FC_CREDIT_REQ;
    
    queue.flow_control.fc_remote_req_first = true;
    queue.flow_control.fc_remote_req_seq = 255;

    imm.flow_control.seq = 1;
    bool ret = umq_ub_fc_msg_dedup_check_ut(&queue, imm);
    EXPECT_EQ(ret, false);
    EXPECT_EQ(queue.flow_control.fc_remote_req_seq, 1);
}

TEST_F(UmqFcDedupTest, test_return_req_first_packet_not_duplicate)
{
    umq_ub_imm_t imm = {0};
    imm.bs.type = IMM_TYPE_FC_CREDIT_RETURN_REQ;
    imm.flow_control.seq = 1;

    bool ret = umq_ub_fc_msg_dedup_check_ut(&queue, imm);
    EXPECT_EQ(ret, false);
    EXPECT_EQ(queue.flow_control.fc_remote_req_first, true);
    EXPECT_EQ(queue.flow_control.fc_remote_req_seq, 1);
}

TEST_F(UmqFcDedupTest, test_return_req_duplicate_packet)
{
    umq_ub_imm_t imm = {0};
    imm.bs.type = IMM_TYPE_FC_CREDIT_RETURN_REQ;
    imm.flow_control.seq = 100;
    
    queue.flow_control.fc_remote_req_first = true;
    queue.flow_control.fc_remote_req_seq = 100;

    bool ret = umq_ub_fc_msg_dedup_check_ut(&queue, imm);
    EXPECT_EQ(ret, true);
}

TEST_F(UmqFcDedupTest, test_rep_normal_packet)
{
    umq_ub_imm_t imm = {0};
    imm.bs.type = IMM_TYPE_FC_CREDIT_REP;
    imm.flow_control.seq = 5;
    
    queue.flow_control.fc_local_req_seq = 5;

    bool ret = umq_ub_fc_msg_dedup_check_ut(&queue, imm);
    EXPECT_EQ(ret, false);
}

TEST_F(UmqFcDedupTest, test_rep_duplicate_packet)
{
    umq_ub_imm_t imm = {0};
    imm.bs.type = IMM_TYPE_FC_CREDIT_REP;
    imm.flow_control.seq = 3;
    
    queue.flow_control.fc_local_req_seq = 5;

    bool ret = umq_ub_fc_msg_dedup_check_ut(&queue, imm);
    EXPECT_EQ(ret, true);
}

TEST_F(UmqFcDedupTest, test_rep_seq_not_match)
{
    umq_ub_imm_t imm = {0};
    imm.bs.type = IMM_TYPE_FC_CREDIT_REP;
    imm.flow_control.seq = 10;
    
    queue.flow_control.fc_local_req_seq = 20;

    bool ret = umq_ub_fc_msg_dedup_check_ut(&queue, imm);
    EXPECT_EQ(ret, true);
}

TEST_F(UmqFcDedupTest, test_return_ack_normal_packet)
{
    umq_ub_imm_t imm = {0};
    imm.bs.type = IMM_TYPE_FC_CREDIT_RETURN_ACK;
    imm.flow_control.seq = 50;
    
    queue.flow_control.fc_local_req_seq = 50;

    bool ret = umq_ub_fc_msg_dedup_check_ut(&queue, imm);
    EXPECT_EQ(ret, false);
}

TEST_F(UmqFcDedupTest, test_return_ack_duplicate_packet)
{
    umq_ub_imm_t imm = {0};
    imm.bs.type = IMM_TYPE_FC_CREDIT_RETURN_ACK;
    imm.flow_control.seq = 40;
    
    queue.flow_control.fc_local_req_seq = 50;

    bool ret = umq_ub_fc_msg_dedup_check_ut(&queue, imm);
    EXPECT_EQ(ret, true);
}

TEST_F(UmqFcDedupTest, test_other_type_not_dedup)
{
    umq_ub_imm_t imm = {0};
    imm.bs.type = IMM_TYPE_USER;

    bool ret = umq_ub_fc_msg_dedup_check_ut(&queue, imm);
    EXPECT_EQ(ret, false);
}

TEST_F(UmqFcDedupTest, test_fc_init_type_not_dedup)
{
    umq_ub_imm_t imm = {0};
    imm.bs.type = IMM_TYPE_FC_CREDIT_INIT;

    bool ret = umq_ub_fc_msg_dedup_check_ut(&queue, imm);
    EXPECT_EQ(ret, false);
}

TEST_F(UmqFcDedupTest, test_check_and_store_req_first_packet)
{
    umq_ub_imm_t imm = {0};
    imm.bs.type = IMM_TYPE_FC_CREDIT_REQ;
    imm.flow_control.seq = 10;

    bool ret = umq_ub_fc_msg_dedup_check_and_store_ut(&queue, imm);
    EXPECT_EQ(ret, false);
    EXPECT_EQ(queue.flow_control.fc_remote_req_first, true);
    EXPECT_EQ(queue.flow_control.fc_remote_req_seq, 10);
}

TEST_F(UmqFcDedupTest, test_check_and_store_req_duplicate)
{
    umq_ub_imm_t imm = {0};
    imm.bs.type = IMM_TYPE_FC_CREDIT_REQ;
    imm.flow_control.seq = 10;
    
    queue.flow_control.fc_remote_req_first = true;
    queue.flow_control.fc_remote_req_seq = 10;

    bool ret = umq_ub_fc_msg_dedup_check_and_store_ut(&queue, imm);
    EXPECT_EQ(ret, true);
}

TEST_F(UmqFcDedupTest, test_check_and_store_rep_normal)
{
    umq_ub_imm_t imm = {0};
    imm.bs.type = IMM_TYPE_FC_CREDIT_REP;
    imm.flow_control.seq = 100;
    
    queue.flow_control.fc_local_req_seq = 100;

    bool ret = umq_ub_fc_msg_dedup_check_and_store_ut(&queue, imm);
    EXPECT_EQ(ret, false);
}

TEST_F(UmqFcDedupTest, test_check_and_store_rep_duplicate)
{
    umq_ub_imm_t imm = {0};
    imm.bs.type = IMM_TYPE_FC_CREDIT_REP;
    imm.flow_control.seq = 50;
    
    queue.flow_control.fc_local_req_seq = 100;

    bool ret = umq_ub_fc_msg_dedup_check_and_store_ut(&queue, imm);
    EXPECT_EQ(ret, true);
}

TEST_F(UmqFcDedupTest, test_check_and_store_return_req)
{
    umq_ub_imm_t imm = {0};
    imm.bs.type = IMM_TYPE_FC_CREDIT_RETURN_REQ;
    imm.flow_control.seq = 30;

    bool ret = umq_ub_fc_msg_dedup_check_and_store_ut(&queue, imm);
    EXPECT_EQ(ret, false);
    EXPECT_EQ(queue.flow_control.fc_remote_req_seq, 30);
}

TEST_F(UmqFcDedupTest, test_check_and_store_return_ack_normal)
{
    umq_ub_imm_t imm = {0};
    imm.bs.type = IMM_TYPE_FC_CREDIT_RETURN_ACK;
    imm.flow_control.seq = 200;
    
    queue.flow_control.fc_local_req_seq = 200;

    bool ret = umq_ub_fc_msg_dedup_check_and_store_ut(&queue, imm);
    EXPECT_EQ(ret, false);
}

TEST_F(UmqFcDedupTest, test_check_and_store_other_type)
{
    umq_ub_imm_t imm = {0};
    imm.bs.type = IMM_TYPE_NOTIFY;

    bool ret = umq_ub_fc_msg_dedup_check_and_store_ut(&queue, imm);
    EXPECT_EQ(ret, false);
}