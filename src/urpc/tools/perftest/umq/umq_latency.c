/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: umq perftest epoll_wait latency test case
 * Create: 2026-5-25
 */

#include <errno.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "umq_api.h"
#include "umq_pro_api.h"

#include "perftest_latency.h"
#include "perftest_util.h"
#include "umq_perftest_latency.h"

#define UMQ_EPOLL_EVENT_BATCH 32
#define UMQ_POST_POLL_BATCH   64

static int umq_tx_qbuf_cnt(umq_buf_t *head, umq_buf_t *tail)
{
    int cnt = 0;
    umq_buf_t *tmp = head;
    while (tmp != tail) {
        tmp = tmp->qbuf_next;
        cnt++;
    }

    return cnt;
}

void umq_server_latency_pro_interrupt(uint64_t umqh, umq_perftest_latency_arg_t *lat_arg)
{
    int ret;
    // init
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        LOG_PRINT("epoll_create1 failed, errno=%d\n", errno);
        return;
    }

    umq_interrupt_option_t interrupt_option = {UMQ_INTERRUPT_FLAG_IO_DIRECTION, UMQ_IO_TX};
    int tx_fd = umq_interrupt_fd_get(umqh, &interrupt_option);
    if (tx_fd < 0) {
        LOG_PRINT("get tx_fd failed\n");
        goto CLOSE_EPOLL_FD;
    }

    interrupt_option.direction = UMQ_IO_RX;
    int rx_fd = umq_interrupt_fd_get(umqh, &interrupt_option);
    if (rx_fd < 0) {
        LOG_PRINT("get rx_fd failed\n");
        goto CLOSE_EPOLL_FD;
    }

    struct epoll_event event;
    event.events = EPOLLIN | EPOLLET;

    event.data.fd = rx_fd;
    ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, rx_fd, &event);
    if (ret < 0) {
        LOG_PRINT("epoll_ctl add rx_fd failed, errno=%d\n", errno);
        goto CLOSE_EPOLL_FD;
    }

    event.data.fd = tx_fd;
    ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, tx_fd, &event);
    if (ret < 0) {
        LOG_PRINT("epoll_ctl add tx_fd failed, errno=%d\n", errno);
        goto CLOSE_EPOLL_FD;
    }

    LOG_PRINT("epoll_ctl add tx_fd %d, rx_fd %d\n", tx_fd, rx_fd);

    interrupt_option.direction = UMQ_IO_TX;
    if (umq_rearm_interrupt(umqh, false, &interrupt_option) != 0) {
        LOG_PRINT("umq_rearm_interrupt failed\n");
        goto CLOSE_EPOLL_FD;
    }

    interrupt_option.direction = UMQ_IO_RX;
    if (umq_rearm_interrupt(umqh, false, &interrupt_option) != 0) {
        LOG_PRINT("umq_rearm_interrupt failed\n");
        goto CLOSE_EPOLL_FD;
    }

    umq_buf_t *head, *recv_qbuf, *send_qbuf, *bad_qbuf = NULL;
    umq_buf_pro_t *buf_pro;

    // epoll_wait
    int event_num, poll_num, cq_num;
    uint64_t recv_size = 0;
    uint64_t send_size = 0;
    uint32_t test_round = lat_arg->cfg->test_round;
    uint32_t poll_rx_num = 0;
    struct epoll_event e[UMQ_EPOLL_EVENT_BATCH];
    umq_buf_t *buf[UMQ_POST_POLL_BATCH];
    bool can_send = false;
    uint32_t send_cnt = 0, recv_cnt = 0;
    uint32_t tx_depth = lat_arg->config.tx_depth;

    send_qbuf = umq_buf_alloc(lat_arg->cfg->rsp_size, 1, UMQ_INVALID_HANDLE, NULL);
    if (send_qbuf == NULL) {
        goto CLOSE_EPOLL_FD;
    }

    head = send_qbuf;
    while (head != NULL) {
        buf_pro = (umq_buf_pro_t *)head->qbuf_ext;
        buf_pro->opcode = UMQ_OPC_SEND;
        buf_pro->flag.value = 0;
        buf_pro->user_ctx = 0;
        buf_pro->flag.bs.solicited_enable = 1;
        buf_pro->flag.bs.complete_enable = 1; // tx notify

        head = head->qbuf_next;
    }

    while (get_perftest_latency_ctx()->iters < test_round && !is_perftest_force_quit()) {
        event_num = epoll_wait(epoll_fd, e, UMQ_EPOLL_EVENT_BATCH, -1);
        if (event_num < 0) {
            LOG_PRINT("epoll_wait failed, errno=%d\n", errno);
            goto CLOSE_EPOLL_FD;
        }

        LOG_PRINT("epoll_wait event cnt %d\n", event_num);
        for (int i = 0; i < event_num; i++) {
            LOG_PRINT("epoll_wait event fd %d\n", e[i].data.fd);
            if (e[i].data.fd == rx_fd) {
                interrupt_option.direction = UMQ_IO_RX;
                cq_num = umq_get_cq_event(umqh, &interrupt_option);
                if (cq_num < 0) {
                    LOG_PRINT("umq_get_cq_event failed, cq_num=%d\n", cq_num);
                    goto CLOSE_EPOLL_FD;
                }

                // 检查接收长度满足情况, 开启发送流程
                do {
                    poll_num = umq_poll(umqh, UMQ_IO_RX, buf, UMQ_POST_POLL_BATCH);
                    if (poll_num < 0) {
                        LOG_PRINT("umq_poll failed, ret=%d\n", poll_num);
                        goto CLOSE_EPOLL_FD;
                    }

                    // LOG_PRINT("poll %d rx, poll_rx_num %d\n", poll_num, poll_rx_num);

                    for (int j = 0; j < poll_num; j++) {
                        if (buf[j]->status != UMQ_BUF_SUCCESS) {
                            if (buf[j]->status == UMQ_FAKE_BUF_FC_UPDATE) {
                                // continue to send tx
                                can_send = true;
                                buf[j]->qbuf_next = NULL;
                                umq_buf_free(buf[j]);
                                LOG_PRINT("umq_poll fc update\n");
                                continue;
                            }

                            LOG_PRINT("umq_poll failed, rx buf status=%d\n", buf[j]->status);
                            goto CLOSE_EPOLL_FD;
                        }
                        
                        poll_rx_num++;
                        recv_size += buf[j]->total_data_size;
                        buf[j]->qbuf_next = NULL;
                        umq_buf_free(buf[j]);

                        if (recv_size == lat_arg->cfg->req_size) {
                            can_send = true;
                            LOG_PRINT("recv %uth request %lu\n", recv_cnt, recv_size);
                            recv_size = 0;
                            recv_cnt++;
                        } else if (recv_size > lat_arg->cfg->req_size) {
                            LOG_PRINT("umq_poll failed, recv_size=%lu larger than req_size=%u\n", recv_size,
                                      lat_arg->cfg->req_size);
                            goto CLOSE_EPOLL_FD;
                        }
                    }
                } while (poll_num == UMQ_POST_POLL_BATCH);

                umq_ack_interrupt(umqh, cq_num, &interrupt_option);

                // post rx
                if (poll_rx_num >= UMQ_POST_POLL_BATCH) {
                    recv_qbuf = umq_buf_alloc(lat_arg->cfg->blk_size, poll_rx_num, UMQ_INVALID_HANDLE, NULL);
                    if (recv_qbuf == NULL) {
                        LOG_PRINT("alloc buf failed\n");
                        goto CLOSE_EPOLL_FD;
                    }

                    if (umq_post(umqh, recv_qbuf, UMQ_IO_RX, &bad_qbuf) != UMQ_SUCCESS) {
                        LOG_PRINT("post rx failed\n");
                        umq_buf_free(recv_qbuf);
                        goto CLOSE_EPOLL_FD;
                    }

                    poll_rx_num = 0;
                }

                ret = umq_rearm_interrupt(umqh, false, &interrupt_option);
                if (ret < 0) {
                    LOG_PRINT("umq_rearm_interrupt failed, ret=%d\n", ret);
                    goto CLOSE_EPOLL_FD;
                }
                // poll_tx if necessary
            } else {
                interrupt_option.direction = UMQ_IO_TX;
                cq_num = umq_get_cq_event(umqh, &interrupt_option);
                if (cq_num < 0) {
                    LOG_PRINT("umq_get_cq_event failed, cq_num=%d\n", cq_num);
                    goto CLOSE_EPOLL_FD;
                }

                LOG_PRINT("umq_get_cq_event tx cq_num=%d\n", cq_num);
                do {
                    poll_num = umq_poll(umqh, UMQ_IO_TX, buf, UMQ_POST_POLL_BATCH);
                    if (poll_num < 0) {
                        LOG_PRINT("umq_poll failed, ret=%d\n", poll_num);
                        goto CLOSE_EPOLL_FD;
                    }
                    tx_depth += poll_num;
                    LOG_PRINT("umq_poll tx poll_num=%d\n", poll_num);
                    for (int j = 0; j < poll_num; j++) {
                        if (buf[j]->status != UMQ_BUF_SUCCESS) {
                            LOG_PRINT("umq_poll failed, tx buf status=%d\n", buf[j]->status);
                            goto CLOSE_EPOLL_FD;
                        }

                        send_size += buf[j]->data_size;
                        buf[j]->qbuf_next = NULL;
                        umq_buf_free(buf[j]);

                        if (send_size == lat_arg->cfg->rsp_size) {
                            can_send = true;
                            get_perftest_latency_ctx()->iters++;
                            if (get_perftest_latency_ctx()->iters >= test_round) {
                                goto CLOSE_EPOLL_FD;
                            }

                            send_qbuf = umq_buf_alloc(lat_arg->cfg->rsp_size, 1, UMQ_INVALID_HANDLE, NULL);
                            if (send_qbuf == NULL) {
                                goto CLOSE_EPOLL_FD;
                            }
                            head = send_qbuf;
                            while (head != NULL) {
                                buf_pro = (umq_buf_pro_t *)head->qbuf_ext;
                                buf_pro->opcode = UMQ_OPC_SEND;
                                buf_pro->flag.value = 0;
                                buf_pro->user_ctx = 0;
                                buf_pro->flag.bs.solicited_enable = 1;
                                buf_pro->flag.bs.complete_enable = 1;

                                head = head->qbuf_next;
                            }

                            send_size = 0;
                        } else if (send_size > lat_arg->cfg->rsp_size) {
                            LOG_PRINT("umq_poll failed, send_size=%lu larger than rsp_size=%u\n", send_size,
                                      lat_arg->cfg->rsp_size);
                            goto CLOSE_EPOLL_FD;
                        }
                    }
                } while (poll_num == UMQ_POST_POLL_BATCH);

                umq_ack_interrupt(umqh, cq_num, &interrupt_option);

                ret = umq_rearm_interrupt(umqh, false, &interrupt_option);
                if (ret < 0) {
                    LOG_PRINT("umq_rearm_interrupt failed, ret=%d\n", ret);
                    goto CLOSE_EPOLL_FD;
                }
            }
        }

        // send tx
        if (can_send && tx_depth > 0 && send_qbuf != NULL) {
            bad_qbuf = NULL;
            ret = umq_post(umqh, send_qbuf, UMQ_IO_TX, &bad_qbuf);
            if (ret == 0) {
                send_cnt++;

                tx_depth -= umq_tx_qbuf_cnt(send_qbuf, NULL);
                can_send = false;
                LOG_PRINT("umq_post %u success\n", send_cnt);
                continue;
            }

            if (ret == -UMQ_ERR_EAGAIN) {
                tx_depth -= umq_tx_qbuf_cnt(send_qbuf, bad_qbuf);
                send_qbuf = bad_qbuf;
                continue;
            }

            LOG_PRINT("umq_post failed, ret=%d\n", ret);
            goto CLOSE_EPOLL_FD;
        }
    }

