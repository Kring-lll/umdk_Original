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

#define EXAMPLE_TEST_ROUND 64
static const uint32_t EXAMPLE_MAX_DEPTH = 64;
static const uint32_t EXAMPLE_MAX_POLL_BATCH = 64;
static const uint32_t EXAMPLE_POLL_NUM = 2;
static const uint32_t EXAMPLE_RW_SIZE = 8192 * 6;
static const uint64_t USER_CTX = 112233;
static const char *EXAMPLE_CLIENT_POST_DATA = "hello, this is umq pro client";
static const char *EXAMPLE_SERVER_POST_DATA = "hello, this is umq pro server";
static const char *g_operate_to_str[UMQ_OPC_MAX] = {"send", "send imm", "read", "write", "write imm"};
static uint64_t addr_list[EXAMPLE_TEST_ROUND];

typedef union umq_ub_imm {
    uint64_t value;
    struct {
        uint64_t umq_private : 1;
        uint64_t type : 3;
        uint64_t rsvd1 : 12;
        uint64_t buf_id : 16;
        uint64_t rsvd2 : 32;
    } ub_plus;
} umq_ub_imm_t;

static int import_mem_for_read_write(uint64_t umqh, umq_buf_t *buf)
{
    umq_mempool_state_t state;
    int ret = umq_mempool_state_get(umqh, buf->mempool_id, &state);
    if (ret != 0) {
        LOG_PRINT_ERR("umq_mempool_state_get failed\n");
        return -1;
    }
    if (state.import_state == MEMPOOL_STATE_IMPORTED) {
        return 0;
    }
    ret = umq_mempool_state_refresh(umqh, buf->mempool_id);
    if (ret != 0) {
        LOG_PRINT_ERR("umq_mempool_state_refresh failed\n");
        return -1;
    }
    umq_buf_t *qbuf = NULL;
    int poll_ret = 0;
    while (1) {
        poll_ret = umq_poll(umqh, UMQ_IO_ALL, &qbuf, EXAMPLE_POLL_NUM);
        if (poll_ret > 0) {
            if (qbuf->status == UMQ_MEMPOOL_UPDATE_SUCCESS) { // refresh的rx
                break;
            }
            umq_buf_free(qbuf); // 释放正常tx
        }
    }
    ret = umq_mempool_state_get(umqh, buf->mempool_id, &state);
    if (ret != UMQ_SUCCESS || state.import_state != MEMPOOL_STATE_IMPORTED) {
        LOG_PRINT("umq_mempool_state_get failed, mempool_id %u, state %u\n", buf->mempool_id,
                  state.import_state);
        umq_buf_free(qbuf);
        return -1;
    }
    LOG_PRINT("umq_mempool_state_get mempool_id %u, state %u\n", buf->mempool_id,
              state.import_state);
    umq_buf_free(qbuf);
    return 0;
}

static int umq_pro_post_tx(uint64_t umqh, umq_buf_t *buf)
{
    umq_buf_t *bad_buf = NULL;
    if (umq_post(umqh, buf, UMQ_IO_TX, &bad_buf) != UMQ_SUCCESS) {
        umq_buf_free(bad_buf);
        LOG_PRINT_ERR("post tx failed\n");
        return -1;
    }
    LOG_PRINT("client post_tx succeeded\n");
    return 0;
}

static int server_response(uint64_t umqh, enum UMQ_OPCODE_TYPE opc, uint64_t imm_data)
{
    if (example_post_tx(umqh, EXAMPLE_SERVER_POST_DATA, strlen(EXAMPLE_SERVER_POST_DATA), imm_data) != 0) {
        LOG_PRINT_ERR("server post_tx failed\n");
        return -1;
    }
    LOG_PRINT("server post tx succeeded\n");
    return 0;
}

static void umq_pro_set_buf_pro(umq_buf_pro_t *buf_pro, umq_ub_ref_data_t *ref_data)
{
    buf_pro->flag.bs.solicited_enable = 1;
    buf_pro->flag.bs.complete_enable = 1;
    buf_pro->flag.bs.inline_flag = 0;
    buf_pro->user_ctx = USER_CTX;
    buf_pro->remote_sge.addr = ref_data->addr;
    buf_pro->remote_sge.length = ref_data->len;
    buf_pro->remote_sge.mempool_id = ref_data->mempool_id;
}

