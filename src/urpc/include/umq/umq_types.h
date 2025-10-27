/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: Public header file of UMQ function types
 * Create: 2025-7-7
 * Note:
 * History: 2025-7-7
 */

#ifndef UMQ_TYPES_H
#define UMQ_TYPES_H
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum umq_buf_mode {
    UMQ_BUF_SPLIT,                  // umq_buf_t and buf is split
    UMQ_BUF_COMBINE,                // umq_buf_t and buf is combine
} umq_buf_mode_t;

typedef enum umq_io_direction {
    UMQ_IO_ALL = 0,
    UMQ_IO_TX,
    UMQ_IO_RX,
    UMQ_IO_MAX,
} umq_io_direction_t;

typedef enum umq_queue_mode {
    UMQ_MODE_POLLING,             // polling mode
    UMQ_MODE_INTERRUPT,           // interrupt mode
    UMQ_MODE_MAX
} umq_queue_mode_t;

typedef enum umq_trans_mode {
    UMQ_TRANS_MODE_UB = 0,              // ub, max io size 64K
    UMQ_TRANS_MODE_IB,                  // ib, max io size 64K
    UMQ_TRANS_MODE_UCP,                 // ub offload, max io size 64K
    UMQ_TRANS_MODE_IPC,                 // local ipc, max io size 10M
    UMQ_TRANS_MODE_UBMM,                // ub share memory, max io size 8K
    UMQ_TRANS_MODE_UB_PLUS,             // ub, max io size 10M
    UMQ_TRANS_MODE_IB_PLUS,             // ib, max io size 10M
    UMQ_TRANS_MODE_UBMM_PLUS,           // ub share memory, max io size 10M
    UMQ_TRANS_MODE_MAX,
} umq_trans_mode_t;

typedef enum umq_dev_assign_mode {
    UMQ_DEV_ASSIGN_MODE_IPV4,
    UMQ_DEV_ASSIGN_MODE_IPV6,
    UMQ_DEV_ASSIGN_MODE_EID,
    UMQ_DEV_ASSIGN_MODE_DEV,
    UMQ_DEV_ASSIGN_MODE_MAX
} umq_dev_assign_mode_t;

#define UMQ_EID_SIZE                 (16)
#define UMQ_IPV4_SIZE                (16)
#define UMQ_IPV6_SIZE                (46)
#define UMQ_DEV_NAME_SIZE            (64)
#define UMQ_MAX_BIND_INFO_SIZE       (256)
#define UMQ_BATCH_SIZE               (64)

#define UMQ_INTERRUPT_FLAG_IO_DIRECTION         (1)         // enable arg direction

typedef struct umq_interrupt_option {
    uint32_t flag;                      // indicates which below property takes effect
    umq_io_direction_t direction;
} umq_interrupt_option_t;

typedef union umq_eid {
    uint8_t raw[UMQ_EID_SIZE];      // Network Order
    struct {
        uint64_t reserved;          // If IPv4 mapped to IPv6, == 0
        uint32_t prefix;            // If IPv4 mapped to IPv6, == 0x0000ffff
        uint32_t addr;              // If IPv4 mapped to IPv6, == IPv4 addr
    } in4;
    struct {
        uint64_t subnet_prefix;
        uint64_t interface_id;
    } in6;
} umq_eid_t;

typedef struct umq_dev_assign {
    umq_dev_assign_mode_t assign_mode;  // Decide how to choose a device
    union {
        struct {
            char ip_addr[UMQ_IPV4_SIZE];
        } ipv4;
        struct {
            char ip_addr[UMQ_IPV6_SIZE];
        } ipv6;
        struct {
            umq_eid_t eid;
        } eid;
        struct {
            char dev_name[UMQ_DEV_NAME_SIZE];
        } dev;
    };
} umq_dev_assign_t;

typedef struct umq_trans_info {
    umq_trans_mode_t trans_mode;
    umq_dev_assign_t dev_info;
} umq_trans_info_t;

#define MAX_UMQ_TRANS_INFO_NUM (128)

/* umq feature */
#define UMQ_FEATURE_API_BASE                (0)         // enable base feature. set when use umq_enqueue/umq_dequeue
#define UMQ_FEATURE_API_PRO                 (1)         // enable pro feature. set when use umq_post/umq_poll
#define UMQ_FEATURE_ENABLE_TOKEN_POLICY (1 << 1)        // enable token policy.

