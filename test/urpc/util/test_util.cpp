/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: urpc util test
 */

#include "gtest/gtest.h"

#include "urpc_bitmap.h"
#include "urpc_id_generator.h"
#include "urpc_pool.h"
#include "urpc_slab.h"
#include "urpc_util.h"

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

#define TEST_CTX_POOL_ELEM_SIZE 4096

static void *test_ctx_pool_job_func(void *arg)
{
    int cnt0 = 0, cnt = 0;
    urpc_pool_t *pool = (urpc_pool_t *)arg;
    void *elem[TEST_CTX_POOL_ELEM_SIZE] = {0};

    for (int i = 0; i < TEST_CTX_POOL_ELEM_SIZE; i++) {
        elem[i] = urpc_pool_element_get(pool);
        if (elem[i] != NULL) {
            memset(elem[i], 0, 128);
            cnt++;
        } else {
            cnt0++;
        }
    }

    printf("thread get element null cnt %d, not null cnt %d\n", cnt0, cnt);

    for (int i = 0; i < TEST_CTX_POOL_ELEM_SIZE; i++) {
        if (elem[i] != NULL) {
            urpc_pool_element_put(pool, elem[i]);
        }
    }

    return NULL;
}

static struct {
    void *elem;
    pthread_mutex_t lock;
    bool produced;
    volatile bool quit;
} g_test_ctx_pool = {
    .elem = nullptr,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .produced = false,
    .quit = false,
};

static void *test_ctx_pool_release_func(void *arg)
{
    urpc_pool_t *pool = (urpc_pool_t *)arg;
    while (!g_test_ctx_pool.quit) {
        (void)pthread_mutex_lock(&g_test_ctx_pool.lock);
        if (g_test_ctx_pool.produced) {
            urpc_pool_element_put(pool, g_test_ctx_pool.elem);
            g_test_ctx_pool.produced = false;
        }
        (void)pthread_mutex_unlock(&g_test_ctx_pool.lock);
    }

    return NULL;
}

TEST(UrpcUtilTest, TestCtxPool)
{
    urpc_pool_config_t cfg;
    cfg.element_size = 128;
    cfg.element_num_per_block = TEST_CTX_POOL_ELEM_SIZE;
    cfg.block_num = 4;
    urpc_pool_t pool;
    pthread_t thread;
    void *elem[TEST_CTX_POOL_ELEM_SIZE] = {0};
    int ret, cnt0 = 0, cnt = 0;

    ret = urpc_pool_init(&cfg, &pool);
    ASSERT_EQ(ret, 0);

    // test concurrency get element
    ret = pthread_create(&thread, NULL, test_ctx_pool_job_func, (void *)&pool);
    ASSERT_EQ(ret, 0);

    for (int i = 0; i < TEST_CTX_POOL_ELEM_SIZE; i++) {
        elem[i] = urpc_pool_element_get(&pool);
        if (elem[i] != NULL) {
            memset(elem[i], 0, 128);
            cnt++;
        } else {
            cnt0++;
        }
    }

    printf("main get element null cnt %d, not null cnt %d\n", cnt0, cnt);

    for (int i = 0; i < TEST_CTX_POOL_ELEM_SIZE; i++) {
        if (elem[i] != NULL) {
            urpc_pool_element_put(&pool, elem[i]);
        }
    }

    (void)pthread_join(thread, NULL);

    // test one thread get, another thread put, and get element exceed block num
    ret = pthread_create(&thread, NULL, test_ctx_pool_release_func, (void *)&pool);
    ASSERT_EQ(ret, 0);

    cnt = 0, cnt0 = 0;
    for (uint32_t i = 0; i < cfg.element_num_per_block * cfg.block_num + 1; i++) {
        void *e = urpc_pool_element_get(&pool);
        if (e != NULL) {
            (void)pthread_mutex_lock(&g_test_ctx_pool.lock);
            g_test_ctx_pool.elem = e;
            g_test_ctx_pool.produced = true;
            (void)pthread_mutex_unlock(&g_test_ctx_pool.lock);
            cnt++;
        } else {
            cnt0++;
        }
    }

    g_test_ctx_pool.quit = true;
    (void)pthread_join(thread, NULL);

    printf("main get element null cnt %d, not null cnt %d\n", cnt0, cnt);

    urpc_pool_thread_closure(0);
    urpc_pool_uninit(&pool);
}

typedef struct {
    uint64_t start_cycles;
    uint64_t start_ns;
    uint64_t delay_us;
    volatile bool ready;
    volatile bool done;
    uint64_t end_cycles;
    uint64_t end_ns;
} cross_thread_ctx_t;

