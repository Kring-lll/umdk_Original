/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: Boundary and edge case tests for URPC module
 */

#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>
#include <cstring>
#include <limits.h>

#include "msg_ring.h"
#include "umq_api.h"
#include "umq_inner.h"
#include "urpc_bitmap.h"
#include "urpc_id_generator.h"
#include "urpc_pool.h"
#include "urpc_slab.h"

class BoundaryTest : public ::testing::Test {
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

class EdgeCaseTest : public ::testing::Test {
public:
    void SetUp() override
    {
    }

    void TearDown() override
    {
        GlobalMockObject::verify();
    }
};

class ConcurrentTest : public ::testing::Test {
public:
    void SetUp() override
    {
    }

    void TearDown() override
    {
    }
};

TEST_F(BoundaryTest, test_bitmap_boundary_min_size)
{
    urpc_bitmap_t bitmap = urpc_bitmap_alloc(1);
    ASSERT_NE(bitmap, nullptr);
    urpc_bitmap_set(bitmap, 0, true);
    ASSERT_EQ(urpc_bitmap_is_set(bitmap, 0), true);
    urpc_bitmap_set(bitmap, 0, false);
    ASSERT_EQ(urpc_bitmap_is_set(bitmap, 0), false);
    urpc_bitmap_free(bitmap);
}

TEST_F(BoundaryTest, test_bitmap_boundary_max_index)
{
    urpc_bitmap_t bitmap = urpc_bitmap_alloc(1024);
    ASSERT_NE(bitmap, nullptr);
    urpc_bitmap_set(bitmap, 1023, true);
    ASSERT_EQ(urpc_bitmap_is_set(bitmap, 1023), true);
    urpc_bitmap_set(bitmap, 1023, false);
    ASSERT_EQ(urpc_bitmap_is_set(bitmap, 1023), false);
    urpc_bitmap_free(bitmap);
}

TEST_F(BoundaryTest, test_bitmap_boundary_zero_size)
{
    urpc_bitmap_t bitmap = urpc_bitmap_alloc(0);
    if (bitmap != nullptr) {
        urpc_bitmap_free(bitmap);
    }
}

TEST_F(BoundaryTest, test_id_generator_boundary_min_id)
{
    urpc_id_generator_t id_gen;
    urpc_id_generator_init(&id_gen, URPC_ID_GENERATOR_TYPE_BITMAP, 1024);

    uint32_t id;
    int ret = urpc_id_generator_alloc(&id_gen, 0, &id);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(id, (uint32_t)0);
    
    urpc_id_generator_uninit(&id_gen);
}

TEST_F(BoundaryTest, test_id_generator_boundary_max_id)
{
    urpc_id_generator_t id_gen;
    urpc_id_generator_init(&id_gen, URPC_ID_GENERATOR_TYPE_BITMAP, 1024);

    uint32_t id;
    int ret = urpc_id_generator_alloc(&id_gen, 1023, &id);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(id, (uint32_t)1023);
    
    urpc_id_generator_uninit(&id_gen);
}

TEST_F(BoundaryTest, test_id_generator_boundary_exceed_max)
{
    urpc_id_generator_t id_gen;
    urpc_id_generator_init(&id_gen, URPC_ID_GENERATOR_TYPE_BITMAP, 1024);

    uint32_t id;
    int ret = urpc_id_generator_alloc(&id_gen, 1024, &id);
    ASSERT_EQ(ret, 0);
    ASSERT_NE(id, (uint32_t)1024);
    
    urpc_id_generator_uninit(&id_gen);
}

TEST_F(BoundaryTest, test_id_generator_boundary_negative_hint)
{
    urpc_id_generator_t id_gen;
    urpc_id_generator_init(&id_gen, URPC_ID_GENERATOR_TYPE_BITMAP, 1024);

    uint32_t id;
    int ret = urpc_id_generator_alloc(&id_gen, -1, &id);
    ASSERT_EQ(ret, -ENOSPC);
    
    urpc_id_generator_uninit(&id_gen);
}

TEST_F(BoundaryTest, test_msg_ring_boundary_max_name_length)
{
    char name[MAX_MSG_RING_NAME];
    memset(name, 'a', MAX_MSG_RING_NAME - 1);
    name[MAX_MSG_RING_NAME - 1] = '\0';
    
    msg_ring_option_t cfg;
    memset(&cfg, 0, sizeof(msg_ring_option_t));
    cfg.owner = true;
    cfg.tx_max_buf_size = 32;
    cfg.tx_depth = 32;
    cfg.rx_max_buf_size = 32;
    cfg.rx_depth = 32;

    msg_ring_t *r = msg_ring_create(name, MAX_MSG_RING_NAME, &cfg);
    if (r != nullptr) {
        msg_ring_destroy(r);
    }
}

TEST_F(BoundaryTest, test_msg_ring_boundary_exceed_name_length)
{
    char name[MAX_MSG_RING_NAME + 1];
    memset(name, 'a', MAX_MSG_RING_NAME);
    name[MAX_MSG_RING_NAME] = '\0';
    
    msg_ring_option_t cfg;
    memset(&cfg, 0, sizeof(msg_ring_option_t));
    cfg.owner = true;
    cfg.tx_max_buf_size = 32;
    cfg.tx_depth = 32;
    cfg.rx_max_buf_size = 32;
    cfg.rx_depth = 32;

    msg_ring_t *r = msg_ring_create(name, sizeof(name), &cfg);
    ASSERT_EQ(r, nullptr);
}

TEST_F(BoundaryTest, test_msg_ring_boundary_zero_depth)
{
    char name[MAX_MSG_RING_NAME];
    snprintf(name, MAX_MSG_RING_NAME, "%s", "test_zero_depth");
    
    msg_ring_option_t cfg;
    memset(&cfg, 0, sizeof(msg_ring_option_t));
    cfg.owner = true;
    cfg.tx_max_buf_size = 32;
    cfg.tx_depth = 0;
    cfg.rx_max_buf_size = 32;
    cfg.rx_depth = 0;

    msg_ring_t *r = msg_ring_create(name, MAX_MSG_RING_NAME, &cfg);
    if (r != nullptr) {
        msg_ring_destroy(r);
    }
}

TEST_F(BoundaryTest, test_msg_ring_boundary_large_buf_size)
{
    char name[MAX_MSG_RING_NAME];
    snprintf(name, MAX_MSG_RING_NAME, "%s", "test_large_buf");
    
    msg_ring_option_t cfg;
    memset(&cfg, 0, sizeof(msg_ring_option_t));
    cfg.owner = true;
    cfg.tx_max_buf_size = 1024 * 1024;
    cfg.tx_depth = 32;
    cfg.rx_max_buf_size = 1024 * 1024;
    cfg.rx_depth = 32;

    msg_ring_t *r = msg_ring_create(name, MAX_MSG_RING_NAME, &cfg);
    if (r != nullptr) {
        msg_ring_destroy(r);
    }
}

TEST_F(EdgeCaseTest, test_umq_log_config_null_pointer)
{
    ASSERT_NE(umq_log_config_get(nullptr), 0);
    ASSERT_NE(umq_log_config_set(nullptr), 0);
}

TEST_F(EdgeCaseTest, test_umq_log_config_invalid_level)
{
    umq_log_config_t cfg;
    memset(&cfg, 0, sizeof(umq_log_config_t));
    
    cfg.log_flag |= UMQ_LOG_FLAG_LEVEL;
    cfg.level = UMQ_LOG_LEVEL_MAX;
    ASSERT_NE(umq_log_config_set(&cfg), 0);
    
    cfg.level = UMQ_LOG_LEVEL_EMERG + 100;
    ASSERT_NE(umq_log_config_set(&cfg), 0);
}

TEST_F(EdgeCaseTest, test_is_timeout_edge_cases)
{
    struct timespec start;
    
    clock_gettime(CLOCK_MONOTONIC, &start);
    ASSERT_EQ(is_timeout(&start, 0), false);
    
    start.tv_sec -= 1000;
    ASSERT_EQ(is_timeout(&start, 1), true);
    
    start.tv_sec = 0;
    start.tv_nsec = 0;
    ASSERT_EQ(is_timeout(&start, INT_MAX), false);
}

TEST_F(EdgeCaseTest, test_msg_ring_null_name)
{
    msg_ring_option_t cfg;
    memset(&cfg, 0, sizeof(msg_ring_option_t));
    cfg.owner = true;
    cfg.tx_max_buf_size = 32;
    cfg.tx_depth = 32;
    cfg.rx_max_buf_size = 32;
    cfg.rx_depth = 32;

    msg_ring_t *r = msg_ring_create(nullptr, 0, &cfg);
    ASSERT_EQ(r, nullptr);
}

TEST_F(EdgeCaseTest, test_msg_ring_null_config)
{
    char name[MAX_MSG_RING_NAME];
    snprintf(name, MAX_MSG_RING_NAME, "%s", "test_null_cfg");
    
    msg_ring_t *r = msg_ring_create(name, MAX_MSG_RING_NAME, nullptr);
    ASSERT_EQ(r, nullptr);
}

TEST_F(EdgeCaseTest, test_msg_ring_post_empty_message)
{
    msg_ring_t *r_t, *r_r;
    char name[MAX_MSG_RING_NAME];
    snprintf(name, MAX_MSG_RING_NAME, "%s", "test_empty_msg");
    
    msg_ring_option_t cfg;
    memset(&cfg, 0, sizeof(msg_ring_option_t));
    cfg.owner = true;
    cfg.tx_max_buf_size = 32;
    cfg.tx_depth = 32;
    cfg.rx_max_buf_size = 32;
    cfg.rx_depth = 32;

    shm_unlink(name);
    r_t = msg_ring_create(name, MAX_MSG_RING_NAME, &cfg);
    ASSERT_NE(r_t, nullptr);
    
    cfg.owner = false;
    r_r = msg_ring_create(name, MAX_MSG_RING_NAME, &cfg);
    ASSERT_NE(r_r, nullptr);
    
    int ret = msg_ring_post_tx(r_t, "", 0);
    ASSERT_EQ(ret, 0);
    
    char recv_msg[32];
    uint32_t recv_len = 0;
    ret = msg_ring_poll_tx(r_r, recv_msg, 32, &recv_len);
    
    msg_ring_destroy(r_t);
    msg_ring_destroy(r_r);
}

static void *concurrent_bitmap_worker(void *arg)
{
    urpc_bitmap_t bitmap = (urpc_bitmap_t)arg;
    for (int i = 0; i < 100; i++) {
        int idx = i % 100;
        urpc_bitmap_set(bitmap, idx, true);
        urpc_bitmap_is_set(bitmap, idx);
        urpc_bitmap_set(bitmap, idx, false);
    }
    return nullptr;
}

TEST_F(ConcurrentTest, test_bitmap_concurrent_access)
{
    urpc_bitmap_t bitmap = urpc_bitmap_alloc(1024);
    ASSERT_NE(bitmap, nullptr);
    
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], nullptr, concurrent_bitmap_worker, bitmap);
    }
    
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], nullptr);
    }
    
    urpc_bitmap_free(bitmap);
}

