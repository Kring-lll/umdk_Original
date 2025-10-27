/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: urpc queue test
 */

#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <deque>
#include <iostream>

#include "urma_api.h"
#include "urpc_framework_errno.h"
#include "provider_ops_jetty.h"
#include "cp.h"
#include "dp.h"
#include "urpc_timer.h"
#include "queue.h"
#include "jetty_public_func.h"
#include "queue_send_recv.h"
#include "protocol.h"

#include "state.h"
#include "urpc_framework_api.h"

#include "urpc_dbuf_stat.h"
#include "notify.h"
#include <random>
#include <future>


extern queue_ops_t g_urpc_send_recv_ops;

class QueueSendRecvTestNoThing : public :: testing::Test {
public:
    void SetUp() override {
    }

    void TearDown() override {
        urpc_uninit();
        GlobalMockObject::verify();
    }
};

TEST_F(QueueSendRecvTestNoThing, CoverageSendRecvTransportInfoGetNoProvider)
{
    queue_t queue;
    urpc_ext_q_cfg_get_t cfg_get;
    // 覆盖tranport_info_get中queue的provider为空的情况
    queue.provider = NULL;
    int ret = g_urpc_send_recv_ops.tranport_info_get(&queue, &cfg_get);
    ASSERT_EQ(ret, URPC_FAIL);
}

TEST_F(QueueSendRecvTestNoThing, CoverageSendRecvTransportInfoGetNoImport)
{
    provider_flag_t flag = {0};
    provider_init(0, NULL, flag);

    urpc_ext_q_cfg_get_t cfg_get;
    // 覆盖queue 是remote q的情况没有import
    send_recv_queue_remote_t remote_queue;
    urma_target_jetty_t tjetty;
    jetty_provider_t provider_j;
    urma_device_t urma_dev;

    provider_j.urma_dev = &urma_dev;
    urma_dev.type = URMA_TRANSPORT_UB;
    remote_queue.tjetty = &tjetty;
    remote_queue.remote_q.queue.provider = &provider_j.provider;
    remote_queue.remote_q.queue.flag.is_remote = 1;
    remote_queue.flag.is_imported = 0;
    int ret = g_urpc_send_recv_ops.tranport_info_get(&remote_queue.remote_q.queue, &cfg_get);
    ASSERT_EQ(ret, URPC_FAIL);
}

TEST_F(QueueSendRecvTestNoThing, CoverageSendRecvTransportInfoGet)
{
    provider_flag_t flag = {0};
    provider_init(0, NULL, flag);
    urpc_ext_q_cfg_get_t cfg_get;
    // 覆盖queue 是remote q的情况
    send_recv_queue_remote_t remote_queue;
    urma_target_jetty_t tjetty;
    jetty_provider_t provider_j;
    urma_device_t urma_dev;

    provider_j.urma_dev = &urma_dev;
    urma_dev.type = URMA_TRANSPORT_UB;
    remote_queue.tjetty = &tjetty;
    remote_queue.remote_q.queue.provider = &provider_j.provider;
    remote_queue.remote_q.queue.flag.is_remote = 1;
    remote_queue.flag.is_imported = 1;
    int ret = g_urpc_send_recv_ops.tranport_info_get(&remote_queue.remote_q.queue, &cfg_get);
    ASSERT_EQ(ret, URPC_SUCCESS);
}