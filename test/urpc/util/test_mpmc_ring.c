/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: urpc MPMC lock-free ring buffer test (pure C standalone version)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>

#include "urpc_mpmc_ring.h"

#define TEST_CAPACITY 1024
#define TEST_SUCCESS   "\x1b[32mPASSED\x1b[0m"
#define TEST_FAIL      "\x1b[31mFAILED\x1b[0m"

struct test_item {
    uint64_t seq;
    uint64_t prod_id;
};

URPC_MPMC_RING_DECLARE(test_ring, struct test_item, TEST_CAPACITY);

static int g_failures = 0;

#define ASSERT_TRUE(cond) do {                       \
    if (!(cond)) {                                   \
        fprintf(stderr, "  %s:%d: " #cond "\n",      \
                __FILE__, __LINE__);                 \
        g_failures++;                                \
    }                                                \
} while (0)
#define ASSERT_FALSE(cond)   ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b)      ASSERT_TRUE((a) == (b))

#define RUN_TEST(name) do {                      \
    int _prev = g_failures;                      \
    printf("  %s ", #name);                      \
    fflush(stdout);                              \
    test_##name();                               \
    printf(g_failures == _prev ? TEST_SUCCESS : TEST_FAIL); \
    printf("\n");                                \
} while (0)

static void test_init_capacity(void)
{
    test_ring_t ring;
    URPC_MPMC_RING_INIT(&ring);
    ASSERT_EQ(URPC_MPMC_CAPACITY(&ring), (uint32_t)TEST_CAPACITY);
    ASSERT_TRUE(URPC_MPMC_IS_EMPTY(&ring));
    ASSERT_FALSE(URPC_MPMC_IS_FULL(&ring));
    ASSERT_EQ(URPC_MPMC_COUNT(&ring), (uint32_t)0);
}

static void test_single_enqueue_dequeue(void)
{
    test_ring_t ring;
    URPC_MPMC_RING_INIT(&ring);
    struct test_item in = { .seq = 42, .prod_id = 0 };
    struct test_item out;
    memset(&out, 0, sizeof(out));
    ASSERT_TRUE(URPC_MPMC_TRY_ENQUEUE(&ring, &in));
    ASSERT_EQ(URPC_MPMC_COUNT(&ring), (uint32_t)1);
    ASSERT_TRUE(URPC_MPMC_TRY_DEQUEUE(&ring, &out));
    ASSERT_EQ(out.seq, (uint64_t)42);
    ASSERT_TRUE(URPC_MPMC_IS_EMPTY(&ring));
}

static void test_fill_and_drain(void)
{
    test_ring_t ring;
    URPC_MPMC_RING_INIT(&ring);
    uint32_t i;
    for (i = 0; i < TEST_CAPACITY; i++) {
        struct test_item in = { .seq = i };
        ASSERT_TRUE(URPC_MPMC_TRY_ENQUEUE(&ring, &in));
    }
    ASSERT_TRUE(URPC_MPMC_IS_FULL(&ring));
    struct test_item overflow = { .seq = 999 };
    ASSERT_FALSE(URPC_MPMC_TRY_ENQUEUE(&ring, &overflow));
    uint32_t count = 0;
    struct test_item out;
    while (URPC_MPMC_TRY_DEQUEUE(&ring, &out)) {
        ASSERT_EQ(out.seq, (uint64_t)count);
        count++;
    }
    ASSERT_EQ(count, (uint32_t)TEST_CAPACITY);
    ASSERT_TRUE(URPC_MPMC_IS_EMPTY(&ring));
}

static void test_empty_dequeue(void)
{
    test_ring_t ring;
    URPC_MPMC_RING_INIT(&ring);
    struct test_item out;
    ASSERT_FALSE(URPC_MPMC_TRY_DEQUEUE(&ring, &out));
}

static void test_blocking_enqueue(void)
{
    test_ring_t ring;
    URPC_MPMC_RING_INIT(&ring);
    struct test_item in = { .seq = 1, .prod_id = 0 };
    URPC_MPMC_ENQUEUE(&ring, &in);
    ASSERT_EQ(URPC_MPMC_COUNT(&ring), (uint32_t)1);
}

static void test_mixed_enqueue_dequeue(void)
{
    test_ring_t ring;
    URPC_MPMC_RING_INIT(&ring);
    struct test_item out;
    uint32_t enqueued = 0, dequeued = 0, i;
    for (i = 0; i < 500; i++) {
        struct test_item in = { .seq = (uint64_t)i };
        ASSERT_TRUE(URPC_MPMC_TRY_ENQUEUE(&ring, &in));
        enqueued++;
    }
    for (i = 0; i < 300; i++) {
        ASSERT_TRUE(URPC_MPMC_TRY_DEQUEUE(&ring, &out));
        dequeued++;
    }
    ASSERT_EQ(URPC_MPMC_COUNT(&ring), enqueued - dequeued);
}