static int umq_pro_server_do_write(uint64_t umqh, umq_buf_t *polled_buf, enum UMQ_OPCODE_TYPE opc)
{
    LOG_PRINT("server do write\n");
    // 申请用于write数据的buf
    umq_ub_ref_data_t *ref_data = (umq_ub_ref_data_t *)polled_buf->buf_data;
    umq_buf_pro_t *rx_buf_pro = (umq_buf_pro_t *)polled_buf->qbuf_ext;
    umq_buf_t *write_buf = umq_buf_alloc(ref_data->len, 1, UMQ_INVALID_HANDLE, NULL);
    if (write_buf == NULL) {
        umq_buf_free(polled_buf);
        LOG_PRINT_ERR("server buf alloc failed\n");
        return -1;
    }
    // 填充数据
    (void)memcpy(write_buf->buf_data, EXAMPLE_SERVER_POST_DATA, strlen(EXAMPLE_SERVER_POST_DATA));
    umq_buf_pro_t *buf_pro = (umq_buf_pro_t *)write_buf->qbuf_ext;
    buf_pro->imm_data = rx_buf_pro->imm_data;
    umq_pro_set_buf_pro(buf_pro, ref_data);
    if (opc == UMQ_OPCODE_WRITE) {
        buf_pro->opcode = UMQ_OPC_WRITE;
    } else {
        buf_pro->opcode = UMQ_OPC_WRITE_IMM;
    }
    umq_buf_free(polled_buf);

    // 发起write请求
    if (umq_pro_post_tx(umqh, write_buf)) {
        return -1;
    }
    return 0;
}

static int umq_pro_server_do_read(uint64_t umqh, umq_buf_t *rx_buf)
{
    // 获取远端数据
    LOG_PRINT_ERR("server do read\n");
    umq_ub_ref_data_t *ref_data = (umq_ub_ref_data_t *)rx_buf->buf_data;
    umq_buf_pro_t *rx_buf_pro = (umq_buf_pro_t *)rx_buf->qbuf_ext;
    umq_buf_t *read_buf = umq_buf_alloc(ref_data->len, 1, UMQ_INVALID_HANDLE, NULL);
    if (read_buf == NULL) {
        umq_buf_free(rx_buf);
        LOG_PRINT_ERR("server buf alloc failed\n");
        return -1;
    }
    umq_buf_pro_t *buf_pro = (umq_buf_pro_t *)read_buf->qbuf_ext;
    buf_pro->imm_data = rx_buf_pro->imm_data;
    umq_pro_set_buf_pro(buf_pro, ref_data);
    buf_pro->opcode = UMQ_OPC_READ;
    umq_buf_free(rx_buf);

    // 发起read请求
    (void)memset(read_buf->buf_data, 0, strlen(EXAMPLE_CLIENT_POST_DATA));
    LOG_PRINT("read before, read data[%s]\n", (char *)read_buf->buf_data);
    if (umq_pro_post_tx(umqh, read_buf)) {
        return -1;
    }
    return 0;
}

static int umq_pro_check_send_data(uint64_t umqh, umq_buf_t *polled_buf, enum UMQ_OPCODE_TYPE opc,
                                   const char *check_data)
{
    umq_buf_pro_t *pro = (umq_buf_pro_t *)polled_buf->qbuf_ext;
    if (memcmp((char *)polled_buf->buf_data, check_data, strlen(check_data)) != 0) {
        LOG_PRINT_ERR("polled data[%s] doesn't match check data[%s]\n", (char *)polled_buf->buf_data, check_data);
        umq_buf_free(polled_buf);
        return -1;
    }
    if (opc == UMQ_OPCODE_SEND_IMM && pro->imm_data != EXAMPLE_TEST_IMM_DATA) {
        LOG_PRINT_ERR("polled imm_data[%u] doesn't match imm data[%u]\n", (uint32_t)pro->imm_data,
                      EXAMPLE_TEST_IMM_DATA);
        umq_buf_free(polled_buf);
        return -1;
    }
    LOG_PRINT("polled data: %s, imm_data: %lu\n", (char *)polled_buf->buf_data, pro->imm_data);
    return 0;
}

