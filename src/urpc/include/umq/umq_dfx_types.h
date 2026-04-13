/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Public header file of UMQ dfx types
 * Create: 2026-2-4
 * Note:
 * History: 2026-2-4
 */

#ifndef UMQ_DFX_TYPES_H
#define UMQ_DFX_TYPES_H

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include "umq_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UMQ_STATS_QBUF_POOL_TYPE_MAX (16u)
#define UMQ_PERF_QUANTILE_MAX_NUM (8u)
#define UMQ_PERF_REC_MAX_NUM (64u)
#define UMQ_DFX_TO_STRING_DEFAULT_LEN (16384)
#define UMQ_LOCAL_QBUF_POOL_MAX_NUM (64)

typedef struct umq_packet_stats {
    uint64_t send_cnt;              // number of packets sent
    uint64_t send_success;          // number of packets successfully sent
    uint64_t recv_cnt;              // number of packets received
    uint64_t send_error_cnt;        // number of packets failed to send
    uint64_t recv_error_cnt;        // number of packets failed to receive
} umq_packet_stats_t;

typedef struct umq_credit_pool_stats {
    /* current actual statistics of the system */
    uint64_t pool_idle; // the number of credits currently available in the credit pool
    uint64_t pool_be_allocated; // the number of credits currently allocated and taken from the credit pool
    uint64_t rsvd0[2];

    /* cumulative statistics */
    uint64_t total_pool_idle; // the total number of available credits in the current credit pool.
    uint64_t total_pool_be_allocated; // the total number of credits currently allocated and taken from the credit pool
    uint64_t rsvd1[2];

    /* error in statistics */
    uint64_t total_pool_post_rx_err; // the total number of invalid credits (which will cause `pool_idle` to overflow)
    uint64_t rsvd2[2];
} umq_credit_pool_stats_t;

typedef struct umq_credit_private_stats {
    /* current actual statistics at the queue level */
    uint64_t queue_idle; // credits to be allocated to the peer (no practical use, always 0, reserved field)
    uint64_t queue_be_allocated; // credits already be allocated to the peer, used for rx direction receive io
    uint64_t queue_acquired; // credits acquired from the peer, used for tx direction send io
    uint64_t rsvd0[2];

    /* cumulative statistics at the queue level */
    uint64_t total_queue_idle; // the total number of credits to be allocated to the peer
    uint64_t total_queue_acquired; // the total number of credits obtained from the peer
    uint64_t total_queue_be_allocated; // the total number of credits already be allocated to the peer
    uint64_t total_queue_post_tx_success; // the total number of already consumed credits in the tx direction
    uint64_t rsvd1[2];

    /* error in statistics */
    uint64_t total_queue_post_tx_err; // the total number of transmitted io that failed in the tx direction
    uint64_t total_queue_acquired_err; // the total number of credits obtained from the peer that were invalid
    uint64_t rsvd[4];
} umq_credit_private_stats_t;

typedef struct umq_flow_control_stats {
    umq_credit_pool_stats_t pool_credit; // credit-related statistics of a main queue
    umq_credit_private_stats_t queue_credit; // credit-related statistics of a specific queue
    umq_packet_stats_t packet_stats; // flow control sacket statistics
} umq_flow_control_stats_t;


// 扩容内存池统计（新增）
typedef struct umq_expansion_pool_stats {
    uint32_t expansion_count;            // 已扩容次数
    uint32_t slot_count;               // 当前活跃 slot 数
    uint32_t slot_cnt_max;            // 最大 slot 数
    uint64_t exp_total_block_num;     // expansion pool 总块数
    uint64_t exp_total_free_block_num; // expansion pool 空闲块数
    uint64_t exp_total_mem_size;      // expansion pool 总内存
    uint64_t exp_used_mem_size;       // expansion pool 已用内存
    uint64_t total_expansion_count;       // 累计分配次数
    uint64_t total_shrink_count;        // 累计释放次数
} umq_expansion_pool_stats_t;
// 每线程本地池统计（新增）
typedef struct umq_local_qbuf_pool_stats {
    uint64_t tid;                              // 线程 ID
    uint64_t capacity_with_data;               // with_data 容量
    uint64_t buf_cnt_with_data;                // with_data 当前持有数
    uint64_t capacity_without_data;            // without_data 容量
    uint64_t buf_cnt_without_data;             // without_data 当前持有数
    uint64_t tls_fetch_count_with_data;                  // 该线程从全局池获取 buf 的累计次数(with_data)
    uint64_t tls_fetch_buf_cnt_with_data;                // 该线程从全局池获取的累计 buf 数量(with_data)
    uint64_t tls_fetch_count_without_data;              // 该线程从全局池获取 buf 的累计次数(without_data)
    uint64_t tls_fetch_buf_cnt_without_data;            // 该线程从全局池获取的累计 buf 数量(without_data)
    uint64_t tls_return_count_with_data;                 // 该线程向全局池归还的累计次数(with_data)
    uint64_t tls_return_buf_cnt_with_data;               // 该线程向全局池归还的累计 buf 数量(with_data)
    uint64_t tls_return_count_without_data;             // 该线程向全局池归还的累计次数(without_data)
    uint64_t tls_return_buf_cnt_without_data;           // 该线程向全局池归还的累计 buf 数量(without_data)
    uint64_t alloc_count_with_data;                      // 该线程累计申请 buf 次数(with_data)
    uint64_t alloc_count_without_data;                   // 该线程累计申请 buf 次数(without_data)
    uint64_t free_count_with_data;                       // 该线程累计释放 buf 次数(with_data)
    uint64_t free_count_without_data;                    // 该线程累计释放 buf 次数(without_data)
} umq_local_qbuf_pool_stats_t;
// 扩展后的 umq_qbuf_pool_info_t
typedef struct umq_qbuf_pool_info {
    // === 原有字段 ===
    umq_buf_mode_t mode;
    uint64_t total_size;
    uint64_t total_block_num;
    uint32_t block_size;
    uint32_t headroom_size;
    uint32_t data_size;
    uint32_t buf_size;
    uint32_t umq_buf_t_size;
    union {
        struct {
            uint64_t block_num_with_data;
            uint64_t size_with_data;
            uint64_t block_num_without_data;
            uint64_t size_without_data;
        } split;
        struct {
            uint64_t block_num_with_data;
            uint64_t size_with_data;
        } combine;
    } available_mem;
} umq_qbuf_pool_info_t;

