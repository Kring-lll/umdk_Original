/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: umq ipc test
 */

#include <sys/mman.h>

#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"

#include "perf.h"
#include "umq_api.h"
#include "umq_qbuf_pool.h"
#include "urpc_util.h"

// --gtest_filter="*test_umq_create_success*"

class UmqIPCTest : public ::testing::Test {
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

TEST_F(UmqIPCTest, test_umq_init_failure)
{
    umq_init_cfg_t cfg;
    memset(&cfg, 0, sizeof(umq_init_cfg_t));
    int ret;

    ret = umq_init(nullptr);
    ASSERT_NE(ret, 0);

    cfg.trans_info_num = MAX_UMQ_TRANS_INFO_NUM + 1;
    ret = umq_init(&cfg);
    ASSERT_NE(ret, 0);
    cfg.trans_info_num = 0;

    cfg.transport_mode = UMQ_TM_UM;
    ret = umq_init(&cfg);
    ASSERT_NE(ret, 0);
    cfg.transport_mode = UMQ_TM_RC;

    cfg.tp_type = UMQ_TP_TYPE_UTP;
    ret = umq_init(&cfg);
    ASSERT_NE(ret, 0);
    cfg.tp_type = UMQ_TP_TYPE_RTP;

    cfg.headroom_size = UMQ_HEADROOM_SIZE_LIMIT + 1;
    ret = umq_init(&cfg);
    ASSERT_NE(ret, 0);
    cfg.headroom_size = 0;

    MOCKER(urpc_rand_seed_init).stubs().will(returnValue(-1));
    cfg.feature = UMQ_FEATURE_ENABLE_TOKEN_POLICY;
    ret = umq_init(&cfg);
    ASSERT_NE(ret, 0);
    cfg.feature = 0;
    GlobalMockObject::verify();

    cfg.block_cfg.small_block_size = BLOCK_SIZE_MAX;
    ret = umq_init(&cfg);
    ASSERT_NE(ret, 0);
    cfg.block_cfg.small_block_size = BLOCK_SIZE_8K;

    cfg.trans_info_num = 32;
    ret = umq_init(&cfg);
    ASSERT_NE(ret, 0);
    cfg.trans_info_num = 0;

    MOCKER(umq_perf_init).stubs().will(returnValue(-1));
    cfg.feature = UMQ_FEATURE_ENABLE_PERF;
    ret = umq_init(&cfg);
    ASSERT_NE(ret, 0);
    cfg.feature = 0;
    GlobalMockObject::verify();
}

static int test_umq_ipc_init(void)
{
    umq_init_cfg_t cfg;
    memset(&cfg, 0, sizeof(umq_init_cfg_t));

    cfg.trans_info_num = 1;
    cfg.trans_info[0].trans_mode = UMQ_TRANS_MODE_IPC;

    return umq_init(&cfg);
}

TEST_F(UmqIPCTest, test_umq_init_success)
{
    ASSERT_EQ(test_umq_ipc_init(), 0);

    umq_uninit();
}

TEST_F(UmqIPCTest, test_umq_create_failure)
{
    umq_create_option_t option;
    memset(&option, 0, sizeof(umq_create_option_t));
    (void)snprintf(option.name, UMQ_NAME_MAX_LEN, "%s", "test_umq_ipc");
    uint64_t umqh;

    umqh = umq_create(nullptr);
    ASSERT_EQ(umqh, UMQ_INVALID_HANDLE);

    option.trans_mode = UMQ_TRANS_MODE_MAX;
    umqh = umq_create(&option);
    ASSERT_EQ(umqh, UMQ_INVALID_HANDLE);

    option.trans_mode = UMQ_TRANS_MODE_UCP;
    umqh = umq_create(&option);
    ASSERT_EQ(umqh, UMQ_INVALID_HANDLE);
    option.trans_mode = UMQ_TRANS_MODE_UB;
}

static uint64_t test_umq_create_ipc_qh(void)
{
    umq_create_option_t option;
    memset(&option, 0, sizeof(umq_create_option_t));
    (void)snprintf(option.name, UMQ_NAME_MAX_LEN, "%s", "test_umq_ipc");

    // clean up leftover files
    shm_unlink(option.name);

    option.trans_mode = UMQ_TRANS_MODE_IPC;
    return umq_create(&option);
};

TEST_F(UmqIPCTest, test_umq_create_success)
{
    ASSERT_EQ(test_umq_ipc_init(), 0);

    uint64_t umqh;
    umqh = test_umq_create_ipc_qh();
    ASSERT_NE(umqh, UMQ_INVALID_HANDLE);

    ASSERT_EQ(umq_destroy(umqh), 0);

    umq_uninit();
}

TEST_F(UmqIPCTest, test_umq_bind_failure)
{
#define TEST_IPC_BIND_INFO_SIZE 2048

    ASSERT_EQ(test_umq_ipc_init(), 0);

    int ret;
    uint64_t umqh;
    umqh = test_umq_create_ipc_qh();
    ASSERT_NE(umqh, UMQ_INVALID_HANDLE);

    uint8_t bind_info[TEST_IPC_BIND_INFO_SIZE];
    uint32_t bind_info_size;

    ASSERT_EQ(umq_bind_info_get(0, bind_info, TEST_IPC_BIND_INFO_SIZE), 0);
    ASSERT_EQ(umq_bind_info_get(umqh, bind_info, 0), 0);

    bind_info_size = umq_bind_info_get(umqh, bind_info, TEST_IPC_BIND_INFO_SIZE);
    EXPECT_GT(bind_info_size, 0);

    ret = umq_bind(0, bind_info, TEST_IPC_BIND_INFO_SIZE);
    ASSERT_NE(ret, 0);

    ret = umq_bind(umqh, bind_info, 0);
    ASSERT_NE(ret, 0);

    ASSERT_EQ(umq_destroy(umqh), 0);

    umq_uninit();
}

TEST_F(UmqIPCTest, test_umq_bind_success)
{
#define TEST_IPC_BIND_INFO_SIZE 2048

    ASSERT_EQ(test_umq_ipc_init(), 0);

    int ret;
    uint64_t umqh, umqh1;
    umqh = test_umq_create_ipc_qh();
    ASSERT_NE(umqh, UMQ_INVALID_HANDLE);

    umqh1 = test_umq_create_ipc_qh();
    ASSERT_NE(umqh1, UMQ_INVALID_HANDLE);

    uint8_t bind_info[TEST_IPC_BIND_INFO_SIZE];
    uint32_t bind_info_size;

    bind_info_size = umq_bind_info_get(umqh, bind_info, TEST_IPC_BIND_INFO_SIZE);
    EXPECT_GT(bind_info_size, 0);

    ret = umq_bind(umqh1, bind_info, bind_info_size);
    ASSERT_EQ(ret, 0);

    ASSERT_EQ(umq_unbind(umqh1), 0);

    ASSERT_EQ(umq_destroy(umqh), 0);
    ASSERT_EQ(umq_destroy(umqh1), 0);

    umq_uninit();
}

// IPC umq not support
TEST_F(UmqIPCTest, test_umq_state_set_failure)
{
    ASSERT_EQ(test_umq_ipc_init(), 0);
    uint64_t umqh;
    umqh = test_umq_create_ipc_qh();
    ASSERT_NE(umqh, UMQ_INVALID_HANDLE);

    ASSERT_NE(umq_state_set(0, QUEUE_STATE_IDLE), 0);
    ASSERT_NE(umq_state_set(umqh, QUEUE_STATE_IDLE), 0);

    ASSERT_EQ(umq_destroy(umqh), 0);

    umq_uninit();
}

// IPC umq only support get umq state
TEST_F(UmqIPCTest, test_umq_state_get_success)
{
    ASSERT_EQ(test_umq_ipc_init(), 0);
    uint64_t umqh;
    umqh = test_umq_create_ipc_qh();
    ASSERT_NE(umqh, UMQ_INVALID_HANDLE);

    ASSERT_EQ(umq_state_get(umqh), QUEUE_STATE_READY);

    ASSERT_EQ(umq_destroy(umqh), 0);

    umq_uninit();
}

// IPC umq not support
TEST_F(UmqIPCTest, test_umq_async_event_fd_get_failure)
{
    ASSERT_EQ(test_umq_ipc_init(), 0);

    umq_trans_info_t trans_info;
    memset(&trans_info, 0, sizeof(umq_trans_info_t));
    trans_info.trans_mode = UMQ_TRANS_MODE_IPC;
    ASSERT_EQ(umq_async_event_fd_get(&trans_info), UMQ_INVALID_FD);

    umq_uninit();
}

// IPC umq not support
TEST_F(UmqIPCTest, test_umq_get_async_event_failure)
{
    ASSERT_EQ(test_umq_ipc_init(), 0);

    umq_trans_info_t trans_info;
    memset(&trans_info, 0, sizeof(umq_trans_info_t));
    trans_info.trans_mode = UMQ_TRANS_MODE_IPC;
    umq_async_event_t event;
    ASSERT_NE(umq_get_async_event(&trans_info, nullptr), 0);
    ASSERT_NE(umq_get_async_event(&trans_info, &event), 0);

    umq_uninit();
}

// IPC umq not support
TEST_F(UmqIPCTest, test_umq_ack_async_event_failure)
{
    ASSERT_EQ(test_umq_ipc_init(), 0);

    umq_async_event_t event;
    umq_ack_async_event(nullptr);
    event.trans_info.trans_mode = UMQ_TRANS_MODE_MAX;
    umq_ack_async_event(&event);
    event.trans_info.trans_mode = UMQ_TRANS_MODE_IPC;
    umq_ack_async_event(&event);

    umq_uninit();
}

// IPC umq not support
TEST_F(UmqIPCTest, test_umq_get_route_list_failure)
{
    umq_route_t route;
    umq_route_list_t route_list;

    ASSERT_NE(umq_get_route_list(&route, UMQ_TRANS_MODE_IPC, &route_list), 0);

    ASSERT_EQ(test_umq_ipc_init(), 0);

    ASSERT_NE(umq_get_route_list(nullptr, UMQ_TRANS_MODE_IPC, nullptr), 0);
    ASSERT_NE(umq_get_route_list(&route, UMQ_TRANS_MODE_IPC, nullptr), 0);
    ASSERT_NE(umq_get_route_list(&route, UMQ_TRANS_MODE_MAX, &route_list), 0);
    ASSERT_NE(umq_get_route_list(&route, UMQ_TRANS_MODE_IPC, &route_list), 0);

    umq_uninit();
}

// IPC umq not support
TEST_F(UmqIPCTest, test_umq_user_ctl_failure)
{
    ASSERT_EQ(test_umq_ipc_init(), 0);
    uint64_t umqh;
    umqh = test_umq_create_ipc_qh();
    ASSERT_NE(umqh, UMQ_INVALID_HANDLE);

    umq_user_ctl_in_t in;
    umq_user_ctl_out_t out;
    ASSERT_NE(umq_user_ctl(umqh, nullptr, nullptr), 0);
    ASSERT_NE(umq_user_ctl(umqh, &in, nullptr), 0);
    ASSERT_NE(umq_user_ctl(umqh, nullptr, &out), 0);
    ASSERT_NE(umq_user_ctl(umqh, &in, &out), 0);

    ASSERT_EQ(umq_destroy(umqh), 0);
    umq_uninit();
}

// IPC umq not support
TEST_F(UmqIPCTest, test_umq_dev_add_failure)
{
    umq_trans_info_t trans_info;
    memset(&trans_info, 0, sizeof(umq_trans_info_t));
    trans_info.trans_mode = UMQ_TRANS_MODE_IPC;
    ASSERT_NE(umq_dev_add(umqh, &in, &out), 0);
}