CLOSE_EPOLL_FD:
    close(epoll_fd);
}

void umq_client_latency_pro_interrupt(uint64_t umqh, umq_perftest_latency_arg_t *lat_arg)
{
    int ret;
    // init
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        LOG_PRINT("epoll_create1 failed, errno=%d\n", errno);
        return;
    }

    umq_interrupt_option_t interrupt_option = {UMQ_INTERRUPT_FLAG_IO_DIRECTION, UMQ_IO_TX};
    int tx_fd = umq_interrupt_fd_get(umqh, &interrupt_option);
    if (tx_fd < 0) {
        LOG_PRINT("get tx_fd failed\n");
        goto CLOSE_EPOLL_FD;
    }

    struct epoll_event event;
    event.events = EPOLLIN | EPOLLET;
    event.data.fd = tx_fd;
    ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, tx_fd, &event);
    if (ret < 0) {
        LOG_PRINT("epoll_ctl add tx_fd failed, errno=%d\n", errno);
        goto CLOSE_EPOLL_FD;
    }

    interrupt_option.direction = UMQ_IO_RX;
    int rx_fd = umq_interrupt_fd_get(umqh, &interrupt_option);
    if (rx_fd < 0) {
        LOG_PRINT("get rx_fd failed\n");
        goto CLOSE_EPOLL_FD;
    }

    event.data.fd = rx_fd;
    ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, rx_fd, &event);
    if (ret < 0) {
        LOG_PRINT("epoll_ctl add rx_fd failed, errno=%d\n", errno);
        goto CLOSE_EPOLL_FD;
    }

    interrupt_option.direction = UMQ_IO_TX;
    if (umq_rearm_interrupt(umqh, false, &interrupt_option) != 0) {
        LOG_PRINT("umq_rearm_interrupt failed\n");
        goto CLOSE_EPOLL_FD;
    }

    interrupt_option.direction = UMQ_IO_RX;
    if (umq_rearm_interrupt(umqh, false, &interrupt_option) != 0) {
        LOG_PRINT("umq_rearm_interrupt failed\n");
        goto CLOSE_EPOLL_FD;
    }

    umq_buf_t *head, *recv_qbuf, *send_qbuf, *bad_qbuf = NULL;

    // epoll_wait
    int event_num, poll_num, cq_num;
    uint64_t recv_size = 0;
    uint64_t send_size = 0;
    uint32_t test_round = lat_arg->cfg->test_round;
    uint32_t poll_rx_num = 0;
    struct epoll_event e[UMQ_EPOLL_EVENT_BATCH];
    umq_buf_t *buf[UMQ_POST_POLL_BATCH];
    umq_buf_pro_t *buf_pro;
    bool can_send = true;
    uint32_t send_cnt = 0, recv_cnt = 0;
    uint32_t tx_depth = lat_arg->config.tx_depth;

    send_qbuf = umq_buf_alloc(lat_arg->cfg->req_size, 1, UMQ_INVALID_HANDLE, NULL);
    if (send_qbuf == NULL) {
        goto CLOSE_EPOLL_FD;
    }
    head = send_qbuf;
    while (head != NULL) {
        buf_pro = (umq_buf_pro_t *)head->qbuf_ext;
        buf_pro->opcode = UMQ_OPC_SEND;
        buf_pro->flag.value = 0;
        buf_pro->user_ctx = 0;
        buf_pro->flag.bs.solicited_enable = 1;
        buf_pro->flag.bs.complete_enable = 1;

        head = head->qbuf_next;
    }

    ret = umq_post(umqh, send_qbuf, UMQ_IO_TX, &bad_qbuf);
    if (ret == 0) {
        can_send = false;
        tx_depth -= umq_tx_qbuf_cnt(send_qbuf, NULL);
        send_cnt++;
    } else if (ret == -UMQ_ERR_EAGAIN) {
        tx_depth -= umq_tx_qbuf_cnt(send_qbuf, bad_qbuf);
        send_qbuf = bad_qbuf;
    } else {
        goto CLOSE_EPOLL_FD;
    }

    while (get_perftest_latency_ctx()->iters < test_round && !is_perftest_force_quit()) {
        event_num = epoll_wait(epoll_fd, e, UMQ_EPOLL_EVENT_BATCH, -1);
        if (event_num < 0) {
            LOG_PRINT("epoll_wait failed, errno=%d\n", errno);
            goto CLOSE_EPOLL_FD;
        }

        for (int i = 0; i < event_num; i++) {
            if (e[i].data.fd == rx_fd) {
                interrupt_option.direction = UMQ_IO_RX;
                cq_num = umq_get_cq_event(umqh, &interrupt_option);
                if (cq_num < 0) {
                    LOG_PRINT("umq_get_cq_event failed, cq_num=%d\n", cq_num);
                    goto CLOSE_EPOLL_FD;
                }

                // 检查接收长度满足情况, 开启发送流程
                do {
                    poll_num = umq_poll(umqh, UMQ_IO_RX, buf, UMQ_POST_POLL_BATCH);
                    if (poll_num < 0) {
                        LOG_PRINT("umq_poll failed, ret=%d\n", poll_num);
                        goto CLOSE_EPOLL_FD;
                    }

                    // LOG_PRINT("poll %d rx, poll_rx_num %d\n", poll_num, poll_rx_num);

                    for (int j = 0; j < poll_num; j++) {
                        if (buf[j]->status != UMQ_BUF_SUCCESS) {
                            if (buf[j]->status == UMQ_FAKE_BUF_FC_UPDATE) {
                                // continue to send tx
                                can_send = true;
                                buf[j]->qbuf_next = NULL;
                                umq_buf_free(buf[j]);
                                LOG_PRINT("umq_poll fc update\n");
                                continue;
                            }

                            LOG_PRINT("umq_poll failed, rx buf status=%d\n", buf[j]->status);
                            goto CLOSE_EPOLL_FD;
                        }

                        poll_rx_num++;
                        recv_size += buf[j]->total_data_size;
                        buf[j]->qbuf_next = NULL;
                        umq_buf_free(buf[j]);

                        if (recv_size == lat_arg->cfg->rsp_size) {
                            can_send = true;
                            recv_cnt++;
                            LOG_PRINT("recv %uth response %lu\n", recv_cnt, recv_size);

                            get_perftest_latency_ctx()->iters++;
                            if (get_perftest_latency_ctx()->iters >= test_round) {
                                goto CLOSE_EPOLL_FD;
                            }

                            recv_size = 0;
                        } else if (recv_size > lat_arg->cfg->rsp_size) {
                            LOG_PRINT("umq_poll failed, recv_size=%lu larger than rsp_size=%u\n", recv_size,
                                      lat_arg->cfg->rsp_size);
                            goto CLOSE_EPOLL_FD;
                        }
                    }
                } while (poll_num == UMQ_POST_POLL_BATCH);

                umq_ack_interrupt(umqh, cq_num, &interrupt_option);

                // post rx
                if (poll_rx_num >= UMQ_POST_POLL_BATCH) {
                    recv_qbuf = umq_buf_alloc(lat_arg->cfg->blk_size, poll_rx_num, UMQ_INVALID_HANDLE, NULL);
                    if (recv_qbuf == NULL) {
                        LOG_PRINT("alloc buf failed\n");
                        goto CLOSE_EPOLL_FD;
                    }

                    if (umq_post(umqh, recv_qbuf, UMQ_IO_RX, &bad_qbuf) != UMQ_SUCCESS) {
                        LOG_PRINT("post rx failed\n");
                        umq_buf_free(recv_qbuf);
                        goto CLOSE_EPOLL_FD;
                    }

                    poll_rx_num = 0;
                }

                ret = umq_rearm_interrupt(umqh, false, &interrupt_option);
                if (ret < 0) {
                    LOG_PRINT("umq_rearm_interrupt failed, ret=%d\n", ret);
                    goto CLOSE_EPOLL_FD;
                }
                // poll_tx if necessary
            } else {
                interrupt_option.direction = UMQ_IO_TX;
                cq_num = umq_get_cq_event(umqh, &interrupt_option);
                if (cq_num < 0) {
                    LOG_PRINT("umq_get_cq_event failed, cq_num=%d\n", cq_num);
                    goto CLOSE_EPOLL_FD;
                }

                do {
                    poll_num = umq_poll(umqh, UMQ_IO_TX, buf, UMQ_POST_POLL_BATCH);
                    if (poll_num < 0) {
                        LOG_PRINT("umq_poll failed, ret=%d\n", poll_num);
                        goto CLOSE_EPOLL_FD;
                    }

                    tx_depth += poll_num;

                    for (int j = 0; j < poll_num; j++) {
                        if (buf[j]->status != UMQ_BUF_SUCCESS) {
                            LOG_PRINT("umq_poll failed, tx buf status=%d\n", buf[j]->status);
                            goto CLOSE_EPOLL_FD;
                        }

                        send_size += buf[j]->data_size;
                        buf[j]->qbuf_next = NULL;
                        umq_buf_free(buf[j]);

                        if (send_size == lat_arg->cfg->req_size) {
                            can_send = true;
                            send_qbuf = umq_buf_alloc(lat_arg->cfg->req_size, 1, UMQ_INVALID_HANDLE, NULL);
                            if (send_qbuf == NULL) {
                                goto CLOSE_EPOLL_FD;
                            }
                            head = send_qbuf;
                            while (head != NULL) {
                                buf_pro = (umq_buf_pro_t *)head->qbuf_ext;
                                buf_pro->opcode = UMQ_OPC_SEND;
                                buf_pro->flag.value = 0;
                                buf_pro->user_ctx = 0;
                                buf_pro->flag.bs.solicited_enable = 1;
                                buf_pro->flag.bs.complete_enable = 1;

                                head = head->qbuf_next;
                            }

                            send_size = 0;
                        } else if (send_size > lat_arg->cfg->req_size) {
                            LOG_PRINT("umq_poll failed, send_size=%lu larger than req_size=%u\n", send_size,
                                      lat_arg->cfg->req_size);
                            goto CLOSE_EPOLL_FD;
                        }
                    }
                } while (poll_num == UMQ_POST_POLL_BATCH);

                umq_ack_interrupt(umqh, cq_num, &interrupt_option);

                ret = umq_rearm_interrupt(umqh, false, &interrupt_option);
                if (ret < 0) {
                    LOG_PRINT("umq_rearm_interrupt failed, ret=%d\n", ret);
                    goto CLOSE_EPOLL_FD;
                }
            }
        }

        // send tx
        if (can_send && tx_depth > 0 && send_qbuf != NULL) {
            bad_qbuf = NULL;
            ret = umq_post(umqh, send_qbuf, UMQ_IO_TX, &bad_qbuf);
            if (ret == 0) {
                can_send = false;
                tx_depth -= umq_tx_qbuf_cnt(send_qbuf, NULL);
                send_cnt++;
                LOG_PRINT("umq_post %uth success\n", send_cnt);
                continue;
            }

            if (ret == -UMQ_ERR_EAGAIN) {
                tx_depth -= umq_tx_qbuf_cnt(send_qbuf, bad_qbuf);
                send_qbuf = bad_qbuf;
                continue;
            }

            LOG_PRINT("umq_post failed, ret=%d\n", ret);
            goto CLOSE_EPOLL_FD;
        }
    }

CLOSE_EPOLL_FD:
    close(epoll_fd);
}
