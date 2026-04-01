/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: umq example of pro feature functions
 * Create: 2025-8-16
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include "umq_example_common.h"
#include "umq_example_pro.h"

static const uint32_t EXAMPLE_MAX_DEPTH = 64;
static const char *EXAMPLE_CLIENT_POST_DATA = "hello, this is umq pro client";
static const char *EXAMPLE_SERVER_POST_DATA = "hello, this is umq pro server";

umq_buf_t *test_mem_buf(uint64_t umqh)
{   
    uint32_t req_block[] = {1, 16, 32, 64, 128}; // 
    int test_id = 0;
    umq_buf_t *buf_list[1000];
    uint32_t request_sizes[] = {0, 8192};
    while (test_id < 1000) {
        buf_list[0] = NULL;
        for (int i = 0; i < test_id; i++) {
            buf_list[i] = umq_buf_alloc(request_sizes[test_id % 2], req_block[i % (sizeof(req_block)/sizeof(uint32_t))], umqh, NULL);
            if (buf_list[i] == NULL) {
                LOG_PRINT_ERR("umq alloc buf failed\n");
                return NULL;
            }
        }
        umq_buf_t *tmp_buf;
        for (int i = 0; i < test_id - 1; i++) {
            tmp_buf = buf_list[i];
            LOG_PRINT_ERR("tmp_buf mempool_id %u, %u\n", tmp_buf->mempool_id, tmp_buf->mempool_id - 257);
            while (tmp_buf->qbuf_next != NULL) {
                tmp_buf = tmp_buf->qbuf_next;
            }
            tmp_buf->qbuf_next = buf_list[i + 1];
        }
        if (test_id != 0) {
            LOG_PRINT_ERR("tmp_buf mempool_id %u, %u, request_size %u\n",
                buf_list[test_id - 1]->mempool_id, buf_list[test_id - 1]->mempool_id - 257, request_sizes[test_id % 2]);
        }
        umq_buf_free(buf_list[0]);
        test_id += 53;
    }
    return buf_list[0];
}

int run_umq_example_pro_server(struct urpc_example_config *cfg)
{
    int ret = -1;
    uint32_t local_bind_info_size = UMQ_MAX_BIND_INFO_SIZE;
    uint8_t local_bind_info[UMQ_MAX_BIND_INFO_SIZE] = {0};
    uint64_t umqh = init_and_create_umq(cfg, local_bind_info, &local_bind_info_size);
    if (umqh == UMQ_INVALID_HANDLE) {
        LOG_PRINT_ERR("init and create umq failed\n");
        return -1;
    }

    test_mem_buf(umqh);

    uint32_t remote_bind_info_size = UMQ_MAX_BIND_INFO_SIZE;
    uint8_t remote_bind_info[UMQ_MAX_BIND_INFO_SIZE] = {0};
    ret = server_exchange_bind_info(cfg->server_ip, cfg->tcp_port, local_bind_info,
        local_bind_info_size, remote_bind_info, &remote_bind_info_size);
    if (ret < 0 || remote_bind_info_size > UMQ_MAX_BIND_INFO_SIZE) {
        LOG_PRINT_ERR("server_exchange_bind_info failed\n");
        goto DESTROY;
    }

    ret = umq_bind(umqh, remote_bind_info, remote_bind_info_size);
    if (ret != UMQ_SUCCESS) {
        LOG_PRINT_ERR("server bind failed\n");
        goto DESTROY;
    }
    LOG_PRINT("server bind success\n");

    if (example_post_rx(umqh, EXAMPLE_MAX_DEPTH) != 0) {
        LOG_PRINT_ERR("server post rx failed\n");
        goto UNBIND;
    }
    LOG_PRINT("server post rx succeeded\n");

    umq_interrupt_option_t interrupt_option = {
        .flag = UMQ_INTERRUPT_FLAG_IO_DIRECTION,
        .direction = UMQ_IO_RX,
    };
    if (umq_rearm_interrupt(umqh, false, &interrupt_option) != 0) {
        LOG_PRINT_ERR("server umq_rearm_interrupt failed\n");
        goto UNBIND;
    }

    int32_t nevents = umq_wait_interrupt(umqh, EXAMPLE_MAX_WAIT_TIME_MS, &interrupt_option);
    if (nevents < 1) {
        LOG_PRINT_ERR("server umq_wait_interrupt failed, ret: %d\n", nevents);
        goto UNBIND;
    }
    LOG_PRINT("server umq_wait_interrupt succeeded\n");

    if (example_poll_rx(umqh, EXAMPLE_CLIENT_POST_DATA, strlen(EXAMPLE_CLIENT_POST_DATA), true) != 0) {
        LOG_PRINT_ERR("server poll_rx failed\n");
        umq_ack_interrupt(umqh, nevents, &interrupt_option);
        goto UNBIND;
    }
    LOG_PRINT("server poll_rx succeeded\n");

    umq_ack_interrupt(umqh, nevents, &interrupt_option);
    LOG_PRINT("server ack interrupt succeeded\n");

    if (example_post_tx(umqh, EXAMPLE_SERVER_POST_DATA, strlen(EXAMPLE_SERVER_POST_DATA)) != 0) {
        LOG_PRINT_ERR("server post_tx failed\n");
        goto UNBIND;
    }
    LOG_PRINT("server post tx succeeded\n");

    umq_notify(umqh);

    uint64_t start = get_timestamp_ms();
    int poll_ret = example_poll_tx(umqh);
    while (poll_ret != 0 && get_timestamp_ms() - start < EXAMPLE_MAX_WAIT_TIME_MS) {
        usleep(EXAMPLE_SLEEP_TIME_US);
        poll_ret = example_poll_tx(umqh);
        continue;
    }

    if (poll_ret != 0) {
        LOG_PRINT_ERR("server poll_tx failed\n");
        goto UNBIND;
    }

    ret = 0;

UNBIND:
    umq_unbind(umqh);
    example_flush(umqh);
DESTROY:
    umq_destroy(umqh);
    umq_uninit();
    return ret;
}