static void test_reset(void)
{
    test_ring_t ring;
    URPC_MPMC_RING_INIT(&ring);
    uint32_t i;
    for (i = 0; i < 100; i++) {
        struct test_item in = { .seq = i };
        URPC_MPMC_ENQUEUE(&ring, &in);
    }
    URPC_MPMC_RING_RESET(&ring);
    ASSERT_TRUE(URPC_MPMC_IS_EMPTY(&ring));
    ASSERT_EQ(URPC_MPMC_COUNT(&ring), (uint32_t)0);
}

/* ════════════════════════════════════════════════════════════════
 * Multi-threaded
 * ════════════════════════════════════════════════════════════════ */

static uint64_t g_seq;

struct prod_arg { test_ring_t *ring; uint32_t ops; uint64_t id; };
struct cons_arg { test_ring_t *ring; uint32_t ops; uint64_t *out; };

static void *producer_main(void *arg)
{
    struct prod_arg *a = (struct prod_arg *)arg;
    uint32_t i;
    for (i = 0; i < a->ops; i++) {
        uint64_t s = __atomic_fetch_add(&g_seq, 1, __ATOMIC_RELAXED);
        struct test_item item = { .seq = s, .prod_id = a->id };
        URPC_MPMC_ENQUEUE(a->ring, &item);
    }
    return NULL;
}

static void *consumer_main(void *arg)
{
    struct cons_arg *a = (struct cons_arg *)arg;
    uint32_t count = 0, empty_spins = 0;
    while (count < a->ops) {
        struct test_item out;
        if (URPC_MPMC_TRY_DEQUEUE(a->ring, &out)) {
            a->out[count++] = out.seq;
            empty_spins = 0;
        } else if (++empty_spins > 1024) {
            sched_yield();
            empty_spins = 0;
        }
    }
    return NULL;
}

static int cmp_uint64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

static void test_multithreaded(void)
{
    const uint32_t kP = 4, kC = 4;
    const uint32_t kOpsPerP = 1000;
    const uint32_t kTotal = kP * kOpsPerP;
    const uint32_t kOpsPerC = kTotal / kC;

    g_seq = 0;
    test_ring_t *ring = (test_ring_t *)malloc(sizeof(test_ring_t));
    assert(ring != NULL);
    URPC_MPMC_RING_INIT(ring);

    pthread_t pt[4], ct[4];
    struct prod_arg pa[4];
    struct cons_arg ca[4];
    uint64_t *results[4] = { NULL };

    uint32_t i;
    for (i = 0; i < kP; i++) {
        pa[i].ring = ring; pa[i].ops = kOpsPerP; pa[i].id = (uint64_t)i;
        pthread_create(&pt[i], NULL, producer_main, &pa[i]);
    }
    for (i = 0; i < kC; i++) {
        results[i] = (uint64_t *)malloc(kOpsPerC * sizeof(uint64_t));
        assert(results[i] != NULL);
        ca[i].ring = ring; ca[i].ops = kOpsPerC; ca[i].out = results[i];
        pthread_create(&ct[i], NULL, consumer_main, &ca[i]);
    }
    for (i = 0; i < kP; i++) pthread_join(pt[i], NULL);
    for (i = 0; i < kC; i++) pthread_join(ct[i], NULL);

    uint64_t *all = (uint64_t *)malloc(kTotal * sizeof(uint64_t));
    assert(all != NULL);
    uint32_t pos = 0;
    for (i = 0; i < kC; i++) {
        uint32_t j;
        for (j = 0; j < kOpsPerC; j++) all[pos++] = results[i][j];
        free(results[i]);
    }
    qsort(all, kTotal, sizeof(uint64_t), cmp_uint64);
    for (i = 0; i < kTotal; i++) ASSERT_EQ(all[i], (uint64_t)i);

    free(all);
    free(ring);
}

int main(void)
{
    printf("MPMC Ring test (pure C)\n");
    RUN_TEST(init_capacity);
    RUN_TEST(single_enqueue_dequeue);
    RUN_TEST(fill_and_drain);
    RUN_TEST(empty_dequeue);
    RUN_TEST(blocking_enqueue);
    RUN_TEST(mixed_enqueue_dequeue);
    RUN_TEST(reset);
    RUN_TEST(multithreaded);
    printf("\n%d failures\n", g_failures);
    return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