typedef struct umq_qbuf_pool_stats {
    umq_qbuf_pool_info_t qbuf_pool_info[UMQ_STATS_QBUF_POOL_TYPE_MAX]; // statistical information list for qbuf pool
    uint32_t num; // number of valid qbuf pool info structures

    umq_local_qbuf_pool_stats_t local_qbuf_pool_stats[UMQ_LOCAL_QBUF_POOL_MAX_NUM]; // 本地池统计
    uint32_t local_qbuf_pool_num; // 本地池数量
    // === 全局汇总字段（仅第一个 info 有效） ===
    umq_expansion_pool_stats_t exp_pool_with_data;  // with_data 扩容池
    umq_expansion_pool_stats_t exp_pool_without_data; // without_data 扩容池
    uint64_t total_tls_capacity_with_data;          // 所有线程 capacity_with_data 之和(with_data)
    uint64_t total_tls_buf_cnt_with_data;           // 所有线程 buf_cnt_with_data 之和(with_data)
    uint64_t total_tls_capacity_without_data;       // 所有线程 capacity_without_data 之和(with_data)
    uint64_t total_tls_buf_cnt_without_data;        // 所有线程 buf_cnt_without_data 之和(with_data)
    uint64_t total_tls_fetch_count_with_data;     // 所有线程 tls_fetch_count 之和(with_data)
    uint64_t total_tls_fetch_buf_cnt_with_data;   // 所有线程 tls_fetch_buf_cnt 之和(with_data)
    uint64_t total_tls_fetch_count_without_data;  // 所有线程 tls_fetch_count 之和(without_data)
    uint64_t total_tls_fetch_buf_cnt_without_data;// 所有线程 tls_fetch_buf_cnt 之和(without_data)
    uint64_t total_tls_return_count_with_data;    // 所有线程 tls_return_count 之和(with_data)
    uint64_t total_tls_return_buf_cnt_with_data; // 所有线程 tls_return_buf_cnt 之和(with_data)
    uint64_t total_tls_return_count_without_data; // 所有线程 tls_return_count 之和(without_data)
    uint64_t total_tls_return_buf_cnt_without_data;// 所有线程 tls_return_buf_cnt 之和(without_data)
    uint64_t total_alloc_count_with_data;          // 所有线程 alloc_count 之和(with_data)
    uint64_t total_alloc_count_without_data;       // 所有线程 alloc_count 之和(without_data)
    uint64_t total_free_count_with_data;           // 所有线程 free_count 之和(with_data)
    uint64_t total_free_count_without_data;        // 所有线程 free_count 之和(without_data)
} umq_qbuf_pool_stats_t;

typedef struct umq_info {
    umq_trans_mode_t trans_mode; // transmission mode of the queue
    union {
        struct {
            uint32_t umq_id; // the ID of the UMQ
            umq_eid_t eid; // the EID used by the UMQ
            char dev_name[UMQ_DEV_NAME_SIZE]; // the UDMA device name corresponding to the EID
            uint32_t local_io_jetty_id; // the I/O jetty ID within the UMQ
            uint32_t local_fc_jetty_id; // the flow control jetty ID within the UMQ
            uint32_t remote_io_jetty_id; // the I/O jetty ID of the peer UMQ that is bound to the local UMQ
            uint32_t remote_fc_jetty_id; // the flow control jetty ID of the peer UMQ that is bound to the local UMQ
        } ub;
    };
} umq_info_t;