typedef struct umq_init_cfg {
    umq_buf_mode_t buf_mode;
    uint32_t feature;               // feature flags
    uint16_t headroom_size;         // header size of umq buffer
    bool io_lock_free;              // true: user should ensure thread safety when call io function
    uint8_t trans_info_num;
    uint16_t order_type;
    uint16_t eid_idx;
    uint16_t cna;
    uint32_t ubmm_eid;
    umq_trans_info_t trans_info[MAX_UMQ_TRANS_INFO_NUM];
} umq_init_cfg_t;

#define UMQ_NAME_MAX_LEN (32)

#define UMQ_CREATE_FLAG_RX_BUF_SIZE         (1)             // enable arg rx_buf_size when create umq
#define UMQ_CREATE_FLAG_TX_BUF_SIZE         (1 << 1)        // enable arg tx_buf_size when create umq
#define UMQ_CREATE_FLAG_RX_DEPTH            (1 << 2)        // enable arg rx_depth when create umq
#define UMQ_CREATE_FLAG_TX_DEPTH            (1 << 3)        // enable arg tx_depth when create umq
#define UMQ_CREATE_FLAG_QUEUE_MODE          (1 << 4)        // enable arg mode when create umq
#define UMQ_CREATE_FLAG_IS_LOCAL_IPC        (1 << 5)        // enable arg is_local_ipc when create umq
#define UMQ_CREATE_FLAG_OWNER               (1 << 6)        // enable arg owner when create umq

typedef struct umq_create_option {
    /*************Required paramenters start*****************/
    umq_trans_mode_t trans_mode;
    umq_dev_assign_t dev_info;
    char name[UMQ_NAME_MAX_LEN];     // include '\0', size of valid name is UMQ_NAME_MAX_LEN - 1

    uint32_t create_flag;            // indicates which below creation property takes effect
    /*************Required paramenters end*******************/
    /*************Optional paramenters start*****************/
    uint32_t rx_buf_size;
    uint32_t tx_buf_size;
    uint32_t rx_depth;
    uint32_t tx_depth;

    umq_queue_mode_t mode;      // mode of queue, QUEUE_MODE_POLLING for default
    bool is_local_ipc;          // whether peer is local ipc
    bool owner;                 // whether current side is owner of shared memory
    /*************Optional paramenters end*******************/
} umq_create_option_t;

/**
 * layout: | umq_buf_t | headroom | data |  unuse |
 * buf_size = sizeof(umq_buf_t) + headroom_size + data_size +  sizeof(unuse)
 * total_data_size of one qbuf = data_size + data_size of qbuf_next  + data_size of qbuf_next + ...
 * we can also list multi-qbufs with qbuf_next
 */
typedef struct umq_buf umq_buf_t;
struct umq_buf {
    // cache line 0 : 64B
    umq_buf_t *qbuf_next;

    uint64_t umqh;                        // umqh which buf alloc from

    uint32_t total_data_size;             // size of all umq buf data
    uint32_t buf_size;                    // size of current umq buf

    uint32_t data_size;                   // size of umq buf data
    uint16_t headroom_size;               // size of umq buf headroom
    uint16_t rsvd1;

    uint32_t token_id;                    // token_id for reference operation
    uint32_t token_value;                 // token_value for reference operation

    uint64_t status : 32;                 // status of umq buf
    uint64_t io_direction : 2;            // 0: no direction; 1: tx qbuf; 2: rx qbuf
    uint64_t write_flag : 1;
    uint64_t rsvd2 : 29;

    uint64_t rsvd3;

    char *buf_data;                       // point to data[0]

    // cache line 1 : 64B
    uint64_t qbuf_ext[8];                 // extern data, etc: umq_buf_pro_t

    char data[0];                         // size of data should be data_size
};

#define UMQ_ALLOC_FLAG_HEAD_ROOM_SIZE         (1)             // enable arg headroom_size

typedef struct umq_alloc_option {
    uint32_t flag;                          // indicates which below property takes effect
    uint16_t headroom_size;
} umq_alloc_option_t;

#ifdef __cplusplus
}
#endif

#endif
