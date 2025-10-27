/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: umq perftest qps test case
 * Create: 2025-8-27
 */

#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

#include "umq_api.h"
#include "umq_pro_api.h"
#include "perftest_util.h"
#include "perftest_qps.h"
#include "umq_perftest_qps.h"

#define QPS_PRINT_STR_LEN   (4096)
#define UMQ_PERFTEST_1M     (1000000)
#define UMQ_PERFTEST_1MB    (0x100000)

static perftest_qps_ctx_t g_umq_perftest_qps_ctx = {0};

perftest_qps_ctx_t *get_perftest_qps_ctx(void)
{
    return &g_umq_perftest_qps_ctx;
}

static void umq_perftest_server_run_qps_base(uint64_t umqh, umq_perftest_qps_arg_t *qps_arg)
{
    umq_buf_t *recv_buf = NULL;
    umq_buf_t *tmp_buf = NULL;
    uint32_t poll_num = 0;
    uint32_t thread_inx = perftest_thread_index();
    while (!perftest_get_status()) {
        // 接收请求，计数，然后释放请求
        recv_buf = umq_dequeue(umqh);
        if (recv_buf == NULL) {
            continue;
        }

        tmp_buf = recv_buf;
        poll_num = 0;
        while (tmp_buf) {
            poll_num++;
            tmp_buf = tmp_buf->qbuf_next;
        }

        umq_buf_free(recv_buf);
        (void)atomic_fetch_add(&g_umq_perftest_qps_ctx.reqs[thread_inx], poll_num);
    }
}

static void umq_perftest_client_run_qps_base(uint64_t umqh, umq_perftest_qps_arg_t *qps_arg)
{
    umq_buf_t *bad_buf = NULL;
    uint32_t thread_inx = perftest_thread_index();
    while (!perftest_get_status()) {
        // 申请buf，发送请求，每次发送64个wr。内部会在下一次enqueue时尝试poll tx，然后释放buf
        umq_buf_t *req_buf = umq_buf_alloc(qps_arg->cfg->config.size, UMQ_BATCH_SIZE, UMQ_INVALID_HANDLE, NULL);
        if (req_buf == NULL) {
            LOG_PRINT("alloc buf failed\n");
            return;
        }

        // 发送请求
        int32_t ret = -1;
        do {
            bad_buf = NULL;
            ret = umq_enqueue(umqh, req_buf, &bad_buf);
            if (perftest_get_status()) {
                return;
            }
            if (ret == -EAGAIN) {
                continue;
            }
            if (ret == UMQ_FAIL) {
                umq_buf_free(req_buf);
                return;
            }
        } while (ret != UMQ_SUCCESS);

        (void)atomic_fetch_add(&g_umq_perftest_qps_ctx.reqs[thread_inx], UMQ_BATCH_SIZE);
    }
}

static void umq_perftest_server_run_qps_pro(uint64_t umqh, umq_perftest_qps_arg_t *qps_arg)
{
    umq_buf_t *bad_buf = NULL;
    uint32_t require_rx_cnt = 0;
    int32_t poll_num = 0;
    umq_buf_t *polled_buf[UMQ_BATCH_SIZE];
    uint32_t thread_inx = perftest_thread_index();

    while (!perftest_get_status()) {
        // 接收请求，计数后释放请求buf
        poll_num = umq_poll(umqh, UMQ_IO_RX, polled_buf, UMQ_BATCH_SIZE);
        if (poll_num < 0) {
            LOG_PRINT("poll rx failed\n");
            return;
        }

        require_rx_cnt += (uint32_t)poll_num;
        for (int i = 0; i < poll_num; ++i) {
            umq_buf_free(polled_buf[i]);
        }
        (void)atomic_fetch_add(&g_umq_perftest_qps_ctx.reqs[thread_inx], poll_num);

        // 批量填充rx
        if (require_rx_cnt >= UMQ_BATCH_SIZE) {
            umq_buf_t *rx_buf = umq_buf_alloc(qps_arg->cfg->config.size, UMQ_BATCH_SIZE, UMQ_INVALID_HANDLE, NULL);
            if (rx_buf == NULL) {
                LOG_PRINT("alloc buf failed\n");
                return;
            }

            if (umq_post(umqh, rx_buf, UMQ_IO_RX, &bad_buf) != UMQ_SUCCESS) {
                LOG_PRINT("post rx failed\n");
                umq_buf_free(bad_buf);
                bad_buf = NULL;
                return;
            }
            require_rx_cnt -= UMQ_BATCH_SIZE;
        }
    }
}

static void umq_perftest_client_run_qps_pro(uint64_t umqh, umq_perftest_qps_arg_t *qps_arg)
{
    // 准备请求数据，请求数据复用
    umq_buf_t *req_buf = umq_buf_alloc(qps_arg->cfg->config.size, UMQ_BATCH_SIZE, UMQ_INVALID_HANDLE, NULL);
    if (req_buf == NULL) {
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

    umq_buf_t *bad_buf = NULL;
    uint32_t can_send_num = qps_arg->cfg->config.tx_depth;
    umq_buf_t *polled_buf[UMQ_BATCH_SIZE];
    uint32_t thread_inx = perftest_thread_index();
    while (!perftest_get_status()) {
        if (can_send_num >= UMQ_BATCH_SIZE) {
            // 当未打满tx深度时，发送请求
            if (umq_post(umqh, req_buf, UMQ_IO_TX, &bad_buf) != UMQ_SUCCESS) {
                LOG_PRINT("post tx failed\n");
                goto ERROR;
            }
            can_send_num -= UMQ_BATCH_SIZE;
        }

        // poll tx cqe，并增加计数
        int ret = umq_poll(umqh, UMQ_IO_TX, polled_buf, UMQ_BATCH_SIZE);
        if (ret < 0) {
            LOG_PRINT("poll tx failed\n");
            goto ERROR;
        }

        (void)atomic_fetch_add(&g_umq_perftest_qps_ctx.reqs[thread_inx], ret);
        can_send_num += (uint32_t)ret;
    }

    umq_buf_free(req_buf);
    return;

ERROR:
    umq_buf_free(req_buf);
    perftest_force_quit();
}

void umq_perftest_run_qps(uint64_t umqh, umq_perftest_qps_arg_t *qps_arg)
{
    if (qps_arg->cfg->config.instance_mode == PERF_INSTANCE_SERVER) {
        if (qps_arg->cfg->feature & UMQ_FEATURE_API_PRO) {
            umq_perftest_server_run_qps_pro(umqh, qps_arg);
        } else {
            umq_perftest_server_run_qps_base(umqh, qps_arg);
        }
    } else {
        if (qps_arg->cfg->feature & UMQ_FEATURE_API_PRO) {
            umq_perftest_client_run_qps_pro(umqh, qps_arg);
        } else {
            umq_perftest_client_run_qps_base(umqh, qps_arg);
        }
    }
}
