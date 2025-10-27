/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: urpc IPC base share memory.
 * Create: 2025-5-22
 */

#ifndef UTIL_IPC_H
#define UTIL_IPC_H

#include <stdbool.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdatomic.h>
#else
#include <atomic>
using namespace std;
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_IPC_NAME 31

typedef struct shm_ring_hdr {
    volatile uint32_t pi;
    volatile uint32_t ci;
    atomic_int cq_event_flag;   // 0: 无事件, 1: 有事件
    atomic_int pending_events;  // 事件计数器
} shm_ring_hdr_t;

typedef struct util_ipc {
    char ipc_name[MAX_IPC_NAME + 1];
    int shm_fd;
    uint32_t shm_size;
    bool owner;

    void *shm_tx_ring;
    shm_ring_hdr_t *shm_tx_ring_hdr;
    uint32_t tx_max_buf_size;
    uint32_t tx_depth;

    void *shm_rx_ring;
    shm_ring_hdr_t *shm_rx_ring_hdr;
    uint32_t rx_max_buf_size;
    uint32_t rx_depth;
} util_ipc_t;

typedef struct util_ipc_option {
    bool owner;
    uint32_t tx_max_buf_size;
    uint32_t tx_depth;
    uint32_t rx_max_buf_size;
    uint32_t rx_depth;
    void *addr;
} util_ipc_option_t;

util_ipc_t *util_ipc_create(char *ipc_name, uint32_t ipc_name_len, util_ipc_option_t *opt);

void util_ipc_destroy(util_ipc_t *ipc_h);

int util_ipc_post_tx(util_ipc_t *ipc_h, char *tx_buf, uint32_t tx_buf_size);

int util_ipc_post_tx_batch(util_ipc_t *ipc_h, char **tx_buf, uint32_t *tx_buf_size, uint32_t tx_buf_cnt);

int util_ipc_poll_tx(util_ipc_t *ipc_h, char *tx_buf, uint32_t tx_max_buf_size, uint32_t *avail_buf_size);

int util_ipc_poll_tx_batch(util_ipc_t *ipc_h, char **tx_buf, uint32_t tx_max_buf_size,
                           uint32_t *avail_buf_size, uint32_t max_cnt);

int util_ipc_post_rx(util_ipc_t *ipc_h, char *rx_buf, uint32_t rx_buf_size);

int util_ipc_poll_rx(util_ipc_t *ipc_h, char *rx_buf, uint32_t rx_max_buf_size, uint32_t *avail_buf_size);

int util_ipc_poll_rx_batch(util_ipc_t *ipc_h, char **rx_buf, uint32_t rx_max_buf_size,
                           uint32_t *avail_buf_size, uint32_t max_cnt);

#ifdef __cplusplus
}
#endif

#endif
