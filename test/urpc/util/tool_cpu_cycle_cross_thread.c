#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#include "urpc_util.h"

#define US_PER_SEC 1000000
#define NS_PER_SEC 1000000000UL
#define NS_PER_US 1000

typedef struct {
    uint64_t start_cycles;
    uint64_t end_cycles;
    uint64_t start_ns;
    uint64_t end_ns;
    uint64_t delay_us;
    volatile int done;
} worker_ctx_t;

static void *worker_fn(void *arg)
{
    worker_ctx_t *ctx = (worker_ctx_t *)arg;
    ctx->start_cycles = urpc_get_cpu_cycles();
    ctx->start_ns = get_timestamp_ns();
    usleep(ctx->delay_us);
    ctx->end_cycles = urpc_get_cpu_cycles();
    ctx->end_ns = get_timestamp_ns();
    ctx->done = 1;
    return NULL;
}

static void test_cross_thread_accuracy(void)
{
    uint64_t hz = urpc_get_cpu_hz();
    printf("=== Cross Thread Accuracy Test ===\n");
    printf("CPU Hz: %lu\n", hz);

    uint64_t delays[] = {1000, 10000, 100000, 1000000, 5000000};
    int num = sizeof(delays) / sizeof(delays[0]);

    printf("%-12s %-14s %-14s %-10s %-10s\n",
           "Delay(us)", "Cycles(us)", "Ns(us)", "Delta(us)", "Error(%)");

    for (int i = 0; i < num; i++) {
        worker_ctx_t ctx = {0};
        ctx.delay_us = delays[i];
        ctx.done = 0;

        pthread_t tid;
        pthread_create(&tid, NULL, worker_fn, &ctx);
        while (!ctx.done) {
            usleep(100);
        }
        pthread_join(tid, NULL);

        uint64_t cycles_diff = ctx.end_cycles - ctx.start_cycles;
        uint64_t cycles_us = cycles_diff * US_PER_SEC / hz;
        uint64_t ns_diff = ctx.end_ns - ctx.start_ns;
        uint64_t ns_us = ns_diff / NS_PER_US;

        uint64_t delta = cycles_us > ns_us ? cycles_us - ns_us : ns_us - cycles_us;
        double err_pct = (double)delta * 100.0 / (double)ns_us;

        printf("%-12lu %-14lu %-14lu %-10lu %-8.2f\n",
               delays[i], cycles_us, ns_us, delta, err_pct);
    }
}

typedef struct {
    uint64_t cycles_a;
    volatile int written;
} compare_ctx_t;

static void *thread_a_fn(void *arg)
{
    compare_ctx_t *ctx = (compare_ctx_t *)arg;
    ctx->cycles_a = urpc_get_cpu_cycles();
    ctx->written = 1;
    usleep(500000);
    return NULL;
}

static void test_cross_thread_monotonicity(void)
{
    uint64_t hz = urpc_get_cpu_hz();
    printf("\n=== Cross Thread Monotonicity Test ===\n");

    compare_ctx_t ctx = {0};
    ctx.written = 0;
    pthread_t tid;
    pthread_create(&tid, NULL, thread_a_fn, &ctx);

    while (!ctx.written) {
        usleep(10);
    }
    usleep(1000);

    uint64_t cycles_b = urpc_get_cpu_cycles();
    uint64_t diff = cycles_b - ctx.cycles_a;
    uint64_t diff_us = diff * US_PER_SEC / hz;

    printf("Thread A cycles: %lu\n", ctx.cycles_a);
    printf("Thread B cycles: %lu (1ms after A)\n", cycles_b);
    printf("Diff: %lu cycles (~%luus)\n", diff, diff_us);
    printf("Monotonic: %s\n", diff > 0 ? "PASS" : "FAIL");

    pthread_join(tid, NULL);
}

static void test_same_thread_accuracy(void)
{
    uint64_t hz = urpc_get_cpu_hz();
    printf("\n=== Same Thread Baseline Test ===\n");
    printf("%-12s %-14s %-14s %-10s %-10s\n",
           "Delay(us)", "Cycles(us)", "Ns(us)", "Delta(us)", "Error(%)");

    uint64_t delays[] = {1000, 10000, 100000, 1000000, 5000000};
    int num = sizeof(delays) / sizeof(delays[0]);

    for (int i = 0; i < num; i++) {
        uint64_t c_start = urpc_get_cpu_cycles();
        uint64_t n_start = get_timestamp_ns();
        usleep(delays[i]);
        uint64_t c_end = urpc_get_cpu_cycles();
        uint64_t n_end = get_timestamp_ns();

        uint64_t cycles_us = (c_end - c_start) * US_PER_SEC / hz;
        uint64_t ns_us = (n_end - n_start) / NS_PER_US;
        uint64_t delta = cycles_us > ns_us ? cycles_us - ns_us : ns_us - cycles_us;
        double err_pct = (double)delta * 100.0 / (double)ns_us;

        printf("%-12lu %-14lu %-14lu %-10lu %-8.2f\n",
               delays[i], cycles_us, ns_us, delta, err_pct);
    }
}

int main(int argc, char *argv[])
{
    printf("CPU Cycle Cross-Thread Timing Verification Tool\n");
    printf("================================================\n\n");

    test_same_thread_accuracy();
    test_cross_thread_accuracy();
    test_cross_thread_monotonicity();

    printf("\nDone.\n");
    return 0;
}
