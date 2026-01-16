/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: umq qbuf test
 */

#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"

#include "umq_api.h"
#include "umq_qbuf_pool.h"
#include "urpc_util.h"

class UmqQbufTest : public ::testing::Test {
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

TEST_F(UmqQbufTest, test_umq_buf_alloc_failure)
{
    umq_alloc_option_t option;
    memset(&option, 0, sizeof(umq_alloc_option_t));
    umq_buf_t *qbuf;

    qbuf = umq_buf_alloc(4096, 0, 0, nullptr);
    ASSERT_EQ(qbuf, nullptr);

    option.flag = UMQ_ALLOC_FLAG_HEAD_ROOM_SIZE;
    option.headroom_size = UMQ_HEADROOM_SIZE_LIMIT + 1;
    qbuf = umq_buf_alloc(4096, 1, 0, &option);
    ASSERT_EQ(qbuf, nullptr);
    option.flag = 0;
    option.headroom_size = 0;

    qbuf = umq_buf_alloc(4096, 1, 0, &option);
    ASSERT_EQ(qbuf, nullptr);
}
