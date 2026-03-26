/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: umq ub device info unit tests
 */

#include "gtest/gtest.h"

#include "umq_ub_private.h"
#include "umq_types.h"
#include "umq_errno.h"

class UmqUbDevTest : public ::testing::Test {
  public:
    void SetUp() override
    {
    }

    void TearDown() override
    {
    }
};

TEST_F(UmqUbDevTest, test_umq_ub_dev_str_get_by_dev_name)
{
    umq_dev_assign_t dev_info = {0};
    char dev_str[128] = {0};

    (void)strncpy(dev_info.dev.dev_name, "bonding_dev_0", UMQ_DEV_NAME_SIZE - 1);
    dev_info.dev.eid_idx = 5;
    dev_info.assign_mode = UMQ_DEV_ASSIGN_MODE_DEV;

    int ret = umq_ub_dev_str_get(&dev_info, dev_str, sizeof(dev_str));
    ASSERT_EQ(ret, UMQ_SUCCESS);
    ASSERT_STREQ(dev_str, "bonding_dev_0[5]");
}

TEST_F(UmqUbDevTest, test_umq_ub_dev_str_get_by_eid)
{
    umq_dev_assign_t dev_info = {0};
    char dev_str[128] = {0};

    dev_info.eid.eid.ipaddr = 0x7F000001;  // 127.0.0.1
    dev_info.eid.eid.ifindex = 1;
    dev_info.assign_mode = UMQ_DEV_ASSIGN_MODE_EID;

    int ret = umq_ub_dev_str_get(&dev_info, dev_str, sizeof(dev_str));
    ASSERT_EQ(ret, UMQ_SUCCESS);
    ASSERT_NE(dev_str[0], '\0');
}

TEST_F(UmqUbDevTest, test_umq_ub_dev_str_get_by_ipv4)
{
    umq_dev_assign_t dev_info = {0};
    char dev_str[128] = {0};

    (void)strncpy(dev_info.ipv4.ip_addr, "192.168.1.100", sizeof(dev_info.ipv4.ip_addr) - 1);
    dev_info.assign_mode = UMQ_DEV_ASSIGN_MODE_IPV4;

    int ret = umq_ub_dev_str_get(&dev_info, dev_str, sizeof(dev_str));
    ASSERT_EQ(ret, UMQ_SUCCESS);
    ASSERT_STREQ(dev_str, "192.168.1.100");
}

TEST_F(UmqUbDevTest, test_umq_ub_dev_str_get_by_ipv6)
{
    umq_dev_assign_t dev_info = {0};
    char dev_str[128] = {0};

    (void)strncpy(dev_info.ipv6.ip_addr, "::1", sizeof(dev_info.ipv6.ip_addr) - 1);
    dev_info.assign_mode = UMQ_DEV_ASSIGN_MODE_IPV6;

    int ret = umq_ub_dev_str_get(&dev_info, dev_str, sizeof(dev_str));
    ASSERT_EQ(ret, UMQ_SUCCESS);
    ASSERT_STREQ(dev_str, "::1");
}

TEST_F(UmqUbDevTest, test_umq_ub_dev_str_get_invalid_mode)
{
    umq_dev_assign_t dev_info = {0};
    char dev_str[128] = {0};

    dev_info.assign_mode = UMQ_DEV_ASSIGN_MODE_MAX;  // Invalid mode

    int ret = umq_ub_dev_str_get(&dev_info, dev_str, sizeof(dev_str));
    ASSERT_NE(ret, UMQ_SUCCESS);
}

TEST_F(UmqUbDevTest, test_umq_ub_dev_str_get_buffer_too_small)
{
    umq_dev_assign_t dev_info = {0};
    char dev_str[8] = {0};  // Small buffer

    (void)strncpy(dev_info.dev.dev_name, "very_long_device_name", UMQ_DEV_NAME_SIZE - 1);
    dev_info.dev.eid_idx = 999;
    dev_info.assign_mode = UMQ_DEV_ASSIGN_MODE_DEV;

    int ret = umq_ub_dev_str_get(&dev_info, dev_str, sizeof(dev_str));
    ASSERT_NE(ret, UMQ_SUCCESS);
}