static void *cross_thread_worker(void *arg)
{
    cross_thread_ctx_t *ctx = (cross_thread_ctx_t *)arg;

    ctx->start_cycles = urpc_get_cpu_cycles();
    ctx->start_ns = get_timestamp_ns();

    usleep(ctx->delay_us);

    ctx->end_cycles = urpc_get_cpu_cycles();
    ctx->end_ns = get_timestamp_ns();
    ctx->done = true;

    return NULL;
}

TEST(UrpcUtilTest, TestCPUCycleCrossThreadAccuracy)
{
    uint64_t hz = urpc_get_cpu_hz();
    ASSERT_NE(hz, (uint64_t)0);

    uint64_t delay_values_us[] = {1000, 10000, 100000, 1000000};

    for (int round = 0; round < (int)(sizeof(delay_values_us) / sizeof(delay_values_us[0])); round++) {
        cross_thread_ctx_t ctx = {0};
        ctx.delay_us = delay_values_us[round];
        ctx.ready = false;
        ctx.done = false;

        pthread_t tid;
        int ret = pthread_create(&tid, NULL, cross_thread_worker, &ctx);
        ASSERT_EQ(ret, 0);

        while (!ctx.done) {
            usleep(100);
        }

        (void)pthread_join(tid, NULL);

        uint64_t cycles_diff = ctx.end_cycles - ctx.start_cycles;
        uint64_t cycles_time_us = cycles_diff * US_PER_SEC / hz;
        uint64_t ns_diff = ctx.end_ns - ctx.start_ns;
        uint64_t ns_time_us = ns_diff / NS_PER_US;

        uint64_t delta_us = cycles_time_us > ns_time_us
            ? cycles_time_us - ns_time_us
            : ns_time_us - cycles_time_us;

        double error_pct = (double)delta_us * 100.0 / (double)ns_time_us;

        printf("delay=%luus, cycles_time=%luus, ns_time=%luus, delta=%luus, error=%.2f%%\n",
               ctx.delay_us, cycles_time_us, ns_time_us, delta_us, error_pct);

        EXPECT_LT(error_pct, 1.0);
    }
}

typedef struct {
    uint64_t cycles_on_thread_a;
    volatile bool a_written;
} cross_thread_compare_ctx_t;

static void *thread_a_func(void *arg)
{
    cross_thread_compare_ctx_t *ctx = (cross_thread_compare_ctx_t *)arg;
    ctx->cycles_on_thread_a = urpc_get_cpu_cycles();
    ctx->a_written = true;
    usleep(500000);
    return NULL;
}

TEST(UrpcUtilTest, TestCPUCycleCrossThreadMonotonicity)
{
    uint64_t hz = urpc_get_cpu_hz();
    ASSERT_NE(hz, (uint64_t)0);

    cross_thread_compare_ctx_t ctx = {0};
    ctx.a_written = false;

    pthread_t tid;
    int ret = pthread_create(&tid, NULL, thread_a_func, &ctx);
    ASSERT_EQ(ret, 0);

    while (!ctx.a_written) {
        usleep(10);
    }

    usleep(1000);

    uint64_t cycles_on_thread_b = urpc_get_cpu_cycles();
    uint64_t diff_cycles = cycles_on_thread_b - ctx.cycles_on_thread_a;
    uint64_t diff_us = diff_cycles * US_PER_SEC / hz;

    printf("thread_a cycles=%lu, thread_b cycles=%lu, diff=%lu cycles (~%luus)\n",
           ctx.cycles_on_thread_a, cycles_on_thread_b, diff_cycles, diff_us);

    EXPECT_GT(diff_cycles, (uint64_t)0);

    uint64_t ns_a = get_timestamp_ns();
    usleep(1000);
    uint64_t ns_b = get_timestamp_ns();
    uint64_t ns_diff_us = (ns_b - ns_a) / NS_PER_US;

    uint64_t cycles_a = urpc_get_cpu_cycles();
    usleep(1000);
    uint64_t cycles_b = urpc_get_cpu_cycles();
    uint64_t cycles_diff_us = (cycles_b - cycles_a) * US_PER_SEC / hz;

    uint64_t delta = cycles_diff_us > ns_diff_us
        ? cycles_diff_us - ns_diff_us
        : ns_diff_us - cycles_diff_us;
    double error_pct = (double)delta * 100.0 / (double)ns_diff_us;

    printf("same-thread: ns_diff=%luus, cycles_diff=%luus, error=%.2f%%\n",
           ns_diff_us, cycles_diff_us, error_pct);

    EXPECT_LT(error_pct, 1.0);

    (void)pthread_join(tid, NULL);
}
