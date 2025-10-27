/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: umq perftest latency test case
 * Create: 2025-8-29
 */

#include <math.h>
#include <unistd.h>

#include "ub_get_clock.h"

#include "umq_api.h"
#include "umq_pro_api.h"

#include "perftest_latency.h"
#include "umq_perftest_latency.h"

#define LATENCY_MEASURE_TAIL 2

perftest_latency_ctx_t g_perftest_latency_ctx = {0};

perftest_latency_ctx_t *get_perftest_latency_ctx(void)
{
    return &g_perftest_latency_ctx;
}

static void umq_perftest_client_run_latency_finish(umq_perftest_latency_arg_t *lat_arg)
{
    LOG_PRINT("--------------------------------latency info---------------------------------\n");
    perftest_calculate_latency(
        g_perftest_latency_ctx.cycles, g_perftest_latency_ctx.iters, lat_arg->cfg->config.size);

    free(g_perftest_latency_ctx.cycles);
    g_perftest_latency_ctx.cycles = NULL;
}

static void umq_perftest_server_run_latency_base(uint64_t umqh, umq_perftest_latency_arg_t *lat_arg)
{
    umq_buf_t *polled_buf = NULL;
    umq_buf_t *bad_buf = NULL;
    while (g_perftest_latency_ctx.iters < DEFAULT_LAT_TEST_ROUND && !perftest_get_status()) {
        // 接收请求
        do {
            polled_buf = umq_dequeue(umqh);
        } while (polled_buf == NULL);
        umq_buf_free(polled_buf);

        // 发送返回
        umq_buf_t *resp_buf = umq_buf_alloc(lat_arg->cfg->config.size, 1, UMQ_INVALID_HANDLE, NULL);
        if (resp_buf == NULL) {
            LOG_PRINT("alloc buf failed\n");
            goto FINISH;
        }
        if (umq_enqueue(umqh, resp_buf, &bad_buf) != UMQ_SUCCESS) {
            LOG_PRINT("enqueue failed\n");
            if (bad_buf != NULL) {
                umq_buf_free(bad_buf);
            }
            goto FINISH;
        }

        g_perftest_latency_ctx.iters++;
    }

FINISH:
    perftest_force_quit();
}

static void umq_perftest_client_run_latency_base(uint64_t umqh, umq_perftest_latency_arg_t *lat_arg)
{
    uint64_t start_cycle = 0;
    uint64_t end_cycle = 0;
    g_perftest_latency_ctx.cycles = (uint64_t *)malloc(sizeof(uint64_t) * DEFAULT_LAT_TEST_ROUND);
    if (g_perftest_latency_ctx.cycles == NULL) {
        LOG_PRINT("alloc cycles failed\n");
        return;
    }

    umq_buf_t *polled_buf;
    umq_buf_t *bad_buf = NULL;
    while (g_perftest_latency_ctx.iters < DEFAULT_LAT_TEST_ROUND && !perftest_get_status()) {
        umq_buf_t *req_buf = umq_buf_alloc(lat_arg->cfg->config.size, 1, UMQ_INVALID_HANDLE, NULL);
        if (req_buf == NULL) {
            LOG_PRINT("alloc buf failed\n");
            goto FINISH;
        }

        // 发送请求
        start_cycle = get_cycles();
        if (umq_enqueue(umqh, req_buf, &bad_buf) != UMQ_SUCCESS) {
            bad_buf = NULL;
            LOG_PRINT("umq_enqueue failed\n");
            goto FINISH;
        }

        // 接收返回
        do {
            polled_buf = umq_dequeue(umqh);
        } while (polled_buf == NULL);

        umq_buf_free(polled_buf);
        end_cycle = get_cycles();
        g_perftest_latency_ctx.cycles[g_perftest_latency_ctx.iters++] = end_cycle - start_cycle;
    }

FINISH:
    umq_perftest_client_run_latency_finish(lat_arg);
    perftest_force_quit();
}