static int umq_pro_server_process_send(uint64_t umqh, umq_buf_t *polled_buf, enum UMQ_OPCODE_TYPE opc)
{
    if (umq_pro_check_send_data(umqh, polled_buf, opc, EXAMPLE_CLIENT_POST_DATA)) {
        return -1;
    }
    umq_buf_free(polled_buf);
    // 向客户端发送通知，send结束
    if (server_response(umqh, opc, EXAMPLE_TEST_IMM_DATA)) {
        return -1;
    }
    return 0;
}

static int umq_pro_server_poll_rx(uint64_t umqh, enum UMQ_OPCODE_TYPE opc)
{
    umq_buf_t *polled_buf[EXAMPLE_MAX_POLL_BATCH];
    int ret = umq_poll(umqh, UMQ_IO_RX, polled_buf, EXAMPLE_MAX_POLL_BATCH);
    if (ret < 0) {
        return -1;
    }
    for (int i = 0; i < ret; i++) {
        umq_ub_ref_data_t *ref_data = (umq_ub_ref_data_t *)polled_buf[i]->buf_data;
        if (polled_buf[i]->status == UMQ_MEMPOOL_UPDATE_SUCCESS) { // import mem的rx
            LOG_PRINT("import mem success\n");
            umq_buf_free(polled_buf[i]);
        } else if (ref_data->opc == UMQ_OPCODE_READ) {
            umq_pro_server_do_read(umqh, polled_buf[i]);
        } else if (ref_data->opc == UMQ_OPCODE_WRITE || ref_data->opc == UMQ_OPCODE_WRITE_IMM) {
            umq_pro_server_do_write(umqh, polled_buf[i], opc);
        } else {
            umq_pro_server_process_send(umqh, polled_buf[i], opc);
        } 
    }
    return 0;
}

static int umq_pro_server_poll_tx(uint64_t umqh, enum UMQ_OPCODE_TYPE opc)
{
    umq_buf_t *polled_buf[EXAMPLE_MAX_POLL_BATCH];
    int poll_ret = umq_poll(umqh, UMQ_IO_TX, polled_buf, EXAMPLE_MAX_POLL_BATCH);
    if (poll_ret < 0) {
        return -1;
    }
    for (int i = 0; i < poll_ret; i++) {
        umq_buf_pro_t *pro = (umq_buf_pro_t *)polled_buf[i]->qbuf_ext;
        umq_ub_imm_t imm = {.value = pro->imm_data};
        if (pro->opcode == UMQ_OPC_READ) {
            // poll到read的数据后，校验数据内容
            if (memcmp((char *)polled_buf[i]->buf_data, EXAMPLE_CLIENT_POST_DATA,
                       strlen(EXAMPLE_CLIENT_POST_DATA)) != 0) {
                LOG_PRINT_ERR("polled data[%s] doesn't match check data[%s]\n", (char *)polled_buf[i]->buf_data,
                              EXAMPLE_CLIENT_POST_DATA);
            }
            LOG_PRINT("read after, read data[%s]\n", (char *)polled_buf[i]->buf_data);
            // 向客户端发送通知，read结束
            if (server_response(umqh, opc, imm.value)) {
                return -1;
            }
        } else if (pro->opcode == UMQ_OPC_WRITE) {
            // 向客户端发送通知，write结束
            if (server_response(umqh, opc, imm.value)) {
                return -1;
            }
        }
        umq_buf_free(polled_buf[i]);
    }
    return 0;
}

static int umq_pro_server_read(uint64_t umqh, enum UMQ_OPCODE_TYPE opc)
{
    LOG_PRINT("server %s test start\n", g_operate_to_str[opc]);
    while (1) {
        if (umq_pro_server_poll_rx(umqh, opc)) {
            return -1;
        }
        if (umq_pro_server_poll_tx(umqh, opc)) {
            return -1;
        }
    }
    return 0;
}

static int umq_pro_server_write(uint64_t umqh, enum UMQ_OPCODE_TYPE opc)
{
    LOG_PRINT("server %s test start\n", g_operate_to_str[opc]);
    while (1) {
        if (umq_pro_server_poll_rx(umqh, opc)) {
            return -1;
        }
        if (umq_pro_server_poll_tx(umqh, opc)) {
            return -1;
        }
    }
    return 0;
}

