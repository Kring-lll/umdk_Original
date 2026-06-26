/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: urpc MPMC lock-free ring buffer test
 */

#include "gtest/gtest.h"
#include "urpc_mpmc_ring.h"

#include <pthread.h>
#include <vector>
#include <set>
#include <algorithm>
#include <atomic>

#define TEST_RING_CAPACITY 1024

/* ── element type for pointer-based testing ── */

struct test_item {
    uint64_t seq;    // 唯一序号，验证去重
    uint64_t prod_id; // 生产者 id
};

URPC_MPMC_RING_DECLARE(test_ring, struct test_item, TEST_RING_CAPACITY);

/* ══════════════════════════════════════════════════════════════════════
 * 单线程基础测试
 * ══════════════════════════════════════════════════════════════════════ */

TEST(MpmcRingTest, InitCapacity)
{
    test_ring_t ring;
    URPC_MPMC_RING_INIT(&ring);

    ASSERT_EQ(URPC_MPMC_CAPACITY(&ring), (uint32_t)TEST_RING_CAPACITY);
    ASSERT_TRUE(URPC_MPMC_IS_EMPTY(&ring));
    ASSERT_FALSE(URPC_MPMC_IS_FULL(&ring));
    ASSERT_EQ(URPC_MPMC_COUNT(&ring), (uint32_t)0);
}

TEST(MpmcRingTest, SingleEnqueueDequeue)
{
    test_ring_t ring;
    URPC_MPMC_RING_INIT(&ring);

    struct test_item in = { .seq = 42, .prod_id = 0 };
    struct test_item out = {};

    ASSERT_TRUE(URPC_MPMC_TRY_ENQUEUE(&ring, &in));
    ASSERT_EQ(URPC_MPMC_COUNT(&ring), (uint32_t)1);
    ASSERT_FALSE(URPC_MPMC_IS_EMPTY(&ring));

    ASSERT_TRUE(URPC_MPMC_TRY_DEQUEUE(&ring, &out));
    ASSERT_EQ(out.seq, (uint64_t)42);
    ASSERT_EQ(out.prod_id, (uint64_t)0);
    ASSERT_TRUE(URPC_MPMC_IS_EMPTY(&ring));
}

TEST(MpmcRingTest, FillAndDrain)
{
    test_ring_t ring;
    URPC_MPMC_RING_INIT(&ring);

    // 填满
    for (uint32_t i = 0; i < TEST_RING_CAPACITY; i++) {
        struct test_item in = { .seq = i, .prod_id = 0 };
        ASSERT_TRUE(URPC_MPMC_TRY_ENQUEUE(&ring, &in));
    }
    ASSERT_TRUE(URPC_MPMC_IS_FULL(&ring));
    struct test_item overflow = { .seq = 999 };
    ASSERT_FALSE(URPC_MPMC_TRY_ENQUEUE(&ring, &overflow));

    // 全部读出
    uint32_t count = 0;
    struct test_item out;
    while (URPC_MPMC_TRY_DEQUEUE(&ring, &out)) {
        ASSERT_EQ(out.seq, (uint64_t)count);
        count++;
    }
    ASSERT_EQ(count, (uint32_t)TEST_RING_CAPACITY);
    ASSERT_TRUE(URPC_MPMC_IS_EMPTY(&ring));
}

TEST(MpmcRingTest, EmptyDequeueReturnsFalse)
{
    test_ring_t ring;
    URPC_MPMC_RING_INIT(&ring);

    struct test_item out;
    ASSERT_FALSE(URPC_MPMC_TRY_DEQUEUE(&ring, &out));
}

TEST(MpmcRingTest, BlockingEnqueue)
{
    test_ring_t ring;
    URPC_MPMC_RING_INIT(&ring);

    struct test_item in = { .seq = 1, .prod_id = 0 };
    URPC_MPMC_ENQUEUE(&ring, &in);  // 不满，直接入

    ASSERT_EQ(URPC_MPMC_COUNT(&ring), (uint32_t)1);
}

/* ══════════════════════════════════════════════════════════════════════
 * 多线程并发测试
 * ══════════════════════════════════════════════════════════════════════ */

static std::atomic<uint64_t> g_next_seq{0};

struct prod_args {
    test_ring_t *ring;
    uint32_t      ops;
    uint64_t      prod_id;
};

struct cons_args {
    test_ring_t *ring;
    uint32_t      ops;
    uint64_t     *out;  // 输出：收集到的 seq 数组
};

// ── 生产者线程：生成序列号，阻塞入队。 ──
static void *producer_thread(void *arg)
{
    struct prod_args *a = (struct prod_args *)arg;
    for (uint32_t i = 0; i < a->ops; i++) {
        uint64_t seq = g_next_seq.fetch_add(1, std::memory_order_relaxed);
        struct test_item item = { .seq = seq, .prod_id = a->prod_id };
        URPC_MPMC_ENQUEUE(a->ring, &item);
    }
    return NULL;
}