static void *concurrent_id_gen_worker(void *arg)
{
    urpc_id_generator_t *id_gen = (urpc_id_generator_t *)arg;
    uint32_t id;
    for (int i = 0; i < 100; i++) {
        urpc_id_generator_alloc(id_gen, i % 100, &id);
    }
    return nullptr;
}

TEST_F(ConcurrentTest, test_id_generator_concurrent_alloc)
{
    urpc_id_generator_t id_gen;
    urpc_id_generator_init(&id_gen, URPC_ID_GENERATOR_TYPE_BITMAP, 1024);
    
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], nullptr, concurrent_id_gen_worker, &id_gen);
    }
    
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], nullptr);
    }
    
    urpc_id_generator_uninit(&id_gen);
}

TEST_F(ConcurrentTest, test_pool_concurrent_get_put)
{
    urpc_pool_config_t cfg;
    cfg.element_size = 128;
    cfg.element_num_per_block = 100;
    cfg.block_num = 4;
    
    urpc_pool_t pool;
    int ret = urpc_pool_init(&cfg, &pool);
    ASSERT_EQ(ret, 0);
    
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], nullptr, [](void *arg) -> void* {
            urpc_pool_t *p = (urpc_pool_t *)arg;
            void *elems[50];
            for (int j = 0; j < 50; j++) {
                elems[j] = urpc_pool_element_get(p);
                if (elems[j] != nullptr) {
                    memset(elems[j], 0, 128);
                }
            }
            for (int j = 0; j < 50; j++) {
                if (elems[j] != nullptr) {
                    urpc_pool_element_put(p, elems[j]);
                }
            }
            return nullptr;
        }, &pool);
    }
    
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], nullptr);
    }
    
    urpc_pool_thread_closure(0);
    urpc_pool_uninit(&pool);
}

