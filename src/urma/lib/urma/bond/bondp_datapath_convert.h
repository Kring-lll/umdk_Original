/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bond provider datapath convert header file
 * Author: Wang Hang
 * Create: 2026-04-02
 * Note:
 * History: 2026-04-02   Create File
 */

#ifndef BONDP_DATAPATH_CONVERT_H
#define BONDP_DATAPATH_CONVERT_H

#include <stdint.h>

#include "bondp_connection.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline bool is_rw_wr(const urma_jfs_wr_t *wr)
{
    return wr->opcode == URMA_OPC_WRITE || wr->opcode == URMA_OPC_WRITE_IMM ||
           wr->opcode == URMA_OPC_WRITE_NOTIFY || wr->opcode == URMA_OPC_READ;
}
static inline bool is_send_wr(const urma_jfs_wr_t *wr)
{
    return wr->opcode == URMA_OPC_SEND || wr->opcode == URMA_OPC_SEND_IMM ||
           wr->opcode == URMA_OPC_SEND_INVALIDATE;
}
static inline bool is_atomic_wr(const urma_jfs_wr_t *wr)
{
    return wr->opcode == URMA_OPC_CAS || wr->opcode == URMA_OPC_FADD;
}

static inline uint32_t jfs_wr_src_num_sge(const urma_jfs_wr_t *wr)
{
    if (is_rw_wr(wr)) {
        return wr->rw.src.num_sge;
    }
    if (is_send_wr(wr)) {
        return wr->send.src.num_sge;
    }
    return 1; // atomic ops use single SGE
}

/* Data bytes carried by one WR on the initiator port.
 * WRITE/SEND: src SGE length (payload). READ: src SGE is the remote source,
 * its length equals the response bytes that return on the same port pair.
 * Atomic ops use a fixed small weight. */
static inline uint64_t jfs_wr_total_bytes(const urma_jfs_wr_t *wr)
{
    uint64_t total = 0;
    if (is_rw_wr(wr)) {
        if (wr->rw.src.sge != NULL) {
            for (uint32_t i = 0; i < wr->rw.src.num_sge; i++) {
                total += wr->rw.src.sge[i].len;
            }
        }
    } else if (is_send_wr(wr)) {
        if (wr->send.src.sge != NULL) {
            for (uint32_t i = 0; i < wr->send.src.num_sge; i++) {
                total += wr->send.src.sge[i].len;
            }
        }
    } else {
        total = 8; /* CAS/FADD fixed weight */
    }
    return total;
}

static inline uint64_t jfs_wr_list_total_bytes(const urma_jfs_wr_t *wr)
{
    uint64_t total = 0;
    while (wr != NULL) {
        total += jfs_wr_total_bytes(wr);
        wr = wr->next;
    }
    return total;
}

urma_status_t copy_jfs_wr(const urma_jfs_wr_t *src, urma_jfs_wr_t *dst,
                          urma_sge_t *prealloc_src_sge, urma_sge_t *prealloc_dst_sge,
                          uint32_t max_sge, uint32_t max_rsge);
urma_status_t copy_jfr_wr(const urma_jfr_wr_t *src, urma_jfr_wr_t *dst,
                          urma_sge_t *prealloc_src_sge, uint32_t max_sge);

void free_jfr_wr(urma_jfr_wr_t *wr);
void free_jfs_wr(urma_jfs_wr_t *wr);

void encode_jfs_wr_msn(urma_jfs_wr_t *wr, bondp_comp_t *bdp_comp, uint32_t msn, bool enable_msn);

urma_status_t check_jfs_wr_path(urma_jfs_wr_t *wr, int send_idx, int target_idx);

void convert_jfs_vwr_to_pwr(urma_jfs_wr_t *wr, int send_idx, int target_idx);
void convert_jfs_pwr_to_vwr(urma_jfs_wr_t *wr, urma_target_jetty_t *vtjetty);

void convert_jfr_vwr_to_pwr(urma_jfr_wr_t *wr, int recv_idx);
void convert_pcr_to_vcr(urma_cr_t *cr, bondp_context_t *bdp_ctx, uint32_t *msn);

static inline bool is_recv_cr(const urma_cr_t *cr)
{
    return cr->flag.bs.s_r == 1;
}

/*
 * When the cr status is URMA_CR_WR_SUSPEND_DONE or URMA_CR_WR_FLUSH_ERR_DONE,
 * it indicates that the CR is a fake one constructed by hardware.
 * At this time, the `urma_ctx` field in CR is invalid and most likely 0.
 */
static inline bool is_fake_cr(const urma_cr_t *cr)
{
    return cr->status == URMA_CR_WR_SUSPEND_DONE ||
           cr->status == URMA_CR_WR_FLUSH_ERR_DONE;
}

/*
 * We currently consider the following status codes on the sender side
 * as indicators of a fault that should recover.
 */
static inline bool is_failover_cr(const urma_cr_t *cr)
{
    return cr->status == URMA_CR_LOC_LEN_ERR ||
           cr->status == URMA_CR_LOC_ACCESS_ERR ||
           cr->status == URMA_CR_ACK_TIMEOUT_ERR;
}

static inline bool is_need_rebuild_jetty(const urma_cr_t *cr)
{
    return cr->status == URMA_CR_ACK_TIMEOUT_ERR;
}

static inline bool is_rnr_retry_cr(const urma_cr_t *cr)
{
    return cr->status == URMA_CR_RNR_RETRY_CNT_EXC_ERR;
}

#ifdef __cplusplus
}
#endif

#endif // BONDP_DATAPATH_CONVERT_H
