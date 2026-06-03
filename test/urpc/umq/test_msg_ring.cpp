/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: msg_ring unit test
 */

#include "mockcpp/mockcpp.hpp"
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include "gtest/gtest.h"
#include "msg_ring.h"

class MsgRingTest : public ::testing::Test {
public:
    void SetUp() override {}
    void TearDown() override { GlobalMockObject::verify(); }
};

TEST_F(MsgRingTest, test_msg_ring_create_name_too_long)
{
    char long_name[64] = {0};
    memset(long_name, 'a', 63);
    msg_ring_option_t opt = {.owner = 1, .tx_max_buf_size = 1024, .tx_depth = 8,
                              .rx_max_buf_size = 1024, .rx_depth = 8, .addr = NULL};
    msg_ring_t *ring = msg_ring_create(long_name, 64, &opt);
    ASSERT_EQ(ring, nullptr);
}

TEST_F(MsgRingTest, test_msg_ring_create_success)
{
    char name[] = "/test_ring_create";
    shm_unlink(name);
    msg_ring_option_t opt = {.owner = 1, .tx_max_buf_size = 1024, .tx_depth = 8,
                              .rx_max_buf_size = 1024, .rx_depth = 8, .addr = NULL};
    msg_ring_t *ring = msg_ring_create(name, strlen(name), &opt);
    ASSERT_NE(ring, nullptr);
    msg_ring_destroy(ring);
    shm_unlink(name);
}

TEST_F(MsgRingTest, test_msg_ring_create_with_preallocated_addr)
{
    char name[] = "/test_prealloc";
    shm_unlink(name);
    msg_ring_option_t opt = {.owner = 1, .tx_max_buf_size = 1024, .tx_depth = 8,
                              .rx_max_buf_size = 1024, .rx_depth = 8};
    uint32_t hdr_size = 2 * sizeof(shm_ring_hdr_t);
    uint32_t shm_size = hdr_size + 8 * 1024 + 8 * 1024;
    void *addr = malloc(shm_size);
    ASSERT_NE(addr, nullptr);
    opt.addr = addr;
    msg_ring_t *ring = msg_ring_create(name, strlen(name), &opt);
    ASSERT_NE(ring, nullptr);
    msg_ring_destroy(ring);
    free(addr);
}

TEST_F(MsgRingTest, test_msg_ring_post_tx_and_poll_tx)
{
    char name[] = "/test_ring_post_poll";
    shm_unlink(name);
    msg_ring_option_t opt = {.owner = 1, .tx_max_buf_size = 1024, .tx_depth = 8,
                              .rx_max_buf_size = 1024, .rx_depth = 8, .addr = NULL};
    msg_ring_t *ring = msg_ring_create(name, strlen(name), &opt);
    ASSERT_NE(ring, nullptr);
    char tx_buf[] = "hello msg_ring";
    ASSERT_EQ(msg_ring_post_tx(ring, tx_buf, strlen(tx_buf)), 0);
    char rx_buf[1024] = {0};
    uint32_t avail_size = 0;
    int ret = msg_ring_poll_tx(ring, rx_buf, 1024, &avail_size);
    ASSERT_EQ(ret, 1);
    ASSERT_EQ(avail_size, strlen(tx_buf));
    msg_ring_destroy(ring);
    shm_unlink(name);
}

TEST_F(MsgRingTest, test_msg_ring_post_tx_size_exceed_payload)
{
    char name[] = "/test_ring_size_exceed";
    shm_unlink(name);
    msg_ring_option_t opt = {.owner = 1, .tx_max_buf_size = 64, .tx_depth = 8,
                              .rx_max_buf_size = 64, .rx_depth = 8, .addr = NULL};
    msg_ring_t *ring = msg_ring_create(name, strlen(name), &opt);
    ASSERT_NE(ring, nullptr);
    char big_buf[128] = {0};
    int ret = msg_ring_post_tx(ring, big_buf, 128);
    ASSERT_EQ(ret, -1);
    msg_ring_destroy(ring);
    shm_unlink(name);
}

TEST_F(MsgRingTest, test_msg_ring_post_tx_queue_full)
{
    char name[] = "/test_ring_full";
    shm_unlink(name);
    msg_ring_option_t opt = {.owner = 1, .tx_max_buf_size = 64, .tx_depth = 2,
                              .rx_max_buf_size = 64, .rx_depth = 2, .addr = NULL};
    msg_ring_t *ring = msg_ring_create(name, strlen(name), &opt);
    ASSERT_NE(ring, nullptr);
    char buf1[] = "msg1";
    char buf2[] = "msg2";
    ASSERT_EQ(msg_ring_post_tx(ring, buf1, 4), 0);
    ASSERT_EQ(msg_ring_post_tx(ring, buf2, 4), -EAGAIN);
    msg_ring_destroy(ring);
    shm_unlink(name);
}