TEST_F(BoundaryTest, test_pool_boundary_min_config)
{
    urpc_pool_config_t cfg;
    cfg.element_size = 1;
    cfg.element_num_per_block = 1;
    cfg.block_num = 1;
    
    urpc_pool_t pool;
    int ret = urpc_pool_init(&cfg, &pool);
    if (ret == 0) {
        void *elem = urpc_pool_element_get(&pool);
        if (elem != nullptr) {
            urpc_pool_element_put(&pool, elem);
        }
        urpc_pool_thread_closure(0);
        urpc_pool_uninit(&pool);
    }
}

TEST_F(BoundaryTest, test_pool_boundary_large_config)
{
    urpc_pool_config_t cfg;
    cfg.element_size = 4096;
    cfg.element_num_per_block = 1000;
    cfg.block_num = 100;
    
    urpc_pool_t pool;
    int ret = urpc_pool_init(&cfg, &pool);
    if (ret == 0) {
        urpc_pool_thread_closure(0);
        urpc_pool_uninit(&pool);
    }
}

TEST_F(EdgeCaseTest, test_pool_null_config)
{
    urpc_pool_t pool;
    int ret = urpc_pool_init(nullptr, &pool);
    ASSERT_NE(ret, 0);
}

TEST_F(EdgeCaseTest, test_pool_null_pool)
{
    urpc_pool_config_t cfg;
    cfg.element_size = 128;
    cfg.element_num_per_block = 10;
    cfg.block_num = 1;
    
    int ret = urpc_pool_init(&cfg, nullptr);
    ASSERT_NE(ret, 0);
}

TEST_F(EdgeCaseTest, test_pool_get_from_null)
{
    void *elem = urpc_pool_element_get(nullptr);
    ASSERT_EQ(elem, nullptr);
}

TEST_F(EdgeCaseTest, test_pool_put_to_null)
{
    void *elem = malloc(128);
    urpc_pool_element_put(nullptr, elem);
    free(elem);
}