static int umq_pro_server_send(uint64_t umqh, enum UMQ_OPCODE_TYPE opc)
{
    LOG_PRINT("server %s test start\n", g_operate_to_str[opc]);
    while (1) {
        if (umq_pro_server_poll_rx(umqh, opc)) {
            return -1;
        }
        if (umq_pro_server_poll_tx(umqh, opc)) {
            return -1;
        }
    }
    return 0;
}

static int run_umq_example_pro_server(struct urpc_example_config *cfg)
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

    enum UMQ_OPCODE_TYPE opc = cfg->opc_type;
    if (opc == UMQ_OPCODE_SEND || opc == UMQ_OPCODE_SEND_IMM) {
        ret = umq_pro_server_send(umqh, opc);
    } else if (opc == UMQ_OPCODE_READ) {
        ret = umq_pro_server_read(umqh, opc);
    } else if (opc == UMQ_OPCODE_WRITE || opc == UMQ_OPCODE_WRITE_IMM) {
        ret = umq_pro_server_write(umqh, opc);
    }
    if (ret != 0) {
        LOG_PRINT_ERR("server failed to run test\n");
        goto UNBIND;
    }

UNBIND:
    umq_unbind(umqh);
    example_flush(umqh);
DESTROY:
    umq_destroy(umqh);
    umq_uninit();
    return ret;
}

static void umq_pro_client_set_send_buf(umq_buf_t *send_buf, umq_buf_t *rw_buf, enum UMQ_OPCODE_TYPE opc,
                                        uint16_t buf_id)
{
    send_buf->io_direction = UMQ_IO_TX;
    umq_buf_pro_t *buf_pro = (umq_buf_pro_t *)send_buf->qbuf_ext;
    umq_ub_ref_data_t *ref_data = (umq_ub_ref_data_t *)send_buf->buf_data;
    ref_data->addr = (uint64_t)(uintptr_t)rw_buf->buf_data;
    ref_data->len = rw_buf->total_data_size;
    ref_data->mempool_id = rw_buf->mempool_id;
    ref_data->opc = opc;
    buf_pro->flag.bs.solicited_enable = 1;
    buf_pro->flag.bs.complete_enable = 1;
    buf_pro->flag.bs.inline_flag = 0;
    umq_ub_imm_t imm = {.ub_plus = {.umq_private = 0, .type = opc, .buf_id = buf_id}};
    buf_pro->opcode = UMQ_OPC_SEND_IMM;
    buf_pro->imm_data = imm.value;
}

static int umq_pro_client_read_post_tx(uint64_t umqh, enum UMQ_OPCODE_TYPE opc, uint16_t buf_id)
{
    // 申请供对端read的buf
    umq_buf_t *read_buf = umq_buf_alloc(EXAMPLE_RW_SIZE, 1, UMQ_INVALID_HANDLE, NULL);
    if (read_buf == NULL) {
        LOG_PRINT_ERR("client buf alloc failed\n");
        return -1;
    }
    if (import_mem_for_read_write(umqh, read_buf) != UMQ_SUCCESS) {
        return -1;
    }
    // 填充数据
    (void)memcpy(read_buf->buf_data, EXAMPLE_CLIENT_POST_DATA, strlen(EXAMPLE_CLIENT_POST_DATA));
    // send_buf传输ref_data信息，并通过SEND语义将数据发送到对端
    umq_buf_t *send_buf = umq_buf_alloc(sizeof(umq_ub_ref_data_t), 1, UMQ_INVALID_HANDLE, NULL);
    if (send_buf == NULL) {
        LOG_PRINT_ERR("client buf alloc failed\n");
        umq_buf_free(read_buf);
        return -1;
    }
    umq_pro_client_set_send_buf(send_buf, read_buf, opc, buf_id);
    addr_list[buf_id] = (uint64_t)(uintptr_t)read_buf;
    if (umq_pro_post_tx(umqh, send_buf)) {
        return -1;
    }
    return 0;
}

