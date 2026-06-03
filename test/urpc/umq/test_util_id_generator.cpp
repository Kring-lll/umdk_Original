/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: util_id_generator unit test
 */

#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "util_id_generator.h"

class IdAllocatorTest : public ::testing::Test {
public:
    util_id_allocator_t allocator;
    void SetUp() override { memset(&allocator, 0, sizeof(allocator)); }
    void TearDown() override { GlobalMockObject::verify(); }
};

TEST_F(IdAllocatorTest, test_init_success)
{
    int ret = util_id_allocator_init(&allocator, 1024, 0);
    ASSERT_EQ(ret, 0);
    ASSERT_NE(allocator.available_ids, nullptr);
    ASSERT_EQ(allocator.max_num, 1024);
    ASSERT_EQ(allocator.next_id, 0);
    ASSERT_EQ(allocator.num_available, 0);
    util_id_allocator_uninit(&allocator);
}

TEST_F(IdAllocatorTest, test_init_already_initialized)
{
    int ret = util_id_allocator_init(&allocator, 1024, 0);
    ASSERT_EQ(ret, 0);
    ret = util_id_allocator_init(&allocator, 1024, 0);
    ASSERT_EQ(ret, -1);
    util_id_allocator_uninit(&allocator);
}



TEST_F(IdAllocatorTest, test_get_id_sequential)
{
    ASSERT_EQ(util_id_allocator_init(&allocator, 1024, 0), 0);
    ASSERT_EQ(util_id_allocator_get(&allocator), 0);
    ASSERT_EQ(util_id_allocator_get(&allocator), 1);
    ASSERT_EQ(util_id_allocator_get(&allocator), 2);
    util_id_allocator_uninit(&allocator);
}

TEST_F(IdAllocatorTest, test_get_id_with_start_id)
{
    ASSERT_EQ(util_id_allocator_init(&allocator, 1024, 100), 0);
    ASSERT_EQ(util_id_allocator_get(&allocator), 100);
    ASSERT_EQ(util_id_allocator_get(&allocator), 101);
    util_id_allocator_uninit(&allocator);
}

TEST_F(IdAllocatorTest, test_release_and_reuse_id)
{
    ASSERT_EQ(util_id_allocator_init(&allocator, 1024, 0), 0);
    uint32_t id1 = util_id_allocator_get(&allocator);
    uint32_t id2 = util_id_allocator_get(&allocator);
    ASSERT_EQ(id1, 0);
    ASSERT_EQ(id2, 1);
    util_id_allocator_release(&allocator, id1);
    uint32_t id3 = util_id_allocator_get(&allocator);
    ASSERT_EQ(id3, id1);
    uint32_t id4 = util_id_allocator_get(&allocator);
    ASSERT_EQ(id4, 2);
    util_id_allocator_uninit(&allocator);
}

TEST_F(IdAllocatorTest, test_id_exhaustion)
{
    ASSERT_EQ(util_id_allocator_init(&allocator, 3, 0), 0);
    ASSERT_EQ(util_id_allocator_get(&allocator), 0);
    ASSERT_EQ(util_id_allocator_get(&allocator), 1);
    ASSERT_EQ(util_id_allocator_get(&allocator), 2);
    uint32_t id = util_id_allocator_get(&allocator);
    ASSERT_EQ(id, (uint32_t)3);
    util_id_allocator_uninit(&allocator);
}

TEST_F(IdAllocatorTest, test_release_when_not_initialized)
{
    memset(&allocator, 0, sizeof(allocator));
    util_id_allocator_release(&allocator, 0);
    SUCCEED();
}


TEST_F(IdAllocatorTest, test_multiple_release_and_get_cycle)
{
    ASSERT_EQ(util_id_allocator_init(&allocator, 100, 0), 0);
    uint32_t ids[10];
    for (int i = 0; i < 10; i++) {
        ids[i] = util_id_allocator_get(&allocator);
        ASSERT_EQ(ids[i], (uint32_t)i);
    }
    for (int i = 0; i < 10; i++) {
        util_id_allocator_release(&allocator, ids[i]);
    }
    for (int i = 0; i < 10; i++) {
        uint32_t id = util_id_allocator_get(&allocator);
        ASSERT_NE(id, (uint32_t)(10 + i));
    }
    util_id_allocator_uninit(&allocator);
}