static void umq_perftest_server_run_latency_pro(uint64_t umqh, umq_perftest_latency_arg_t *lat_arg)
{
    // 准备返回数据
    umq_buf_t *resp_buf = umq_buf_alloc(lat_arg->cfg->config.size, 1, UMQ_INVALID_HANDLE, NULL);
    if (resp_buf == NULL) {
        LOG_PRINT("alloc buf failed\n");
        return;
    }

    umq_buf_t *tmp = resp_buf;
    while (tmp) {
        umq_buf_pro_t *pro = (umq_buf_pro_t *)tmp->qbuf_ext;
        pro->flag.bs.solicited_enable = 1;
        pro->flag.bs.complete_enable = 1;
        pro->opcode = UMQ_OPC_SEND;
        tmp = tmp->qbuf_next;
    }

    umq_buf_t *polled_buf[UMQ_BATCH_SIZE];
    umq_buf_t *bad_buf = NULL;
    uint32_t require_rx_cnt = 0;
    uint32_t send_cnt = 0;
    uint32_t recv_cnt = 0;
    while (g_perftest_latency_ctx.iters < DEFAULT_LAT_TEST_ROUND && !perftest_get_status()) {
        recv_cnt = 0;

        // 接收请求，并释放rx
        do {
            int ret = umq_poll(umqh, UMQ_IO_RX, polled_buf, 1);
            if (ret < 0) {
                LOG_PRINT("poll rx failed\n");
                goto FINISH;
            }

            recv_cnt += (uint32_t)ret;
            require_rx_cnt += (uint32_t)ret;
            for (int i = 0; i < ret; ++i) {
                umq_buf_free(polled_buf[i]);
            }
        } while (recv_cnt < 1);

        // 发送返回
        if (umq_post(umqh, resp_buf, UMQ_IO_TX, &bad_buf) != UMQ_SUCCESS) {
            LOG_PRINT("post tx failed\n");
            goto FINISH;
        }

        // 填充rx
        if (require_rx_cnt >= UMQ_BATCH_SIZE) {
            umq_buf_t *rx_buf = umq_buf_alloc(lat_arg->cfg->config.size, UMQ_BATCH_SIZE, UMQ_INVALID_HANDLE, NULL);
            if (rx_buf == NULL) {
                LOG_PRINT("alloc buf failed\n");
                goto FINISH;
            }

            if (umq_post(umqh, rx_buf, UMQ_IO_RX, &bad_buf) != UMQ_SUCCESS) {
                LOG_PRINT("post rx failed\n");
                umq_buf_free(bad_buf);
                bad_buf = NULL;
                goto FINISH;
            }
            require_rx_cnt -= UMQ_BATCH_SIZE;
        }

        // poll返回的tx cqe。tx buffer复用，无需释放
        send_cnt = 0;
        do {
            int ret = umq_poll(umqh, UMQ_IO_TX, polled_buf, 1);
            if (ret < 0) {
                LOG_PRINT("umq_poll failed\n");
                goto FINISH;
            }

            send_cnt += (uint32_t)ret;
        } while (send_cnt != 1);

        g_perftest_latency_ctx.iters++;
    }

FINISH:
    umq_buf_free(resp_buf);
    perftest_force_quit();
}