static int umq_pro_client_poll_rx(uint64_t umqh)
{
    umq_buf_t *polled_buf[EXAMPLE_MAX_POLL_BATCH];
    int ret = umq_poll(umqh, UMQ_IO_RX, polled_buf, EXAMPLE_MAX_POLL_BATCH);
    if (ret < 0) {
        return -1;
    }
    for (int i = 0; i < ret; i++) {
        umq_buf_pro_t *pro = (umq_buf_pro_t *)polled_buf[i]->qbuf_ext;
        umq_ub_imm_t imm = {.value = pro->imm_data};
        int buf_id = imm.ub_plus.buf_id;
        int type = imm.ub_plus.type;
        LOG_PRINT("client poll rx buf_id %d, type %d\n", buf_id, type);
        if (type == UMQ_OPCODE_READ) {
            // 服务端会在read完成后，发送返回数据，所以收到返回数据后即可释放read_buf
            umq_buf_t *read_buf = (umq_buf_t *)addr_list[buf_id];
            umq_buf_free(read_buf);
        } else if (type == UMQ_OPCODE_WRITE || type == UMQ_OPCODE_WRITE_IMM) {
            // 服务端会在write完成后，发送返回数据，所以收到返回数据后即可查看write_buf
            umq_buf_t *write_buf = (umq_buf_t *)addr_list[buf_id];
            if (memcmp((char *)write_buf->buf_data, EXAMPLE_SERVER_POST_DATA, strlen(EXAMPLE_SERVER_POST_DATA)) != 0) {
                LOG_PRINT_ERR("writed data[%s] doesn't match check data[%s]\n", (char *)write_buf->buf_data,
                              EXAMPLE_SERVER_POST_DATA);
                return -1;
            }
            LOG_PRINT("writed after, write_buf data[%s]\n", (char *)write_buf->buf_data);
            umq_buf_free(write_buf);
        } else {
            if (umq_pro_check_send_data(umqh, polled_buf[i], type, EXAMPLE_SERVER_POST_DATA)) {
                return -1;
            }
        }
        umq_buf_free(polled_buf[i]);
    }
    return 0;
}

static int umq_pro_client_poll_tx(uint64_t umqh)
{
    umq_buf_t *polled_buf[EXAMPLE_MAX_POLL_BATCH];
    int poll_ret = umq_poll(umqh, UMQ_IO_TX, polled_buf, EXAMPLE_MAX_POLL_BATCH);
    if (poll_ret < 0) {
        return -1;
    }
    for (int i = 0; i < poll_ret; i++) {
        umq_buf_free(polled_buf[i]);
    }
    return 0;
}

static int umq_pro_client_read(uint64_t umqh, enum UMQ_OPCODE_TYPE opc)
{
    LOG_PRINT("client %s test start\n", g_operate_to_str[opc]);
    uint32_t iter = 0;
    while (++iter < EXAMPLE_TEST_ROUND) {
        if (umq_pro_client_read_post_tx(umqh, opc, iter)) {
            return -1;
        }
        sleep(0.1);
        if (umq_pro_client_poll_tx(umqh)) {
            return -1;
        }
        // 等待服务器发送的返回数据
        if (umq_pro_client_poll_rx(umqh)) {
            LOG_PRINT_ERR("client poll_rx failed\n");
            return -1;
        }
        LOG_PRINT_ERR("client iter %u\n", iter);
    }
    LOG_PRINT("client %s test succeed\n", g_operate_to_str[opc]);
    return 0;
}

static int umq_pro_client_write_post_tx(uint64_t umqh, enum UMQ_OPCODE_TYPE opc, uint16_t buf_id)
{
    // 申请供对端write的buf
    umq_buf_t *write_buf = umq_buf_alloc(EXAMPLE_RW_SIZE, 1, UMQ_INVALID_HANDLE, NULL);
    if (write_buf == NULL) {
        LOG_PRINT_ERR("client buf alloc failed\n");
        return -1;
    }
    if (import_mem_for_read_write(umqh, write_buf) != UMQ_SUCCESS) {
        return -1;
    }
    (void)memset(write_buf->buf_data, 0, strlen(EXAMPLE_SERVER_POST_DATA));
    // send_buf传输ref_data信息，并通过SEND语义将数据发送到对端
    umq_buf_t *send_buf = umq_buf_alloc(sizeof(umq_ub_ref_data_t), 1, UMQ_INVALID_HANDLE, NULL);
    if (send_buf == NULL) {
        LOG_PRINT_ERR("client buf alloc failed\n");
        umq_buf_free(write_buf);
        return -1;
    }
    umq_pro_client_set_send_buf(send_buf, write_buf, opc, buf_id);
    addr_list[buf_id] = (uint64_t)(uintptr_t)write_buf;
    LOG_PRINT("write before, buf data[%s]\n", (char *)write_buf->buf_data);
    if (umq_pro_post_tx(umqh, send_buf)) {
        return -1;
    }
    return 0;
}

