/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: urpc util test
 */
#include "gtest/gtest.h"
#include "urpc_util.h"
#include "urpc_slab.h"
#include "urpc_bitmap.h"
#include "urpc_id_generator.h"

TEST(UrpcUtilTest, TestBitmap) {
    urpc_bitmap_t bitmap = urpc_bitmap_alloc(1024);
    urpc_bitmap_set(bitmap, 1023, true);
    ASSERT_EQ(urpc_bitmap_is_set(bitmap, 1023), true);

    urpc_bitmap_set(bitmap, 1023, false);
    ASSERT_EQ(urpc_bitmap_is_set(bitmap, 1023), false);

    urpc_bitmap_free(bitmap);
}

TEST(UrpcUtilTest, TestIdGenerator) {
    urpc_id_generator_t id_gen;
    urpc_id_generator_init(&id_gen, URPC_ID_GENERATOR_TYPE_BITMAP, 1024);

    uint32_t id;
    urpc_id_generator_alloc(&id_gen, 0, &id);
    ASSERT_EQ((uint32_t)0, id);
    urpc_id_generator_alloc(&id_gen, 0, &id);
    ASSERT_EQ((uint32_t)1, id);
    urpc_id_generator_alloc(&id_gen, 0, &id);
    ASSERT_EQ((uint32_t)2, id);
    urpc_id_generator_alloc(&id_gen, 1023, &id);
    ASSERT_EQ((uint32_t)1023, id);
    int ret = urpc_id_generator_alloc(&id_gen, 1024, &id);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(id, (uint32_t)3);

    ret = urpc_id_generator_alloc(&id_gen, -1, &id);
    ASSERT_EQ(ret, -ENOSPC);

    urpc_id_generator_uninit(&id_gen);
}

TEST(UrpcUtilTest, tokenTest) {
    uint32_t ret1 = get_timestamp();
    ASSERT_NE(ret1, (uint32_t)0);
    sleep(1);
    uint32_t ret2 = get_timestamp();
    ASSERT_NE(ret2, (uint32_t)0);
    ASSERT_NE(ret1, ret2);
    ret1 = get_timestamp();
    ASSERT_NE(ret1, (uint32_t)0);
    sleep(1);
    ret2 = get_timestamp();
    ASSERT_NE(ret2, (uint32_t)0);
    ASSERT_NE(ret1, ret2);
}

TEST(UrpcUtilTest, TestCPUCycleAndHz)
{
    int test_round = 1000000;
    uint64_t timestamp = get_timestamp_ns();
    uint64_t cpu_cycle = urpc_get_cpu_cycles();

    for (int i = 0; i < test_round; i++) {
        (void)urpc_get_cpu_cycles();
    }

    timestamp = get_timestamp_ns() - timestamp;
    cpu_cycle = urpc_get_cpu_cycles() - cpu_cycle;

    uint64_t cpu_freq = urpc_get_cpu_hz();
    uint64_t cpu_time = (uint64_t)((double)cpu_cycle * NS_PER_SEC / cpu_freq);
    uint64_t delta = cpu_time > timestamp ? cpu_time - timestamp : timestamp - cpu_time;
    delta = (uint64_t)((double)delta * 1000 / timestamp);

    printf("cpu hz is %lu, test %d round cost %lu ns, cpu time is %lu ns\n", cpu_freq, test_round, timestamp, cpu_time);

    // expect error less than 0.1%
    EXPECT_LE(delta, (uint64_t)1);
}

TEST(UrpcUtilTest, TestEslabAlloc)
{
    eslab_t slab;
    uint32_t *addr = (uint32_t *)malloc(sizeof(uint32_t));
    *addr = 2;
    uint32_t id;
    pthread_spin_init(&slab.lock, 0);
    slab.next_free = 0;
    slab.total = 1;
    slab.obj_size = sizeof(uint32_t);
    slab.addr = (void *)addr;
    void *ret = eslab_alloc(&slab, &id);
    ASSERT_EQ(ret, (void *)NULL);
    ASSERT_EQ(errno, URPC_ERR_EPERM);
    free(addr);
    pthread_spin_destroy(&slab.lock);
}