int run_umq_example_pro_client(struct urpc_example_config *cfg)
{
    int ret = -1;
    uint32_t local_bind_info_size = UMQ_MAX_BIND_INFO_SIZE;
    uint8_t local_bind_info[UMQ_MAX_BIND_INFO_SIZE] = {0};
    uint64_t umqh = init_and_create_umq(cfg, local_bind_info, &local_bind_info_size);
    if (umqh == UMQ_INVALID_HANDLE) {
        LOG_PRINT_ERR("init and create umq failed\n");
        return -1;
    }

    uint32_t remote_bind_info_size = UMQ_MAX_BIND_INFO_SIZE;
    uint8_t remote_bind_info[UMQ_MAX_BIND_INFO_SIZE] = {0};
    ret = client_exchange_bind_info(cfg->server_ip, cfg->tcp_port, local_bind_info,
        local_bind_info_size, remote_bind_info, &remote_bind_info_size);
    if (ret < 0 || remote_bind_info_size > UMQ_MAX_BIND_INFO_SIZE) {
        LOG_PRINT_ERR("client_exchange_bind_info failed\n");
        goto DESTROY;
    }

    ret = umq_bind(umqh, remote_bind_info, remote_bind_info_size);
    if (ret != UMQ_SUCCESS) {
        LOG_PRINT_ERR("client bind failed\n");
        goto DESTROY;
    }
    LOG_PRINT("client bind success\n");
    sleep(1); // sleep 1s to wait for server ready

    umq_interrupt_option_t interrupt_option = {
        .flag = UMQ_INTERRUPT_FLAG_IO_DIRECTION,
        .direction = UMQ_IO_RX,
    };

    if (umq_rearm_interrupt(umqh, false, &interrupt_option) != 0) {
        LOG_PRINT_ERR("client umq_rearm_interrupt failed\n");
        goto UNBIND;
    }

    if (example_post_rx(umqh, EXAMPLE_MAX_DEPTH) != 0) {
        LOG_PRINT_ERR("client post rx failed\n");
        goto UNBIND;
    }
    LOG_PRINT("client post rx succeeded\n");

    if (example_post_tx(umqh, EXAMPLE_CLIENT_POST_DATA, strlen(EXAMPLE_CLIENT_POST_DATA)) != 0) {
        LOG_PRINT_ERR("client post_tx failed\n");
        goto UNBIND;
    }
    LOG_PRINT("client post_tx succeeded\n");

    umq_notify(umqh);

    uint64_t start = get_timestamp_ms();
    int poll_ret = example_poll_tx(umqh);
    while (poll_ret != 0 && get_timestamp_ms() - start < EXAMPLE_MAX_WAIT_TIME_MS) {
        usleep(EXAMPLE_SLEEP_TIME_US);
        poll_ret = example_poll_tx(umqh);
        continue;
    }

    if (poll_ret != 0) {
        LOG_PRINT_ERR("client poll_tx failed\n");
        goto UNBIND;
    }

    uint32_t wakeup_num = (uint32_t)umq_wait_interrupt(umqh, 5000, &interrupt_option);
    if (wakeup_num < 1) {
        LOG_PRINT_ERR("client umq_wait_interrupt failed\n");
        goto UNBIND;
    }
    LOG_PRINT("client umq_wait_interrupt succeeded\n");

    if (example_poll_rx(umqh, EXAMPLE_SERVER_POST_DATA, strlen(EXAMPLE_SERVER_POST_DATA), true) != 0) {
        LOG_PRINT_ERR("client poll_rx failed\n");
        goto UNBIND;
    }
    LOG_PRINT("client poll_rx succeeded\n");
    uint32_t nevents = 1;
    umq_ack_interrupt(umqh, nevents, &interrupt_option);

    ret = 0;

UNBIND:
    umq_unbind(umqh);
    example_flush(umqh);
DESTROY:
    umq_destroy(umqh);
    umq_uninit();
    return ret;
}

int run_umq_example_pro(struct urpc_example_config *cfg)
{
    if (cfg->instance_mode == CLIENT) {
        return run_umq_example_pro_client(cfg);
    } else if (cfg->instance_mode == SERVER) {
        return run_umq_example_pro_server(cfg);
    }

    return -1;
}