// ── 消费者线程：等待出队，收集 seq。 ──
static void *consumer_thread(void *arg)
{
    struct cons_args *a = (struct cons_args *)arg;
    uint32_t count = 0;

    while (count < a->ops) {
        struct test_item out;
        if (URPC_MPMC_TRY_DEQUEUE(a->ring, &out)) {
            a->out[count++] = out.seq;
        }
    }
    return NULL;
}

TEST(MpmcRingTest, MultiThreadedNoLossNoDuplication)
{
    const uint32_t kProducers   = 4;
    const uint32_t kConsumers   = 4;
    const uint32_t kOpsPerProd  = 50000;                // 每生产者 50k
    const uint32_t kTotalOps    = kProducers * kOpsPerProd;  // 200k total
    const uint32_t kOpsPerCons  = kTotalOps / kConsumers;

    g_next_seq.store(0, std::memory_order_relaxed);

    test_ring_t *ring = new test_ring_t;
    URPC_MPMC_RING_INIT(ring);

    pthread_t prod_thr[kProducers];
    pthread_t cons_thr[kConsumers];
    struct prod_args pargs[kProducers];
    struct cons_args cargs[kConsumers];
    uint64_t *results[kConsumers] = {};  // 每个消费者的收集数组

    // 启动生产者
    for (uint32_t i = 0; i < kProducers; i++) {
        pargs[i] = { ring, kOpsPerProd, (uint64_t)i };
        pthread_create(&prod_thr[i], NULL, producer_thread, &pargs[i]);
    }
    // 启动消费者
    for (uint32_t i = 0; i < kConsumers; i++) {
        results[i] = new uint64_t[kOpsPerCons];
        cargs[i] = { ring, kOpsPerCons, results[i] };
        pthread_create(&cons_thr[i], NULL, consumer_thread, &cargs[i]);
    }

    // 等待全部线程结束
    for (uint32_t i = 0; i < kProducers; i++) {
        pthread_join(prod_thr[i], NULL);
    }
    for (uint32_t i = 0; i < kConsumers; i++) {
        pthread_join(cons_thr[i], NULL);
    }

    // 收集所有 seq，验证无重复无丢失
    std::set<uint64_t> all_seqs;
    for (uint32_t i = 0; i < kConsumers; i++) {
        for (uint32_t j = 0; j < kOpsPerCons; j++) {
            all_seqs.insert(results[i][j]);
        }
        delete[] results[i];
    }

    ASSERT_EQ(all_seqs.size(), (size_t)kTotalOps)
        << "duplicates or lost elements detected";

    // 验证序号连续：0 .. N-1
    uint64_t expected = 0;
    for (auto s : all_seqs) {
        ASSERT_EQ(s, expected) << "missing seq at " << expected;
        expected++;
    }

    delete ring;
}

/* ══════════════════════════════════════════════════════════════════════
 * 边界场景
 * ══════════════════════════════════════════════════════════════════════ */

TEST(MpmcRingTest, RingReuseAfterFullDrain)
{
    test_ring_t ring;
    URPC_MPMC_RING_INIT(&ring);

    for (int cycle = 0; cycle < 10; cycle++) {
        // 填满
        for (uint32_t i = 0; i < TEST_RING_CAPACITY; i++) {
            struct test_item in = { .seq = (uint64_t)(cycle * TEST_RING_CAPACITY + i) };
            ASSERT_TRUE(URPC_MPMC_TRY_ENQUEUE(&ring, &in));
        }
        // 清空，只计数不计序（MPMC 不保证 FIFO）
        uint32_t drained = 0;
        struct test_item out;
        while (URPC_MPMC_TRY_DEQUEUE(&ring, &out)) {
            drained++;
        }
        ASSERT_EQ(drained, (uint32_t)TEST_RING_CAPACITY);
    }
    ASSERT_TRUE(URPC_MPMC_IS_EMPTY(&ring));
}

TEST(MpmcRingTest, MixedEnqueueDequeue)
{
    test_ring_t ring;
    URPC_MPMC_RING_INIT(&ring);

    struct test_item out;
    uint32_t enqueued = 0, dequeued = 0;

    // 交错操作：入队 500，出队 300
    for (uint32_t i = 0; i < 500; i++) {
        struct test_item in = { .seq = (uint64_t)i };
        ASSERT_TRUE(URPC_MPMC_TRY_ENQUEUE(&ring, &in));
        enqueued++;
    }
    for (uint32_t i = 0; i < 300; i++) {
        ASSERT_TRUE(URPC_MPMC_TRY_DEQUEUE(&ring, &out));
        dequeued++;
    }
    ASSERT_EQ(URPC_MPMC_COUNT(&ring), enqueued - dequeued);
}

TEST(MpmcRingTest, ResetClearsAll)
{
    test_ring_t ring;
    URPC_MPMC_RING_INIT(&ring);

    for (uint32_t i = 0; i < 100; i++) {
        struct test_item in = { .seq = i };
        URPC_MPMC_ENQUEUE(&ring, &in);
    }

    URPC_MPMC_RING_RESET(&ring);

    ASSERT_TRUE(URPC_MPMC_IS_EMPTY(&ring));
    ASSERT_EQ(URPC_MPMC_COUNT(&ring), (uint32_t)0);
    ASSERT_FALSE(URPC_MPMC_IS_FULL(&ring));
}
