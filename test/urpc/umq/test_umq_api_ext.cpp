/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: umq_api extended unit test for coverage
 */

#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "umq_api.h"
#include "umq_dfx_api.h"
#include "umq_qbuf_pool.h"
#include "umq_ub_private.h"
#include "urma_api.h"

class UmqApiExtTest : public ::testing::Test {
public:
    void SetUp() override {}
    void TearDown() override { GlobalMockObject::verify(); }
};

TEST_F(UmqApiExtTest, test_umq_dev_add_not_inited)
{
    umq_trans_info_t trans_info; memset(&trans_info, 0, sizeof(trans_info));
    trans_info.trans_mode = UMQ_TRANS_MODE_UB;
    ASSERT_NE(umq_dev_add(&trans_info), 0);
}

TEST_F(UmqApiExtTest, test_umq_dev_add_null) { ASSERT_NE(umq_dev_add(NULL), 0); }

TEST_F(UmqApiExtTest, test_umq_dev_add_invalid_trans_mode)
{
    umq_trans_info_t trans_info; memset(&trans_info, 0, sizeof(trans_info));
    trans_info.trans_mode = UMQ_TRANS_MODE_MAX;
    ASSERT_NE(umq_dev_add(&trans_info), 0);
}

TEST_F(UmqApiExtTest, test_mutex_ops_register_null) { ASSERT_NE(umq_external_mutex_lock_ops_register(NULL), 0); }
TEST_F(UmqApiExtTest, test_rwlock_ops_register_null) { ASSERT_NE(umq_external_rwlock_ops_register(NULL), 0); }

TEST_F(UmqApiExtTest, test_umq_buf_split_null_head) { ASSERT_NE(umq_buf_split(NULL, (umq_buf_t*)1), 0); }
TEST_F(UmqApiExtTest, test_umq_buf_split_null_node) { ASSERT_NE(umq_buf_split((umq_buf_t*)1, NULL), 0); }
TEST_F(UmqApiExtTest, test_umq_state_set_invalid) { ASSERT_NE(umq_state_set(0, (umq_state_t)0), 0); }
TEST_F(UmqApiExtTest, test_umq_state_get_invalid) { ASSERT_EQ(umq_state_get(0), QUEUE_STATE_MAX); }

TEST_F(UmqApiExtTest, test_log_config_set_null) { ASSERT_NE(umq_log_config_set(NULL), 0); }
TEST_F(UmqApiExtTest, test_log_config_get_null) { ASSERT_NE(umq_log_config_get(NULL), 0); }

TEST_F(UmqApiExtTest, test_log_config_set_invalid_level)
{
    umq_log_config_t config = {0};
    config.log_flag = UMQ_LOG_FLAG_LEVEL;
    config.level = UMQ_LOG_LEVEL_MAX;
    ASSERT_NE(umq_log_config_set(&config), 0);
}

TEST_F(UmqApiExtTest, test_log_config_set_conflicting_flags)
{
    umq_log_config_t config = {0};
    config.log_flag = UMQ_LOG_FLAG_FUNC | UMQ_LOG_FLAG_RATE_LIMITED;
    config.func = NULL;
    config.rate_limited.interval_ms = 100;
    config.rate_limited.num = 5;
    umq_log_config_set(&config);
    
}

TEST_F(UmqApiExtTest, test_log_config_set_level_success)
{
    umq_log_config_t config = {0};
    config.log_flag = UMQ_LOG_FLAG_LEVEL;
    config.level = UMQ_LOG_LEVEL_INFO;
    ASSERT_EQ(umq_log_config_set(&config), 0);
}

