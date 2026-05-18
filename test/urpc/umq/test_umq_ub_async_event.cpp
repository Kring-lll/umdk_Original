/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: umq ub async event test
 */

#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"

#include "umq_api.h"
#include "umq_ub_impl.h"
#include "umq_ub_private.h"
#include "urma_api.h"
#include "umq_ub_flow_control.h"

class UmqUBAsyncEventTest : public ::testing::Test {
public:
    void SetUp() override
    {
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
};

TEST_F(UmqUBAsyncEventTest, test_umq_ub_async_event_get_null_trans_info)
{
    umq_async_event_t event = {0};
    int ret = umq_ub_async_event_get(NULL, &event);
    ASSERT_EQ(ret, -UMQ_ERR_EINVAL);
}

TEST_F(UmqUBAsyncEventTest, test_umq_ub_async_event_get_null_event)
{
    umq_trans_info_t trans_info = {0};
    int ret = umq_ub_async_event_get(&trans_info, NULL);
    ASSERT_EQ(ret, -UMQ_ERR_EINVAL);
}

TEST_F(UmqUBAsyncEventTest, test_umq_ub_async_event_get_invalid_dev_ctx)
{
    umq_trans_info_t trans_info = {0};
    umq_async_event_t event = {0};
    
    int ret = umq_ub_async_event_get(&trans_info, &event);
    ASSERT_EQ(ret, -UMQ_ERR_EINVAL);
}