typedef enum umq_perf_record_type {
    /* record point for umq_enqueue */
    UMQ_PERF_RECORD_ENQUEUE,
    /* record point for umq_dequeue */
    UMQ_PERF_RECORD_DEQUEUE,
    /* record point for umq_dequeue empty */
    UMQ_PERF_RECORD_DEQUEUE_EMPTY,
    /* record point for umq_post_all */
    UMQ_PERF_RECORD_POST_ALL,
    /* record point for umq_post_tx */
    UMQ_PERF_RECORD_POST_TX,
    /* record point for umq_post_rx */
    UMQ_PERF_RECORD_POST_RX,
    /* record point for umq_poll_all */
    UMQ_PERF_RECORD_POLL_ALL,
    /* record point for umq_poll_tx */
    UMQ_PERF_RECORD_POLL_TX,
    /* record point for umq_poll_rx */
    UMQ_PERF_RECORD_POLL_RX,
    /* record point for umq_poll_all when poll is empty */
    UMQ_PERF_RECORD_POLL_ALL_EMPTY,
    /* record point for umq_poll_tx when tx is empty */
    UMQ_PERF_RECORD_POLL_TX_EMPTY,
    /* record point for umq_poll_rx when rx is empty */
    UMQ_PERF_RECORD_POLL_RX_EMPTY,
    /* record point for umq_rearm_interrupt tx */
    UMQ_PERF_RECORD_REARM_TX,
    /* record point for umq_rearm_interrupt rx */
    UMQ_PERF_RECORD_REARM_RX,
    /* record point for umq_wait_interrupt rx */
    UMQ_PERF_RECORD_WAIT_TX,
    /* record point for umq_wait_interrupt tx */
    UMQ_PERF_RECORD_WAIT_RX,
    /* record point for umq_ack_interrupt tx */
    UMQ_PERF_RECORD_ACK_TX,
    /* record point for umq_ack_interrupt rx */
    UMQ_PERF_RECORD_ACK_RX,
    /* record point for umq_notify */
    UMQ_PERF_RECORD_NOTIFY,
    /* record point for transport post send in umq_enqueue and umq_post */
    UMQ_PERF_RECORD_TRANSPORT_POST_SEND,
    /* record point for transport post recv in umq_enqueue and umq_post */
    UMQ_PERF_RECORD_TRANSPORT_POST_RECV,
    /* record point for transport poll tx in umq_dequeue and umq_poll */
    UMQ_PERF_RECORD_TRANSPORT_POLL_TX,
    /* record point for transport poll rx in umq_dequeue and umq_poll */
    UMQ_PERF_RECORD_TRANSPORT_POLL_RX,
    /* record point for transport poll tx in umq_dequeue and umq_poll when tx is empty */
    UMQ_PERF_RECORD_TRANSPORT_POLL_TX_EMPTY,
    /* record point for transport poll rx in umq_dequeue and umq_poll when rx is empty */
    UMQ_PERF_RECORD_TRANSPORT_POLL_RX_EMPTY,
    /* record point for transport rearm interrupt tx */
    UMQ_PERF_RECORD_TRANSPORT_REARM_TX,
    /* record point for transport rearm interrupt rx */
    UMQ_PERF_RECORD_TRANSPORT_REARM_RX,
    /* record point for transport wait interrupt rx */
    UMQ_PERF_RECORD_TRANSPORT_WAIT_TX,
    /* record point for transport wait interrupt tx */
    UMQ_PERF_RECORD_TRANSPORT_WAIT_RX,
    /* record point for transport ack interrupt tx */
    UMQ_PERF_RECORD_TRANSPORT_ACK_TX,
    /* record point for transport ack interrupt rx */
    UMQ_PERF_RECORD_TRANSPORT_ACK_RX,
    UMQ_PERF_RECORD_TYPE_MAX,
} umq_perf_record_type_t;

typedef struct umq_perf_stats {
    struct {
        umq_perf_record_type_t type; // types of probe points supported by perf probe
        uint64_t sample_num; // statistical count
        uint64_t average; // average latency
        uint64_t mininum; // min latency
        uint64_t maxinum; // max latency
        uint64_t median; // median latency
        uint64_t p90; // 90th percentile
        uint64_t p99; // 99th percentile
    } type_record[UMQ_PERF_RECORD_TYPE_MAX]; // statistical results list for each type of probe poin
} umq_perf_stats_t;

typedef struct umq_perf_stats_cfg {
    uint64_t thresh_array[UMQ_PERF_QUANTILE_MAX_NUM]; // quantile values list
    uint32_t thresh_num; // number of valid quantiles
} umq_perf_stats_cfg_t;

#ifdef __cplusplus
}
#endif

#endif