TEST_F(UmqApiExtTest, test_umq_buf_alloc_not_inited) { ASSERT_EQ(umq_buf_alloc(1024, 1, (umq_trans_mode_t)0, NULL), nullptr); }
TEST_F(UmqApiExtTest, test_umq_buf_free_not_inited) { umq_buf_t q; memset(&q, 0, sizeof(q)); umq_buf_free(&q);  }
TEST_F(UmqApiExtTest, test_umq_buf_break_and_free_not_inited) { umq_buf_t q; memset(&q, 0, sizeof(q)); ASSERT_EQ(umq_buf_break_and_free(&q), nullptr); }
TEST_F(UmqApiExtTest, test_umq_buf_headroom_reset_not_inited) { umq_buf_t q; memset(&q, 0, sizeof(q)); ASSERT_NE(umq_buf_headroom_reset(&q, 256), 0); }
TEST_F(UmqApiExtTest, test_umq_buf_reset_not_inited) { umq_buf_t q; memset(&q, 0, sizeof(q)); ASSERT_NE(umq_buf_reset(&q), 0); }
TEST_F(UmqApiExtTest, test_umq_data_to_head_null) { ASSERT_EQ(umq_data_to_head(NULL), nullptr); }
TEST_F(UmqApiExtTest, test_umq_dequeue_not_inited) { ASSERT_EQ(umq_dequeue(0), nullptr); }
TEST_F(UmqApiExtTest, test_umq_enqueue_not_inited) { umq_buf_t q; memset(&q, 0, sizeof(q)); ASSERT_NE(umq_enqueue(0, &q, nullptr), 0); }
TEST_F(UmqApiExtTest, test_umq_notify_invalid) { umq_notify(0);  }
TEST_F(UmqApiExtTest, test_umq_rearm_interrupt_invalid) { ASSERT_NE(umq_rearm_interrupt(0, false, NULL), 0); }
TEST_F(UmqApiExtTest, test_umq_wait_interrupt_invalid) { ASSERT_NE(umq_wait_interrupt(0, 100, NULL), 0); }
TEST_F(UmqApiExtTest, test_umq_ack_interrupt_invalid) { umq_ack_interrupt(0, 1, NULL);  }

TEST_F(UmqApiExtTest, test_async_event_fd_get_not_inited)
{
    umq_trans_info_t t; memset(&t, 0, sizeof(t));
    ASSERT_NE(umq_async_event_fd_get(&t), 0);
}

TEST_F(UmqApiExtTest, test_async_event_get_not_inited)
{
    umq_trans_info_t t; memset(&t, 0, sizeof(t));
    umq_async_event_t e; memset(&e, 0, sizeof(e));
    ASSERT_NE(umq_get_async_event(&t, &e), 0);
}

TEST_F(UmqApiExtTest, test_get_route_list_not_inited)
{
    umq_route_key_t k; memset(&k, 0, sizeof(k));
    umq_route_list_t l; memset(&l, 0, sizeof(l));
    ASSERT_NE(umq_get_route_list(&k, UMQ_TRANS_MODE_UB, &l), 0);
}

TEST_F(UmqApiExtTest, test_dev_info_list_get_invalid_mode) { int n = 0; ASSERT_EQ(umq_dev_info_list_get(UMQ_TRANS_MODE_MAX, &n), nullptr); }
TEST_F(UmqApiExtTest, test_dev_info_list_get_null) { ASSERT_EQ(umq_dev_info_list_get(UMQ_TRANS_MODE_UB, NULL), nullptr); }
TEST_F(UmqApiExtTest, test_dev_info_list_free_null) { umq_dev_info_list_free(UMQ_TRANS_MODE_UB, NULL);  }
TEST_F(UmqApiExtTest, test_cfg_get_invalid) { umq_cfg_get_t c; memset(&c, 0, sizeof(c)); ASSERT_NE(umq_cfg_get(0, &c), 0); }

TEST_F(UmqApiExtTest, test_user_ctl)
{
    umq_user_ctl_in_t in; memset(&in, 0, sizeof(in));
    umq_user_ctl_out_t out; memset(&out, 0, sizeof(out));
    ASSERT_EQ(umq_user_ctl(0, &in, &out), UMQ_SUCCESS);
}

TEST_F(UmqApiExtTest, test_io_perf_callback_register_null)
{
    ASSERT_NE(umq_io_perf_callback_register(NULL), 0);
}

TEST_F(UmqApiExtTest, test_io_perf_process_no_callback)
{
    umq_buf_t qbuf = {0};
    umq_io_perf_process(UMQ_PERF_RECORD_ENQUEUE, &qbuf);
    
}