static int umq_pro_client_write(uint64_t umqh, enum UMQ_OPCODE_TYPE opc)
{
    LOG_PRINT("client %s test start\n", g_operate_to_str[opc]);
    uint32_t iter = 0;
    while (++iter < EXAMPLE_TEST_ROUND) {
        if (umq_pro_client_write_post_tx(umqh, opc, iter)) {
            return -1;
        }
        sleep(1);
        if (umq_pro_client_poll_tx(umqh)) {
            return -1;
        }
        // 等待服务器发送的返回数据
        if (umq_pro_client_poll_rx(umqh)) {
            LOG_PRINT_ERR("client poll_rx failed\n");
            return -1;
        }
        LOG_PRINT_ERR("client iter %u\n", iter);
    }
    LOG_PRINT("client %s test succeed\n", g_operate_to_str[opc]);
    return 0;
}

static int umq_pro_client_send_post_tx(uint64_t umqh, enum UMQ_OPCODE_TYPE opc, uint16_t buf_id)
{
    umq_buf_t *send_buf = umq_buf_alloc(strlen(EXAMPLE_CLIENT_POST_DATA), 1, UMQ_INVALID_HANDLE, NULL);
    if (send_buf == NULL) {
        LOG_PRINT_ERR("client buf alloc failed\n");
        return -1;
    }
    (void)memcpy(send_buf->buf_data, EXAMPLE_CLIENT_POST_DATA, strlen(EXAMPLE_CLIENT_POST_DATA));
    umq_buf_pro_t *buf_pro = (umq_buf_pro_t *)send_buf->qbuf_ext;
    send_buf->io_direction = UMQ_IO_TX;
    buf_pro->flag.bs.solicited_enable = 1;
    buf_pro->flag.bs.complete_enable = 1;
    buf_pro->flag.bs.inline_flag = 0;
    if (opc == UMQ_OPCODE_SEND) {
        buf_pro->opcode = UMQ_OPC_SEND;
    } else {
        buf_pro->opcode = UMQ_OPC_SEND_IMM;
        buf_pro->imm_data = EXAMPLE_TEST_IMM_DATA;
    }
    if (umq_pro_post_tx(umqh, send_buf)) {
        return -1;
    }
    return 0;
}

static int umq_pro_client_send(uint64_t umqh, enum UMQ_OPCODE_TYPE opc)
{
    LOG_PRINT("client %s test start\n", g_operate_to_str[opc]);
    uint32_t iter = 0;
    while (++iter < EXAMPLE_TEST_ROUND) {
        if (umq_pro_client_send_post_tx(umqh, opc, iter)) {
            return -1;
        }
        if (umq_pro_client_poll_tx(umqh)) {
            return -1;
        }
        // 等待服务器发送的返回数据
        if (umq_pro_client_poll_rx(umqh)) {
            LOG_PRINT_ERR("client poll_rx failed\n");
            return -1;
        }
        LOG_PRINT("client iter %u\n", iter);
    }
    LOG_PRINT("client %s test succeed\n", g_operate_to_str[opc]);
    return 0;
}

static int run_umq_example_pro_client(struct urpc_example_config *cfg)
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

    if (example_post_rx(umqh, EXAMPLE_MAX_DEPTH) != 0) {
        LOG_PRINT_ERR("client post rx failed\n");
        return -1;
    }
    LOG_PRINT("client post rx succeeded\n");

    enum UMQ_OPCODE_TYPE opc = cfg->opc_type;
    if (opc == UMQ_OPCODE_SEND || opc == UMQ_OPCODE_SEND_IMM) {
        ret = umq_pro_client_send(umqh, opc);
    } else if (opc == UMQ_OPCODE_READ) {
        ret = umq_pro_client_read(umqh, opc);
    } else if (opc == UMQ_OPCODE_WRITE || opc == UMQ_OPCODE_WRITE_IMM) {
        ret = umq_pro_client_write(umqh, opc);
    }
    if (ret != 0) {
        LOG_PRINT_ERR("client failed to run test\n");
        goto UNBIND;
    }

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
