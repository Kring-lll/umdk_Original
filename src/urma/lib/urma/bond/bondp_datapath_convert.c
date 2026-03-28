/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bond provider datapath convert implementation file
 * Author: Wang Hang
 * Create: 2026-04-02
 * Note:
 * History: 2026-04-02   Create File
 */

#include "bondp_types.h"
#include "urma_log.h"

#include "bondp_datapath_convert.h"

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
int copy_jfr_wr(const urma_jfr_wr_t *src, urma_jfr_wr_t *dst,
                urma_sge_t *prealloc_src_sge)
{
    *dst = *src;
    dst->next = NULL;

    if (copy_sg_list(&src->src, &dst->src, prealloc_src_sge) != 0) {
        return -1;
    }
    return 0;
}

#define WRITE_IMM_USER_BITS (32)
#define IMM_OPCODE_SHIFT    (56)

static inline uint64_t encode_imm_data(uint64_t imm_data, uint32_t msn, uint64_t opcode_tag)
{
    imm_data &= ((uint64_t)1 << WRITE_IMM_USER_BITS) - 1;
    imm_data |= (uint64_t)msn << WRITE_IMM_USER_BITS;
    imm_data |= opcode_tag << IMM_OPCODE_SHIFT;
    return imm_data;
}

static inline urma_target_jetty_t *get_p_tjetty(urma_target_jetty_t *tjetty, int send_idx, int target_idx)
{
    return CONTAINER_OF_FIELD(tjetty, bondp_target_jetty_t, v_tjetty)->p_tjetty[send_idx][target_idx];
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

/** Change vseg and vtjetty to pseg and ptjetty in opcode URMA_OPC_SEND_*.
 * We store the original vtseg and vtjetty in send_wr in wr_buf and set pseg and ptjetty in tmp_wr.
 * This is useful when we need to migrate a WR to another dev.
 */
static urma_status_t set_send_wr_ptseg_ptjetty(urma_jfs_wr_t *send_wr, urma_target_jetty_t *vtjetty,
                                               int send_idx, int target_idx)
{
    for (int i = 0; i < send_wr->send.src.num_sge; ++i) {
        urma_target_seg_t *vtseg = (urma_target_seg_t *)send_wr->send.src.sge[i].tseg;
        if (vtseg == NULL) {
            if (send_wr->flag.bs.inline_flag != 0) {
                continue;
            }
            URMA_LOG_ERR("Failed to set ptseg, vtseg is NULL\n");
            return URMA_EINVAL;
        }
        vtseg = (urma_target_seg_t *)vtseg->handle;
        send_wr->send.src.sge[i].tseg = get_p_tseg(vtseg, send_idx, target_idx);
    }
    send_wr->tjetty = get_p_tjetty(vtjetty, send_idx, target_idx);
    return URMA_SUCCESS;
}

/** Change vseg and vtjetty to pseg and ptjetty in opcode URMA_OPC_WRITE
 * We store the original vseg and vtjetty in send_wr in wr_buf and set pseg and ptjetty in tmp_wr.
 * This is useful when we need to migrate a WR to another dev.
 */
static urma_status_t set_write_wr_ptseg_ptjetty(urma_jfs_wr_t *send_wr, urma_target_jetty_t *vtjetty,
                                                int send_idx, int target_idx)
{
    urma_target_seg_t *vtseg = NULL;
    for (int i = 0; i < send_wr->rw.src.num_sge; ++i) {
        vtseg = (urma_target_seg_t *)send_wr->rw.src.sge[i].tseg;
        if (vtseg == NULL) {
            if (send_wr->flag.bs.inline_flag != 0) {
                continue;
            }
            URMA_LOG_ERR("Failed to set ptseg, vtseg is NULL\n");
            return URMA_EINVAL;
        }
        send_wr->rw.src.sge[i].tseg = get_p_tseg(vtseg, send_idx, target_idx);
    }
    for (int i = 0; i < send_wr->rw.dst.num_sge; ++i) {
        vtseg = (urma_target_seg_t *)send_wr->rw.dst.sge[i].tseg;
        if (vtseg == NULL) {
            if (send_wr->flag.bs.inline_flag != 0) {
                continue;
            }
            URMA_LOG_ERR("Failed to set ptseg, vtseg is NULL\n");
            return URMA_EINVAL;
        }
        send_wr->rw.dst.sge[i].tseg = get_p_tseg(vtseg, send_idx, target_idx);
    }
    send_wr->tjetty = get_p_tjetty(vtjetty, send_idx, target_idx);
    return URMA_SUCCESS;
}

/** Change vseg and vtjetty to pseg and ptjetty in opcode URMA_OPC_CAS
 * We store the original vseg and vtjetty in send_wr in wr_buf and set pseg and ptjetty in tmp_wr.
 * This is useful when we need to migrate a WR to another dev.
 */
static urma_status_t set_cas_wr_ptseg_pjetty(urma_jfs_wr_t *send_wr, urma_target_jetty_t *vtjetty,
                                             int send_idx, int target_idx)
{
    if (send_wr->cas.src == NULL || send_wr->cas.dst == NULL) {
        URMA_LOG_ERR("when set cas_wr, one of src or dst is NULL.\n");
        return URMA_EINVAL;
    }
    urma_target_seg_t *vtseg = NULL;
    vtseg = (urma_target_seg_t *)send_wr->cas.src->tseg;
    if (vtseg == NULL) {
        URMA_LOG_ERR("Failed to set ptseg, vtseg is NULL\n");
        return URMA_EINVAL;
    }

    vtseg = (urma_target_seg_t *)vtseg->handle;
    send_wr->cas.src->tseg = get_p_tseg(vtseg, send_idx, target_idx);
    vtseg = (urma_target_seg_t *)send_wr->cas.dst->tseg;
    if (vtseg == NULL) {
        URMA_LOG_ERR("Failed to set ptseg, vtseg is NULL\n");
        return URMA_EINVAL;
    }
    send_wr->cas.dst->tseg = get_p_tseg(vtseg, send_idx, target_idx);

    send_wr->tjetty = get_p_tjetty(vtjetty, send_idx, target_idx);
    return URMA_SUCCESS;
}

/** Change vseg and vtjetty to pseg and ptjetty in opcode URMA_OPC_FADD
 * We store the original vseg and vtjetty in send_wr in wr_buf and set pseg and ptjetty in tmp_wr.
 * This is useful when we need to migrate a WR to another dev.
 */
static urma_status_t set_fadd_wr_ptseg_pjetty(urma_jfs_wr_t *send_wr, urma_target_jetty_t *vtjetty,
                                              int send_idx, int target_idx)
{
    if (send_wr->faa.src == NULL || send_wr->faa.dst == NULL) {
        URMA_LOG_ERR("when set faa_wr, one of src or dst is NULL.\n");
        return URMA_EINVAL;
    }
    urma_target_seg_t *vtseg = NULL;
    vtseg = (urma_target_seg_t *)send_wr->faa.src->tseg;
    if (vtseg == NULL) {
        URMA_LOG_ERR("Failed to set ptseg, vtseg is NULL\n");
        return URMA_EINVAL;
    }

    vtseg = (urma_target_seg_t *)vtseg->handle;
    send_wr->faa.src->tseg = get_p_tseg(vtseg, send_idx, target_idx);

    vtseg = (urma_target_seg_t *)send_wr->faa.dst->tseg;
    if (vtseg == NULL) {
        URMA_LOG_ERR("Failed to set ptseg, vtseg is NULL\n");
        return URMA_EINVAL;
    }

    vtseg = (urma_target_seg_t *)send_wr->faa.dst->tseg->handle;
    send_wr->faa.dst->tseg = get_p_tseg(vtseg, send_idx, target_idx);
    send_wr->tjetty = get_p_tjetty(vtjetty, send_idx, target_idx);
    return URMA_SUCCESS;
}

int convert_jfs_vwr_to_pwr_nostore(urma_jfs_wr_t *wr, int send_idx, int target_idx)
{
    urma_status_t ret = URMA_SUCCESS;
    switch (wr->opcode) {
        case URMA_OPC_SEND:
        case URMA_OPC_SEND_IMM:
        case URMA_OPC_SEND_INVALIDATE:
            return set_send_wr_ptseg_ptjetty(wr, wr->tjetty, send_idx, target_idx);
        case URMA_OPC_WRITE:
        case URMA_OPC_WRITE_IMM:
        case URMA_OPC_WRITE_NOTIFY:
        case URMA_OPC_READ:
            return set_write_wr_ptseg_ptjetty(wr, wr->tjetty, send_idx, target_idx);
        case URMA_OPC_CAS:
            return set_cas_wr_ptseg_pjetty(wr, wr->tjetty, send_idx, target_idx);
        case URMA_OPC_FADD:
            return set_fadd_wr_ptseg_pjetty(wr, wr->tjetty, send_idx, target_idx);
        default:
            URMA_LOG_ERR("Unsupported send opcode\n");
            return URMA_EINVAL;
    }
    return ret;
}

int convert_jfs_vwr_to_pwr_store(urma_jfs_wr_t *wr, int send_idx, int target_idx, bdp_v_conn_t *v_conn)
{
    /* We need cqe to do deduplication, so we have to set this flag */
    wr->flag.bs.complete_enable = 1;

    if (is_rw_wr(wr)) {
        if (wr->opcode == URMA_OPC_WRITE_NOTIFY || wr->opcode == URMA_OPC_WRITE_IMM) {
            urma_opcode_t original_opcode = wr->opcode;
            wr->opcode = URMA_OPC_WRITE_IMM;

            uint64_t opcode_tag = 0;
            if (original_opcode == URMA_OPC_WRITE_IMM) {
                opcode_tag = URMA_CR_OPC_WRITE_WITH_IMM;
            }

            wr->rw.notify_data = encode_imm_data(wr->rw.notify_data, v_conn->msn, opcode_tag);
            v_conn->msn = (v_conn->msn + 1) % BONDP_MAX_BITMAP_SIZE;
        }
        return set_write_wr_ptseg_ptjetty(wr, wr->tjetty, send_idx, target_idx);
    } else if (is_send_wr(wr)) {
        urma_opcode_t original_opcode = wr->opcode;
        wr->opcode = URMA_OPC_SEND_IMM;

        uint64_t opcode_tag = 0;
        if (original_opcode == URMA_OPC_SEND) {
            opcode_tag = URMA_CR_OPC_SEND;
        } else if (original_opcode == URMA_OPC_SEND_IMM) {
            opcode_tag = URMA_CR_OPC_SEND_WITH_IMM;
        } else if (original_opcode == URMA_OPC_SEND_INVALIDATE) {
            opcode_tag = URMA_CR_OPC_SEND_WITH_INV;
        }

        wr->send.imm_data = encode_imm_data(wr->send.imm_data, v_conn->msn, opcode_tag);
        v_conn->msn = (v_conn->msn + 1) % BONDP_MAX_BITMAP_SIZE;

        return set_send_wr_ptseg_ptjetty(wr, wr->tjetty, send_idx, target_idx);
    } else {
        URMA_LOG_ERR("unsupport opcode %d\n", wr->opcode);
        return -1;
    }
}

int convert_jfr_vwr_to_pwr_nostore(urma_jfr_wr_t *recv_wr, int recv_idx)
{
    /* set user sge */
    for (int i = 0; i < recv_wr->src.num_sge; ++i) {
        if (recv_wr->src.sge[i].tseg == NULL) {
            URMA_LOG_ERR("Recv sge[%d] has NULL tseg\n", i);
            return URMA_EINVAL;
        }
        recv_wr->src.sge[i].tseg = get_p_tseg(recv_wr->src.sge[i].tseg, recv_idx, 0);
    }
    return URMA_SUCCESS;
}

int convert_jfr_vwr_to_pwr_store(urma_jfr_wr_t *wr, int recv_idx)
{
    for (int i = 0; i < wr->src.num_sge; ++i) {
        wr->src.sge[i].tseg = get_p_tseg(wr->src.sge[i].tseg, recv_idx, 0);
    }
    return 0;
}
