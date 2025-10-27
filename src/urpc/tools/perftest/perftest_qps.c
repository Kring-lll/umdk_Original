/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: urpc lib perftest qps test case
 * Create: 2025-9-8
 */

#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

#include "ub_get_clock.h"
#include "urpc_util.h"
#include "perftest_util.h"
#include "perftest_qps.h"

#define QPS_MESSAGE_SIZE 4096

#define QPS_THREAD_FORMAT "       %-7.3lf"
#define QPS_FIRST_THREAD_FORMAT "     %-7.3lf"

static inline void perftest_reqs_get(uint64_t *reqs, perftest_qps_ctx_t *ctx, uint64_t length)
{
    for (uint32_t i = 1; i < ctx->thread_num + 1 && i < length; i++) {
        reqs[i] = atomic_load(&ctx->reqs[i]);
    }
}

static inline uint64_t urpc_perftest_reqs_sum_get(uint64_t *reqs, uint32_t worker_num)
{
    uint64_t total = 0;
    for (uint32_t i = 1; i < worker_num + 1 && i < PERFTEST_THREAD_MAX_NUM; i++) {
        total += reqs[i];
    }

    return total;
}

static void urpc_perftest_print_qps_title(perftest_qps_ctx_t *ctx)
{
    int s = 0;
    int ret;
    char title[QPS_MESSAGE_SIZE];

    if (!ctx->show_thread) {
        (void)printf("  qps[Mpps]    BW[MB/Sec]\n");
        return;
    }

    ret = sprintf(title, "  qps[Mpps]    BW[MB/Sec]");
    if (ret < 0) {
        LOG_PRINT("fail to get qps title string\n");
        ret = 0;
    }

    s += ret;
    for (uint32_t i = 1; i < ctx->thread_num + 1; i++) {
        ret = sprintf(title + s, "  %02u-qps[Mpps]", i);
        if (ret < 0) {
            LOG_PRINT("fail to get qps title string\n");
            ret = 0;
        }

        s += ret;
    }

    (void)printf("%s\n", title);
}

// 用cycle计算qps的精确时间, sleep仅用于定时输出结果
void perftest_print_qps(perftest_qps_ctx_t *ctx)
{
    char result[QPS_MESSAGE_SIZE];
    uint64_t reqs[PERFTEST_THREAD_MAX_NUM] = {0};
    uint64_t reqs_old[PERFTEST_THREAD_MAX_NUM] = {0};
    uint64_t cur_sum, end;
    double cycles_to_units = get_cpu_mhz(true);
    double qps;
    int ret;

    perftest_reqs_get(reqs_old, ctx, PERFTEST_THREAD_MAX_NUM);
    uint64_t reqs_num = urpc_perftest_reqs_sum_get(reqs_old, ctx->thread_num);
    uint64_t begin = get_cycles();

    urpc_perftest_print_qps_title(ctx);
    while (!perftest_get_status()) {
        (void)sleep(1);

        perftest_reqs_get(reqs, ctx, PERFTEST_THREAD_MAX_NUM);
        cur_sum = urpc_perftest_reqs_sum_get(reqs, ctx->thread_num);
        end = get_cycles();

        // show qps
        qps = (double)(cur_sum - reqs_num) * cycles_to_units / (double)(end - begin);

        int s = 0;
        ret = sprintf(result, "  %-7.6lf     %-7.3lf", qps, qps * PERFTEST_1M * ctx->size_total / PERFTEST_1MB);
        if (ret < 0) {
            LOG_PRINT("fail to get qps string\n");
            ret = 0;
        }

        s += ret;
        if (!ctx->show_thread) {
            goto OUTPUT;
        }

        for (uint32_t i = 1; i < ctx->thread_num + 1; i++) {
            ret = sprintf(result + s, i == 1 ? QPS_FIRST_THREAD_FORMAT : QPS_THREAD_FORMAT,
                (double)(reqs[i] - reqs_old[i]) * cycles_to_units / (double)(end - begin));
            if (ret < 0) {
                LOG_PRINT("fail to get worker thread qps string\n");
                ret = 0;
            }

            s += ret;
            reqs_old[i] = reqs[i];
        }

OUTPUT:
        (void)fflush(stdout);
        (void)printf("%s\n", result);
        reqs_num = cur_sum;
        begin = end;
    }
}
