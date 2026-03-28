/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: Bond provider wr buf header file
 * Author: Wang Hang
 * Create: 2026-04-02
 * Note:
 * History: 2026-04-02   Create File
 */

#include "bondp_types.h"
#include "urma_log.h"

#include "bondp_convert_vwr_to_pwr.h"

void free_jfs_wr(urma_jfs_wr_t *wr)
{
    if (is_rw_wr(wr)) {
        if (wr->rw.src.sge != NULL) {
            free(wr->rw.src.sge);
            wr->rw.src.sge = NULL;
        }
        if (wr->rw.dst.sge != NULL) {
            free(wr->rw.dst.sge);
            wr->rw.dst.sge = NULL;
        }
    } else if (is_send_wr(wr)) {
        if (wr->send.src.sge != NULL) {
            free(wr->send.src.sge);
            wr->send.src.sge = NULL;
        }
    }
}

void free_jfr_wr(urma_jfr_wr_t *wr)
{
    if (wr->src.sge != NULL) {
        free(wr->src.sge);
        wr->src.sge = NULL;
    }
}

// Pre-allocated sge array to improve datapath performance, which can avoid malloc/free for each WR copy.

static int copy_sg_list(const urma_sg_t *src, urma_sg_t *dst, urma_sge_t *prealloc_sge)
{
    dst->num_sge = src->num_sge;
    dst->sge = NULL;

    if (prealloc_sge != NULL) {
        dst->sge = prealloc_sge;
    } else if (dst->num_sge > 0) {
        dst->sge = (urma_sge_t *)malloc(dst->num_sge * sizeof(urma_sge_t));
    }

    if (dst->sge == NULL) {
        return -1;
    }
    (void)memcpy(dst->sge, src->sge, src->num_sge * sizeof(urma_sge_t));
    // for (int i = 0; i < dst->num_sge; i++) {
    //     dst->sge[i] = src->sge[i];
    // }

    return 0;
}

/**
 * Performs a deep copy of a JFS work request.
 *
 * Copies all fields from @src to @dst except:
 *   next_wr, target_jetty, target_seg, user_tseg
 *
 * Supported opcodes:
 *   URMA_OPC_WRITE, URMA_OPC_WRITE_IMM, URMA_OPC_WRITE_NOTIFY, URMA_OPC_READ
 *   URMA_OPC_SEND, URMA_OPC_SEND_IMM, URMA_OPC_SEND_INVALIDATE
 */
int copy_jfs_wr(const urma_jfs_wr_t *src, urma_jfs_wr_t *dst,
                urma_sge_t *prealloc_src_sge, urma_sge_t *prealloc_dst_sge)
{
    *dst = *src;
    dst->next = NULL;

    if (is_rw_wr(src)) {
        if (copy_sg_list(&src->rw.src, &dst->rw.src, prealloc_src_sge) != 0 ||
            copy_sg_list(&src->rw.dst, &dst->rw.dst, prealloc_dst_sge) != 0) {
            return -1;
        }
    } else if (is_send_wr(src)) {
        if (copy_sg_list(&src->send.src, &dst->send.src, prealloc_src_sge) != 0) {
            return -1;
        }
    } else {
        return -1;
    }
    return 0;
}

/**
 * Performs a deep copy of a JFR work request.
 */
int copy_jfr_wr(const urma_jfr_wr_t *src, urma_jfr_wr_t *dst)
{
    *dst = *src;
    dst->next = NULL;

    if (copy_sg_list(&src->src, &dst->src, NULL) != 0) {
        return -1;
    }
    return 0;
}

static inline urma_target_seg_t *get_p_tseg(urma_target_seg_t *tseg, int local_idx, int remote_idx)
{
    /* Use token_id to distinguish local register seg and imported seg
       This is useful for write ops */
    if (tseg->token_id != NULL) {
        return CONTAINER_OF_FIELD(tseg, bondp_comp_t, v_tseg)->p_tseg[local_idx];
    } else {
        return CONTAINER_OF_FIELD(tseg, bondp_import_tseg_t, v_tseg)->p_tseg[local_idx][remote_idx];
    }
}

int convert_jfr_vwr_to_pwr(const urma_jfr_wr_t *vwr, urma_jfr_wr_t *pwr, int recv_idx)
{
    if (copy_jfr_wr(vwr, pwr) != 0) {
        URMA_LOG_ERR("Failed to copy jfs wr\n");
        return -1;
    }

    for (int i = 0; i < pwr->src.num_sge; ++i) {
        pwr->src.sge[i].tseg = get_p_tseg(vwr->src.sge[i].tseg, recv_idx, 0);
    }
    return 0;
}