static void umq_perftest_client_run_latency_pro(uint64_t umqh, umq_perftest_latency_arg_t *lat_arg)
{
    uint64_t start_cycle = 0;
    g_perftest_latency_ctx.cycles = (uint64_t *)malloc(sizeof(uint64_t) * DEFAULT_LAT_TEST_ROUND);
    if (g_perftest_latency_ctx.cycles == NULL) {
        LOG_PRINT("alloc cycles failed\n");
        return;
    }

    // 准备请求数据。请求的tx buffer复用
    umq_buf_t *req_buf = umq_buf_alloc(lat_arg->cfg->config.size, 1, UMQ_INVALID_HANDLE, NULL);
    if (req_buf == NULL) {
        free(g_perftest_latency_ctx.cycles);
        LOG_PRINT("alloc buf failed\n");
        return;
    }

    umq_buf_t *tmp = req_buf;
    while (tmp) {
        umq_buf_pro_t *pro = (umq_buf_pro_t *)tmp->qbuf_ext;
        pro->flag.bs.solicited_enable = 1;
        pro->flag.bs.complete_enable = 1;
        pro->opcode = UMQ_OPC_SEND;
        tmp = tmp->qbuf_next;
    }

    umq_buf_t *polled_buf[UMQ_BATCH_SIZE];
    umq_buf_t *bad_buf = NULL;
    uint32_t require_rx_cnt = 0;
    uint32_t send_cnt = 0;
    uint32_t recv_cnt = 0;
    while (g_perftest_latency_ctx.iters < DEFAULT_LAT_TEST_ROUND && !perftest_get_status()) {
        send_cnt = 0;
        // 发送请求
        start_cycle = get_cycles();
        if (umq_post(umqh, req_buf, UMQ_IO_TX, &bad_buf) != UMQ_SUCCESS) {
            LOG_PRINT("post tx failed\n");
            goto FINISH;
        }

        // 填充rx
        if (require_rx_cnt >= UMQ_BATCH_SIZE) {
            umq_buf_t *rx_buf = umq_buf_alloc(lat_arg->cfg->config.size, UMQ_BATCH_SIZE, UMQ_INVALID_HANDLE, NULL);
            if (rx_buf == NULL) {
                LOG_PRINT("alloc buf failed\n");
                goto FINISH;
            }

            if (umq_post(umqh, rx_buf, UMQ_IO_RX, &bad_buf) != UMQ_SUCCESS) {
                LOG_PRINT("post rx failed\n");
                umq_buf_free(bad_buf);
                goto FINISH;
            }
            require_rx_cnt -= UMQ_BATCH_SIZE;
        }

        // poll返回的tx cqe。 tx buffer复用，无需释放
        do {
            int ret = umq_poll(umqh, UMQ_IO_TX, polled_buf, 1);
            if (ret < 0) {
                LOG_PRINT("poll tx failed\n");
                goto FINISH;
            }

            send_cnt += (uint32_t)ret;
        } while (send_cnt != 1);

        // 接收返回，并释放rx
        recv_cnt = 0;
        do {
            int ret = umq_poll(umqh, UMQ_IO_RX, polled_buf, 1);
            if (ret < 0) {
                LOG_PRINT("poll rx failed\n");
                goto FINISH;
            }

            recv_cnt += (uint32_t)ret;
            require_rx_cnt += (uint32_t)ret;
            for (int i = 0; i < ret; ++i) {
                umq_buf_free(polled_buf[i]);
            }
        } while (recv_cnt < 1);

        g_perftest_latency_ctx.cycles[g_perftest_latency_ctx.iters++] = get_cycles() - start_cycle;
    }

FINISH:
    umq_perftest_client_run_latency_finish(lat_arg);
    umq_buf_free(req_buf);
    perftest_force_quit();
}

void umq_perftest_run_latency(uint64_t umqh, umq_perftest_latency_arg_t *lat_arg)
{
    if (lat_arg->cfg->config.instance_mode == PERF_INSTANCE_SERVER) {
        if (lat_arg->cfg->feature & UMQ_FEATURE_API_PRO) {
            umq_perftest_server_run_latency_pro(umqh, lat_arg);
        } else {
            umq_perftest_server_run_latency_base(umqh, lat_arg);
        }
    } else {
        if (lat_arg->cfg->feature & UMQ_FEATURE_API_PRO) {
            umq_perftest_client_run_latency_pro(umqh, lat_arg);
        } else {
            umq_perftest_client_run_latency_base(umqh, lat_arg);
        }
    }
}