TEST_F(MsgRingTest, test_msg_ring_poll_tx_empty)
{
    char name[] = "/test_ring_empty";
    shm_unlink(name);
    msg_ring_option_t opt = {.owner = 1, .tx_max_buf_size = 64, .tx_depth = 8,
                              .rx_max_buf_size = 64, .rx_depth = 8, .addr = NULL};
    msg_ring_t *ring = msg_ring_create(name, strlen(name), &opt);
    ASSERT_NE(ring, nullptr);
    char buf[64] = {0};
    uint32_t avail_size = 0;
    int ret = msg_ring_poll_tx(ring, buf, 64, &avail_size);
    ASSERT_EQ(ret, 0);
    msg_ring_destroy(ring);
    shm_unlink(name);
}

TEST_F(MsgRingTest, test_msg_ring_poll_tx_buf_too_small)
{
    char name[] = "/test_ring_buf_small";
    shm_unlink(name);
    msg_ring_option_t opt = {.owner = 1, .tx_max_buf_size = 256, .tx_depth = 8,
                              .rx_max_buf_size = 256, .rx_depth = 8, .addr = NULL};
    msg_ring_t *ring = msg_ring_create(name, strlen(name), &opt);
    ASSERT_NE(ring, nullptr);
    char tx_buf[256];
    memset(tx_buf, 'x', 200);
    ASSERT_EQ(msg_ring_post_tx(ring, tx_buf, 200), 0);
    char rx_buf[64] = {0};
    uint32_t avail_size = 0;
    int ret = msg_ring_poll_tx(ring, rx_buf, 64, &avail_size);
    ASSERT_EQ(ret, -1);
    msg_ring_destroy(ring);
    shm_unlink(name);
}

TEST_F(MsgRingTest, test_msg_ring_post_rx_and_poll_rx)
{
    char name[] = "/test_ring_rx";
    shm_unlink(name);
    msg_ring_option_t opt = {.owner = 1, .tx_max_buf_size = 256, .tx_depth = 8,
                              .rx_max_buf_size = 256, .rx_depth = 8, .addr = NULL};
    msg_ring_t *ring = msg_ring_create(name, strlen(name), &opt);
    ASSERT_NE(ring, nullptr);
    char rx_data[] = "rx_hello";
    ASSERT_EQ(msg_ring_post_rx(ring, rx_data, strlen(rx_data)), 0);
    char buf[256] = {0};
    uint32_t avail_size = 0;
    int ret = msg_ring_poll_rx(ring, buf, 256, &avail_size);
    ASSERT_EQ(ret, 1);
    ASSERT_EQ(avail_size, strlen(rx_data));
    msg_ring_destroy(ring);
    shm_unlink(name);
}

TEST_F(MsgRingTest, test_msg_ring_post_rx_size_exceed)
{
    char name[] = "/test_ring_rx_size";
    shm_unlink(name);
    msg_ring_option_t opt = {.owner = 1, .tx_max_buf_size = 64, .tx_depth = 8,
                              .rx_max_buf_size = 64, .rx_depth = 8, .addr = NULL};
    msg_ring_t *ring = msg_ring_create(name, strlen(name), &opt);
    ASSERT_NE(ring, nullptr);
    char big_buf[128] = {0};
    int ret = msg_ring_post_rx(ring, big_buf, 128);
    ASSERT_EQ(ret, -1);
    msg_ring_destroy(ring);
    shm_unlink(name);
}

TEST_F(MsgRingTest, test_msg_ring_poll_rx_empty)
{
    char name[] = "/test_ring_rx_empty";
    shm_unlink(name);
    msg_ring_option_t opt = {.owner = 1, .tx_max_buf_size = 256, .tx_depth = 8,
                              .rx_max_buf_size = 256, .rx_depth = 8, .addr = NULL};
    msg_ring_t *ring = msg_ring_create(name, strlen(name), &opt);
    ASSERT_NE(ring, nullptr);
    char buf[256] = {0};
    uint32_t avail_size = 0;
    int ret = msg_ring_poll_rx(ring, buf, 256, &avail_size);
    ASSERT_EQ(ret, 0);
    msg_ring_destroy(ring);
    shm_unlink(name);
}

TEST_F(MsgRingTest, test_msg_ring_destroy_not_owner)
{
    char name[] = "/test_ring_not_owner";
    shm_unlink(name);
    msg_ring_option_t opt = {.owner = 1, .tx_max_buf_size = 256, .tx_depth = 8,
                              .rx_max_buf_size = 256, .rx_depth = 8, .addr = NULL};
    msg_ring_t *ring1 = msg_ring_create(name, strlen(name), &opt);
    ASSERT_NE(ring1, nullptr);
    msg_ring_option_t opt2 = {.owner = 0, .tx_max_buf_size = 256, .tx_depth = 8,
                               .rx_max_buf_size = 256, .rx_depth = 8, .addr = NULL};
    msg_ring_t *ring2 = msg_ring_create(name, strlen(name), &opt2);
    ASSERT_NE(ring2, nullptr);
    msg_ring_destroy(ring2);
    char buf[] = "still alive";
    ASSERT_EQ(msg_ring_post_tx(ring1, buf, strlen(buf)), 0);
    msg_ring_destroy(ring1);
    shm_unlink(name);
}
