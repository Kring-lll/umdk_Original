/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: Bond datapth ops implementation file
 * Author: Ma Chuan
 * Create: 2025-02-19
 * Note:
 * History: 2025-02-19   Create File
 */

#include <threads.h>

#include "urma_log.h"
#include "urma_api.h"
#include "bondp_types.h"
#include "urma_private.h"
#include "wr_buffer.h"
#include "wr_utils.h"
#include "bondp_jetty_ctx.h"
#include "bondp_connection.h"
#include "bondp_context_table.h"
#include "bondp_convert_vwr_to_pwr.h"

#include "bondp_datapath.h"

#define PJETTY_ID_ENCODE_OFFSET (32)
#define VJETTY_ID_ENCODE_OFFSET (48)
#define WRITE_IMM_USER_BITS (32)
#define IMM_OPCODE_SHIFT (56)
#define IMM_OPCODE_MASK (0x3)
#define WRITE_IMM_IS_SO_SHIFT (63)
#define CALLBACK_SUCCESS (0)
#define CALLBACK_SKIP (1)
#define CALLBACK_FAIL (-1)
#define RECV_WR_ID_MAX (1U << 31)

typedef enum bondp_cr_handler_ret {
    CR_HANDLER_ERR_AND_COPY     = -1,
    CR_HANDLER_SUCCESS_AND_COPY = 0,
    CR_HANDLER_SUCCESS_AND_SKIP = 1,
    CR_HANDLER_ERR_AND_SKIP     = 2,
} bondp_cr_handler_ret_t;

/** Pass arguments to JFR WR buffer migration function */
struct rearm_args {
    bjetty_ctx_t *bjetty_ctx;
    int migrate_idx;
    urma_jfr_wr_t **bad_wr;
    int skip_count;
    int ret;
};

/** Pass arguments to JFS WR buffer migration function */
struct resend_args {
    bjetty_ctx_t *bjetty_ctx;
    uint32_t migrate_idx;
    urma_jfs_wr_t **bad_wr;
    int skip_count;
    int ret;
};

static urma_jetty_id_t *get_comp_urma_jetty_id(bondp_comp_t *bdp_comp)
{
    switch (bdp_comp->comp_type) {
        case BONDP_COMP_JFS:
            return &bdp_comp->v_jfs.jfs_id;
        case BONDP_COMP_JETTY:
            return &bdp_comp->v_jetty.jetty_id;
        case BONDP_COMP_JFR:
            return &bdp_comp->v_jfr.jfr_id;
        default:
            URMA_LOG_ERR("Failed to get_comp_urma_jetty, Invalid type: %d\n", bdp_comp->comp_type);
            return NULL;
    }
}

static urma_status_t comp_post_send(bjetty_ctx_t *bjetty_ctx, int send_idx,
    urma_jfs_wr_t *send_wr, urma_jfs_wr_t **bad_wr)
{
    urma_status_t ret;
    if (bjetty_ctx->bdp_comp->comp_type == BONDP_COMP_JETTY) {
        ret = urma_post_jetty_send_wr(bjetty_ctx->pjettys[send_idx], send_wr, bad_wr);
    } else if (bjetty_ctx->bdp_comp->comp_type == BONDP_COMP_JFS) {
        ret = urma_post_jfs_wr((urma_jfs_t *)bjetty_ctx->pjettys[send_idx], send_wr, bad_wr);
    } else {
        URMA_LOG_ERR("Invalid post jfs wr type: %d\n", bjetty_ctx->bdp_comp->comp_type);
        ret = URMA_EINVAL;
    }
    return ret;
}

static urma_status_t comp_post_recv(bjetty_ctx_t *bjetty_ctx, int recv_idx,
    urma_jfr_wr_t *recv_wr, urma_jfr_wr_t **bad_wr)
{
    urma_status_t ret;
    if (bjetty_ctx->bdp_comp->comp_type == BONDP_COMP_JETTY) {
        ret = urma_post_jetty_recv_wr(bjetty_ctx->pjettys[recv_idx], recv_wr, bad_wr);
    } else if (bjetty_ctx->bdp_comp->comp_type == BONDP_COMP_JFR) {
        ret = urma_post_jfr_wr((urma_jfr_t *)bjetty_ctx->pjettys[recv_idx], recv_wr, bad_wr);
    } else {
        URMA_LOG_ERR("Invalid post jfr wr type: %d\n", bjetty_ctx->bdp_comp->comp_type);
        ret = URMA_EINVAL;
    }
    return ret;
}

/** Ignore idx_start, iterate the array from idx_start + 1.
 * This function returns idx_start if no other dev is valid.
 * This function returns -1 when all devs are invalid.
 */
static int find_next_valid_jetty_idx(const bool *pjettys_valid, int dev_num, int idx_start)
{
    int ret = -1;
    for (int i = 0; i < dev_num; ++i) {
        if (pjettys_valid[(idx_start + i + 1) % dev_num]) {
            ret = (idx_start + i + 1) % dev_num;
            break;
        }
    }
    return ret;
}

static bool is_all_pjetty_fail(const bjetty_ctx_t *bjetty_ctx)
{
    for (int i = 0; i < bjetty_ctx->dev_num; ++i) {
        if (bjetty_ctx->pjettys_valid[i]) {
            return false;
        }
    }
    return true;
}

static int get_send_idx_with_least_load(bjetty_ctx_t *bjetty_ctx)
{
    int least_load_idx = -1;
    uint32_t least_load = UINT32_MAX;
    uint32_t count = 0;
    uint32_t send_idx = 0;
    for (int i = 0; i < bjetty_ctx->dev_num; ++i) {
        send_idx = (uint32_t)((i + bjetty_ctx->send_idx + 1) % bjetty_ctx->dev_num);
        if (bjetty_ctx->pjettys_valid[send_idx] == false) {
            continue;
        }
        count = (uint32_t)wr_buf_count(bjetty_ctx->jfs_bufs[send_idx]);
        if (count < least_load) {
            least_load = count;
            least_load_idx = (int)send_idx;
        }
    }
    return least_load_idx;
}

/** Select send pjetty idx in bjetty_ctx and target pjetty idx in bdp_target_jetty.
 * @return: 0 success.
 * @return: -1 when all local pjettys are invalid or all target pjettys are invalid.
 */
static int schedule_send_target_idx_default(bjetty_ctx_t *bjetty_ctx, bdp_v_conn_t *v_conn,
    int local_dev_num, int target_dev_num,
    urma_opcode_t opcode, urma_transport_mode_t trans_mode, int *send_idx, int *target_idx)
{
    bjetty_ctx->send_idx = get_send_idx_with_least_load(bjetty_ctx);
    if (bjetty_ctx->send_idx < 0) {
        URMA_LOG_DEBUG("Failed to find valid send jetty idx.\n");
        return -1;
    }
    *send_idx = bjetty_ctx->send_idx;
    switch (opcode) {
        case URMA_OPC_SEND:
        case URMA_OPC_SEND_IMM:
        case URMA_OPC_SEND_INVALIDATE:
        case URMA_OPC_WRITE_IMM:
        case URMA_OPC_WRITE_NOTIFY:
            v_conn->rqe_idx = find_next_valid_jetty_idx(v_conn->target_valid, target_dev_num, v_conn->rqe_idx);
            if (v_conn->rqe_idx < 0) {
                URMA_LOG_DEBUG("Failed to find valid target jetty idx for rqe_idx.\n");
                return -1;
            }
            *target_idx = v_conn->rqe_idx;
            break;
        default:
            v_conn->non_rqe_idx = find_next_valid_jetty_idx(v_conn->target_valid,
                target_dev_num, v_conn->non_rqe_idx);
            if (v_conn->non_rqe_idx < 0) {
                URMA_LOG_DEBUG("Failed to find valid target jetty idx for non_rqe_idx.\n");
                return -1;
            }
            *target_idx = v_conn->non_rqe_idx;
            break;
    }
    if (trans_mode == URMA_TM_RC) {
        *send_idx = *target_idx;
    }
    return 0;
}
/**
 * In matrix server, multipath mode has two different planes.
 * Each plane has one device which can connect to any other device identified by primary eid.
 * Choose target with RR and RQE requirement, then set send_idx = target_idx.
 * @param v_conn: can be NULL in single-die mode.
 */
static int schedule_next_route_in_matrix_server_multipath(const urma_jfs_wr_t *wr, bondp_target_jetty_t *bdp_tjetty,
    bdp_v_conn_t *v_conn,
    int *send_idx, int *target_idx)
{
    if (is_single_dev_mode(bdp_tjetty->v_tjetty.urma_ctx)) {
        *send_idx = 0;
        *target_idx = 0;
        return 0;
    }
    switch (wr->opcode) {
        case URMA_OPC_SEND:
        case URMA_OPC_SEND_IMM:
        case URMA_OPC_SEND_INVALIDATE:
        case URMA_OPC_WRITE_IMM:
        case URMA_OPC_WRITE_NOTIFY:
            v_conn->rqe_idx = find_next_valid_jetty_idx(v_conn->target_valid,
                bdp_tjetty->target_dev_num, v_conn->rqe_idx);
            if (v_conn->rqe_idx < 0) {
                URMA_LOG_DEBUG("Failed to find valid target jetty idx for rqe_idx.\n");
                return -1;
            }
            *target_idx = v_conn->rqe_idx;
            break;
        default:
            v_conn->non_rqe_idx = find_next_valid_jetty_idx(v_conn->target_valid,
                bdp_tjetty->target_dev_num, v_conn->non_rqe_idx);
            if (v_conn->non_rqe_idx < 0) {
                URMA_LOG_DEBUG("Failed to find valid target jetty idx for non_rqe_idx.\n");
                return -1;
            }
            *target_idx = v_conn->non_rqe_idx;
            break;
    }
    *send_idx = *target_idx;
    return 0;
}

static int schedule_next_route_in_matrix_server_singlepath(bjetty_ctx_t *bjetty_ctx, bondp_target_jetty_t *bdp_tjetty,
    int *send_idx, int *target_idx)
{
    if (bjetty_ctx->direct_local_port == -1 || bjetty_ctx->direct_target_port == -1) {
        URMA_LOG_ERR("Invalid single path port. Single path mode only support RC and need to call bind_jetty\n");
        return -1;
    }
    *send_idx = bjetty_ctx->direct_local_port;
    *target_idx = bjetty_ctx->direct_target_port;
    return 0;
}

/**
 * @param v_conn: can be NULL in singledie mode
 * @param send_idx: output
 * @param target_idx: output
 */
static int schedule_send(const urma_jfs_wr_t *wr, bjetty_ctx_t *bjetty_ctx, bdp_v_conn_t *v_conn,
    int *send_idx, int *target_idx)
{
    bondp_target_jetty_t *bdp_tjetty = CONTAINER_OF_FIELD(wr->tjetty, bondp_target_jetty_t, v_tjetty);
    if (bdp_tjetty == NULL) {
        URMA_LOG_ERR("Invalid wr->tjetty: NULL\n");
        return URMA_EINVAL;
    }
    if (!is_in_matrix_server(bjetty_ctx->bond_ctx)) {
        return schedule_send_target_idx_default(bjetty_ctx, v_conn, bjetty_ctx->dev_num,
            bdp_tjetty->target_dev_num, wr->opcode, bdp_tjetty->v_tjetty.trans_mode, send_idx, target_idx);
    }
    if (is_multipath_comp(bjetty_ctx->bdp_comp)) {
        return schedule_next_route_in_matrix_server_multipath(wr, bdp_tjetty, v_conn, send_idx, target_idx);
    }
    return schedule_next_route_in_matrix_server_singlepath(bjetty_ctx, bdp_tjetty, send_idx, target_idx);
}

/**
 * Local register seg only need param local_idx
 * Imported seg need both idxs
 */
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

static inline urma_target_jetty_t *get_p_tjetty(urma_target_jetty_t *tjetty, int send_idx, int target_idx)
{
    return CONTAINER_OF_FIELD(tjetty, bondp_target_jetty_t, v_tjetty)->p_tjetty[send_idx][target_idx];
}

static int parse_original_cr_opcode(uint64_t imm_data, urma_cr_opcode_t *opcode)
{
    switch ((imm_data >> IMM_OPCODE_SHIFT) & IMM_OPCODE_MASK) {
        case URMA_CR_OPC_SEND:
            *opcode = URMA_CR_OPC_SEND;
            return 0;
        case URMA_CR_OPC_SEND_WITH_IMM:
            *opcode = URMA_CR_OPC_SEND_WITH_IMM;
            return 0;
        case URMA_CR_OPC_SEND_WITH_INV:
            *opcode = URMA_CR_OPC_SEND_WITH_INV;
            return 0;
        case URMA_CR_OPC_WRITE_WITH_IMM:
            *opcode = URMA_CR_OPC_WRITE_WITH_IMM;
            return 0;
        default:
            return -1;
    }
}

/** Change vseg and vtjetty to pseg and ptjetty in opcode URMA_OPC_SEND_*.
 * We store the original vtseg and vtjetty in send_wr in wr_buf and set pseg and ptjetty in tmp_wr.
 * This is useful when we need to migrate a WR to another dev.
 */
static urma_status_t set_send_wr_ptseg_ptjetty(urma_jfs_wr_t *send_wr, urma_target_jetty_t *vtjetty,
    int send_idx, int target_idx)
{
    if (vtjetty == NULL) {
        URMA_LOG_ERR("tjetty in WR is NULL\n");
        return URMA_EINVAL;
    }
    bondp_target_jetty_t *bdp_tjetty = CONTAINER_OF_FIELD(vtjetty, bondp_target_jetty_t, v_tjetty);
    if (!is_valid_bdp_tjetty(bdp_tjetty)) {
        URMA_LOG_ERR("Invalid vtjetty, the structure may be self-consturcted.\n");
        return URMA_EINVAL;
    }
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
    if (vtjetty == NULL) {
        URMA_LOG_ERR("tjetty in WR is NULL\n");
        return URMA_EINVAL;
    }
    bondp_target_jetty_t *bdp_tjetty = CONTAINER_OF_FIELD(vtjetty, bondp_target_jetty_t, v_tjetty);
    if (!is_valid_bdp_tjetty(bdp_tjetty)) {
        URMA_LOG_ERR("Invalid vtjetty, the structure may be self-consturcted.\n");
        return URMA_EINVAL;
    }
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
    if (vtjetty == NULL) {
        URMA_LOG_ERR("tjetty in WR is NULL\n");
        return URMA_EINVAL;
    }
    bondp_target_jetty_t *bdp_tjetty = CONTAINER_OF_FIELD(vtjetty, bondp_target_jetty_t, v_tjetty);
    if (!is_valid_bdp_tjetty(bdp_tjetty)) {
        URMA_LOG_ERR("Invalid vtjetty, the structure may be self-consturcted.\n");
        return URMA_EINVAL;
    }
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
    if (vtjetty == NULL) {
        URMA_LOG_ERR("tjetty in WR is NULL\n");
        return URMA_EINVAL;
    }
    bondp_target_jetty_t *bdp_tjetty = CONTAINER_OF_FIELD(vtjetty, bondp_target_jetty_t, v_tjetty);
    if (!is_valid_bdp_tjetty(bdp_tjetty)) {
        URMA_LOG_ERR("Invalid vtjetty, the structure may be self-consturcted.\n");
        return URMA_EINVAL;
    }
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

static urma_status_t post_send_check_jfs_wr_valid(const bondp_context_t *bdp_ctx, const urma_jfs_wr_t *wr)
{
    switch (wr->opcode) {
        case URMA_OPC_SEND:
        case URMA_OPC_SEND_IMM:
        case URMA_OPC_SEND_INVALIDATE:
            /* No need to handle cases where num_sge == 0 or sge == NULL;
               UDMA will take care of it, as SEND_WITH_IMM may allow NULL to be passed.
            */
            if (bdp_ctx->v_ctx.dev->sysfs_dev->dev_attr.dev_cap.max_jfs_sge < wr->send.src.num_sge) {
                URMA_LOG_WARN("The number of sge %u the destination segment is greater than the maximum supported: %u"
                    "by the device.\n", wr->send.src.num_sge,
                    bdp_ctx->v_ctx.dev->sysfs_dev->dev_attr.dev_cap.max_jfs_sge);
            }
            break;
        case URMA_OPC_WRITE:
        case URMA_OPC_WRITE_IMM:
        case URMA_OPC_WRITE_NOTIFY:
        case URMA_OPC_READ:
            /* It must be verified; otherwise, udma will cause a segmentation fault. */
            if (wr->rw.src.num_sge == 0 || wr->rw.dst.num_sge == 0 ||
                wr->rw.src.sge == NULL || wr->rw.dst.sge == NULL) {
                URMA_LOG_ERR("when set write_wr, either of src/dst num_sge/sge has been set zero or NULL.\n");
                return URMA_EINVAL;
            }
            /* The limitation of rsge is the number of *remote sge* that can be accessed,
               whether for write or read operations. */
            if (wr->opcode == URMA_OPC_READ) {
                if (bdp_ctx->v_ctx.dev->sysfs_dev->dev_attr.dev_cap.max_jfs_rsge < wr->rw.src.num_sge) {
                    URMA_LOG_WARN("The number of remote sge %u is greater than the maximum supported: %u"
                        " by the device.\n", wr->rw.src.num_sge,
                        bdp_ctx->v_ctx.dev->sysfs_dev->dev_attr.dev_cap.max_jfs_rsge);
                }
                if (bdp_ctx->v_ctx.dev->sysfs_dev->dev_attr.dev_cap.max_jfs_sge < wr->rw.dst.num_sge) {
                    URMA_LOG_WARN("The number of local sge %u is greater than the maximum supported: %u"
                        " by the device.\n", wr->rw.dst.num_sge,
                        bdp_ctx->v_ctx.dev->sysfs_dev->dev_attr.dev_cap.max_jfs_sge);
                }
            } else {
                if (bdp_ctx->v_ctx.dev->sysfs_dev->dev_attr.dev_cap.max_jfs_sge < wr->rw.src.num_sge) {
                    URMA_LOG_WARN("The number of local sge %u is greater than the maximum supported: %u"
                        " by the device.\n", wr->rw.src.num_sge,
                        bdp_ctx->v_ctx.dev->sysfs_dev->dev_attr.dev_cap.max_jfs_sge);
                }
                if (bdp_ctx->v_ctx.dev->sysfs_dev->dev_attr.dev_cap.max_jfs_rsge < wr->rw.dst.num_sge) {
                    URMA_LOG_WARN("The number of remote sge %u is greater than the maximum supported: %u"
                        " by the device.\n", wr->rw.dst.num_sge,
                        bdp_ctx->v_ctx.dev->sysfs_dev->dev_attr.dev_cap.max_jfs_rsge);
                }
            }
            break;
        case URMA_OPC_CAS:
            /* It must be verified; otherwise, udma will cause a segmentation fault. */
            if (wr->cas.src == NULL || wr->cas.dst == NULL) {
                URMA_LOG_ERR("when set cas_wr, either src or dst is NULL.\n");
                return URMA_EINVAL;
            }
            break;
        case URMA_OPC_FADD:
            /* It must be verified; otherwise, udma will cause a segmentation fault. */
            if (wr->faa.src == NULL || wr->faa.dst == NULL) {
                URMA_LOG_ERR("when set faa_wr, either src or dst is NULL.\n");
                return URMA_EINVAL;
            }
            break;
        default:
            break;
    }
    return URMA_SUCCESS;
}

static urma_status_t post_send_check_valid(bondp_comp_t *bdp_send_comp, bondp_target_jetty_t *bdp_tjetty,
    const urma_jfs_wr_t *wr)
{
    if (!is_valid_bondp_comp(bdp_send_comp)) {
        URMA_LOG_ERR("Invalid bdp_send_comp");
        return URMA_EINVAL;
    }
    if (bdp_send_comp->comp_type != BONDP_COMP_JFS && bdp_send_comp->comp_type != BONDP_COMP_JETTY) {
        URMA_LOG_ERR("Try to call post_send api by invalid comp_type: %d\n", bdp_send_comp->comp_type);
        return URMA_EINVAL;
    }
    if (!is_valid_bdp_tjetty(bdp_tjetty)) {
        URMA_LOG_ERR("Invalid bdp_target_jetty");
        return URMA_EINVAL;
    }
    if (is_in_matrix_server(bdp_send_comp->bondp_ctx) != bdp_tjetty->is_in_matrix_server) {
        URMA_LOG_ERR("Data cannot be transferred between jettys in different matrix server mode\n");
        return URMA_EINVAL;
    }
    if (is_in_matrix_server(bdp_send_comp->bondp_ctx)) {
        if (is_multipath_comp(bdp_send_comp) != bdp_tjetty->is_multipath) {
            URMA_LOG_ERR("Data cannot be transferred between jettys in different multipath mode\n");
            return URMA_EINVAL;
        }
    }
    bjetty_ctx_t *bjetty_ctx = bdp_send_comp->comp_ctx;
    if (bjetty_ctx == NULL) {
        URMA_LOG_ERR("No bjetty_ctx\n");
        return URMA_EINVAL;
    }
    if (is_all_pjetty_fail(bjetty_ctx)) {
        URMA_LOG_ERR("All bonding devs are invalid");
        return URMA_FAIL;
    }
    urma_status_t ret = post_send_check_jfs_wr_valid(bdp_send_comp->bondp_ctx, wr);
    if (ret != URMA_SUCCESS) {
        return ret;
    }
    return URMA_SUCCESS;
}
/**
 * When sending data, attempt to retrieve the v_conn corresponding to the target tjetty from bjetty_ctx;
 * if it does not exist, try to create one, then init it with `init_v_conn_on_send` if needed.
 * @return: NULL if lookup failed and creation failed
 */
static bdp_v_conn_t *get_v_conn_on_send(bjetty_ctx_t *bjetty_ctx, bondp_target_jetty_t *bdp_tjetty)
{
    urma_jetty_id_t *comp_jetty_id = get_comp_urma_jetty_id(bjetty_ctx->bdp_comp);
    if (comp_jetty_id == NULL) {
        return NULL;
    }
    urma_jetty_id_t *vtjetty_id = &bdp_tjetty->v_tjetty.id;
    bdp_v_conn_t *v_conn = bdp_v_conn_table_lookup(&bjetty_ctx->v_conn_table, vtjetty_id);
    if (!v_conn) {
        int ret = bdp_v_conn_table_add_on_send(&bjetty_ctx->v_conn_table, vtjetty_id,
            bdp_tjetty, bdp_tjetty->target_dev_num, &v_conn, bjetty_ctx->bond_ctx->v_ctx.aggr_mode);
        if (ret != 0) {
            URMA_LOG_ERR("Failed to create v_conn for vjetty, ret: %d, "
                "[" URMA_JETTY_ID_FMT " -> " URMA_JETTY_ID_FMT "]\n",
                ret, URMA_JETTY_ID_ARGS(comp_jetty_id), URMA_JETTY_ID_ARGS(vtjetty_id));
            return NULL;
        }
    }
    /* v_conn is not null */
    /* If this v_conn is created by handle_recv, then we need to initialize it */
    if (v_conn->target_vjetty == NULL) {
        init_v_conn_on_send(v_conn, bdp_tjetty, bdp_tjetty->target_dev_num);
    }
    return v_conn;
}

static int convert_jfs_vwr_to_pwr_nostore(urma_jfs_wr_t *wr, int send_idx, int target_idx)
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

/**
 * Do not cache WR, nor perform reliability processing;
 * simply replicate the WR information,
 * replace it with ptjetty and ptseg content,
 * and then send it.
 */
static urma_status_t bondp_post_send_wr_no_store(bjetty_ctx_t *bjetty_ctx,
    const urma_jfs_wr_t *wr, urma_jfs_wr_t **bad_wr)
{
    static thread_local urma_jfs_wr_t prealloc_wr_list[BONDP_MAX_WR_LIST_NUM];
    static thread_local urma_sge_t prealloc_src_sge[BONDP_MAX_WR_LIST_NUM][BONDP_MAX_SGE_NUM];
    static thread_local urma_sge_t prealloc_dst_sge[BONDP_MAX_WR_LIST_NUM][BONDP_MAX_SGE_NUM];

    urma_status_t ret = URMA_SUCCESS;
    int local_port = -1;
    int target_port = -1;
    bondp_target_jetty_t *bdp_tjetty = CONTAINER_OF_FIELD(wr->tjetty, bondp_target_jetty_t, v_tjetty);
    if (bdp_tjetty == NULL) {
        URMA_LOG_ERR("WR->tjetty is NULL\n");
        return URMA_EINVAL;
    }
    bdp_v_conn_t *v_conn = get_v_conn_on_send(bjetty_ctx, bdp_tjetty);
    if (v_conn == NULL) {
        return URMA_FAIL;
    }
    ret = schedule_send(wr, bjetty_ctx, v_conn, &local_port, &target_port);
    if (ret != 0) {
        return URMA_FAIL;
    }

    int index = 0;
    urma_jfs_wr_t *vwr = (urma_jfs_wr_t *)wr;
    while (vwr != NULL) {
        urma_jfs_wr_t *pwr = &prealloc_wr_list[index];
        ret = copy_jfs_wr(vwr, pwr, prealloc_src_sge[index], prealloc_dst_sge[index]);
        if (ret != 0) {
            return URMA_FAIL;
        }
        ret = convert_jfs_vwr_to_pwr_nostore(pwr, local_port, target_port);
        if (ret != 0) {
            return URMA_FAIL;
        }
        if (pwr->next != NULL) {
            pwr->next = &prealloc_wr_list[index + 1];
        }

        vwr = vwr->next;
        index++;
        if (index >= BONDP_MAX_WR_LIST_NUM - 1) {
            URMA_LOG_ERR("Bondp supports at most %d wr_list.\n", BONDP_MAX_WR_LIST_NUM - 1);
            return URMA_EINVAL;
        }
    }

    ret = comp_post_send(bjetty_ctx, local_port, prealloc_wr_list, bad_wr);
    return ret;
}

static inline uint64_t encode_imm_data(uint64_t imm_data, uint32_t msn, uint64_t opcode_tag)
{
    imm_data &= ((uint64_t)1 << WRITE_IMM_USER_BITS) - 1;
    imm_data |= (uint64_t)msn << WRITE_IMM_USER_BITS;
    imm_data |= opcode_tag << IMM_OPCODE_SHIFT;
    return imm_data;
}

static int convert_jfs_vwr_to_pwr(urma_jfs_wr_t *wr, int send_idx, int target_idx, bdp_v_conn_t *v_conn)
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

static urma_status_t bondp_post_send_wr_and_store(bjetty_ctx_t *bjetty_ctx,
    bondp_jfc_t *vjfc, urma_jfs_wr_t *wr, urma_jfs_wr_t **bad_wr)
{
    bondp_target_jetty_t *bdp_tjetty = CONTAINER_OF_FIELD(wr->tjetty, bondp_target_jetty_t, v_tjetty);
    bdp_v_conn_t *v_conn = get_v_conn_on_send(bjetty_ctx, bdp_tjetty);
    if (v_conn == NULL) {
        return URMA_FAIL;
    }

    int send_idx = 0, target_idx = 0;
    urma_status_t ret;

    ret = schedule_send(wr, bjetty_ctx, v_conn, &send_idx, &target_idx);
    if (ret != 0) {
        return URMA_FAIL;
    }

    jfs_wr_entry_t *wr_entry = jfs_wr_buf_alloc(&vjfc->wr_buf);
    if (wr_entry == NULL) {
        return URMA_EAGAIN;
    }
    wr_entry->user_ctx = wr->user_ctx;
    wr_entry->bjetty_ctx = bjetty_ctx;
    wr_entry->v_conn = v_conn;
    wr_entry->send_idx = send_idx;

    urma_jfs_wr_t *pwr = &wr_entry->wr;
    ret = copy_jfs_wr(wr, pwr, NULL, NULL);
    if (ret != 0) {
        URMA_LOG_ERR("Failed to copy jfs wr\n");
        return -1;
    }
    ret = convert_jfs_vwr_to_pwr(pwr, send_idx, target_idx, v_conn);
    if (ret != 0) {
        goto FREE_PWR;
    }

    pwr->user_ctx = wr_entry->wr_id;
    ret = comp_post_send(bjetty_ctx, send_idx, pwr, bad_wr);
    if (ret != URMA_SUCCESS) {
        goto FREE_PWR;
    }

    return URMA_SUCCESS;

FREE_PWR:
    free_jfs_wr(pwr);
    jfs_wr_buf_release(wr_entry);
    return ret;
}

urma_status_t bondp_post_jetty_send_wr(urma_jetty_t *jetty, urma_jfs_wr_t *wr, urma_jfs_wr_t **bad_wr)
{
    bondp_comp_t *bdp_jetty = CONTAINER_OF_FIELD(jetty, bondp_comp_t, v_jetty);
    bondp_target_jetty_t *bdp_tjetty = CONTAINER_OF_FIELD(wr->tjetty, bondp_target_jetty_t, v_tjetty);
    urma_status_t ret = post_send_check_valid(bdp_jetty, bdp_tjetty, wr);
    if (ret != URMA_SUCCESS) {
        return ret;
    }

    bjetty_ctx_t *bjetty_ctx = bdp_jetty->comp_ctx;
    if (is_single_dev_mode(jetty->urma_ctx)) {
        return bondp_post_send_wr_no_store(bjetty_ctx, wr, bad_wr);
    } else {
        return bondp_post_send_wr_and_store(bjetty_ctx, bdp_jetty->send_jfc, wr, bad_wr);
    }
}

urma_status_t bondp_post_jfs_wr(urma_jfs_t *jfs, urma_jfs_wr_t *wr, urma_jfs_wr_t **bad_wr)
{
    bondp_comp_t *bdp_jfs = CONTAINER_OF_FIELD(jfs, bondp_comp_t, v_jfs);
    bondp_target_jetty_t *bdp_tjetty = CONTAINER_OF_FIELD(wr->tjetty, bondp_target_jetty_t, v_tjetty);
    bjetty_ctx_t *bjetty_ctx = bdp_jfs->comp_ctx;
    urma_status_t ret = URMA_SUCCESS;

    ret = post_send_check_valid(bdp_jfs, bdp_tjetty, wr);
    if (ret != URMA_SUCCESS) {
        return ret;
    }
    if (is_single_dev_mode(jfs->urma_ctx)) {
        return bondp_post_send_wr_no_store(bjetty_ctx, wr, bad_wr);
    } else {
        return bondp_post_send_wr_and_store(bjetty_ctx, bdp_jfs->send_jfc, wr, bad_wr);
    }
}


/** Select recv pjetty in post_jetty_recv_wr */
static urma_status_t schedule_recv_idx_default(bjetty_ctx_t *bjetty_ctx, int *recv_idx)
{
    bjetty_ctx->post_recv_idx = find_next_valid_jetty_idx(bjetty_ctx->pjettys_valid, bjetty_ctx->dev_num,
                                                          bjetty_ctx->post_recv_idx);
    if (bjetty_ctx->post_recv_idx < 0) {
        /* all pjetty fail */
        URMA_LOG_INFO("All pjetty fail in schedule_recv_idx_default.");
        return URMA_FAIL;
    }
    *recv_idx = bjetty_ctx->post_recv_idx;
    return URMA_SUCCESS;
}

static urma_status_t set_jfr_wr_ptjetty_ptseg_without_hdr(urma_jfr_wr_t *recv_wr, int local_idx, int target_idx)
{
    /* set user sge */
    for (int i = 0; i < recv_wr->src.num_sge; ++i) {
        if (recv_wr->src.sge[i].tseg == NULL) {
            URMA_LOG_ERR("Recv sge[%d] has NULL tseg\n", i);
            return URMA_EINVAL;
        }
        recv_wr->src.sge[i].tseg = get_p_tseg(recv_wr->src.sge[i].tseg, local_idx, target_idx);
    }
    return URMA_SUCCESS;
}

static urma_status_t schedule_next_recv_port_matrix_multipath(bjetty_ctx_t *bjetty_ctx, int *recv_idx)
{
    if (is_single_dev_mode(&bjetty_ctx->bond_ctx->v_ctx)) {
        *recv_idx = 0;
        return URMA_SUCCESS;
    }
    return schedule_recv_idx_default(bjetty_ctx, recv_idx);
}

static urma_status_t schedule_next_recv_port_matrix_singlepath(bjetty_ctx_t *bjetty_ctx, int *recv_idx)
{
    if (bjetty_ctx->direct_local_port == -1 || bjetty_ctx->direct_target_port == -1) {
        URMA_LOG_ERR("Invalid single path port in recv."
            "It is likely because `urma_post_jetty_recv` was called before `urma_bind_jetty`.\n");
        return URMA_EINVAL;
    }
    *recv_idx = bjetty_ctx->direct_local_port;
    return URMA_SUCCESS;
}

static urma_status_t schedule_recv(bjetty_ctx_t *bjetty_ctx, int *recv_idx)
{
    if (!is_in_matrix_server(bjetty_ctx->bdp_comp->bondp_ctx)) {
        return schedule_recv_idx_default(bjetty_ctx, recv_idx);
    }
    /* JFR is set to multipath mode at default */
    /* Only JETTY can be single_path mode in schedule_recv */
    if (is_multipath_comp(bjetty_ctx->bdp_comp)) {
        return schedule_next_recv_port_matrix_multipath(bjetty_ctx, recv_idx);
    }
    return schedule_next_recv_port_matrix_singlepath(bjetty_ctx, recv_idx);
}
/**
 * Do not cache WR, nor perform reliability processing;
 * simply replicate the WR information,
 * replace it with ptjetty and ptseg content,
 * and then post recv.
 */
static urma_status_t bondp_post_recv_wr_no_store(bjetty_ctx_t *bjetty_ctx,
    const urma_jfr_wr_t *wr, urma_jfr_wr_t **bad_wr)
{
    int recv_idx = -1;
    urma_status_t ret = 0;
    ret = schedule_recv(bjetty_ctx, &recv_idx);
    if (ret != 0) {
        return URMA_FAIL;
    }
    urma_jfr_wr_t *copied_jfr_wr = deepcopy_jfr_wr(wr);
    if (copied_jfr_wr == NULL) {
        return URMA_FAIL;
    }
    urma_jfr_wr_t *cur_wr = copied_jfr_wr;
    while (cur_wr != NULL) {
        ret = set_jfr_wr_ptjetty_ptseg_without_hdr(cur_wr, recv_idx, 0);
        if (ret != 0) {
            goto DELETE_JFR_WR;
        }
        cur_wr = cur_wr->next;
    }
    ret = comp_post_recv(bjetty_ctx, recv_idx, copied_jfr_wr, bad_wr);
DELETE_JFR_WR:
    (void)delete_copied_jfr_wr(copied_jfr_wr);
    return ret;
}

static urma_status_t bondp_post_recv_wr_and_store(bjetty_ctx_t *bjetty_ctx, bondp_jfc_t *vjfc,
                                                  urma_jfr_wr_t *wr, urma_jfr_wr_t **bad_wr)
{
    int recv_idx = 0;
    urma_status_t ret;

    ret = schedule_recv(bjetty_ctx, &recv_idx);
    if (ret != 0) {
        return URMA_FAIL;
    }

    jfr_wr_entry_t *wr_entry = jfr_wr_buf_alloc(&vjfc->wr_buf);
    if (wr_entry == NULL) {
        return URMA_EAGAIN;
    }
    wr_entry->user_ctx = wr->user_ctx;
    wr_entry->bjetty_ctx = bjetty_ctx;
    wr_entry->recv_idx = recv_idx;

    urma_jfr_wr_t *pwr = &wr_entry->wr;
    pwr->user_ctx = wr_entry->wr_id;
    ret = convert_jfr_vwr_to_pwr(wr, pwr, recv_idx);
    if (ret != 0) {
        goto FREE_PWR;
    }

    ret = comp_post_recv(bjetty_ctx, recv_idx, pwr, bad_wr);
    if (ret != URMA_SUCCESS) {
        goto FREE_PWR;
    }
    return URMA_SUCCESS;

FREE_PWR:
    free_jfr_wr(pwr);
    jfr_wr_buf_release(wr_entry);
    return ret;
}

static urma_status_t post_recv_check_jfr_wr_valid(const bondp_context_t *bdp_ctx, const urma_jfr_wr_t *wr)
{
    /* No need to handle cases where num_sge == 0 or sge == NULL; Certain hardware supports this usage. */
    if (bdp_ctx->v_ctx.dev->sysfs_dev->dev_attr.dev_cap.max_jfr_sge < wr->src.num_sge) {
        URMA_LOG_WARN("The number of sge %u the src segment is greater than the maximum supported: %u"
            " by the device.\n", wr->src.num_sge,
            bdp_ctx->v_ctx.dev->sysfs_dev->dev_attr.dev_cap.max_jfr_sge);
    }
    return URMA_SUCCESS;
}

static urma_status_t post_recv_check_valid(bondp_comp_t *bdp_recv_comp, const urma_jfr_wr_t *wr)
{
    if (!is_valid_bondp_comp(bdp_recv_comp)) {
        URMA_LOG_ERR("Invalid bdp_comp\n");
        return URMA_EINVAL;
    }
    bjetty_ctx_t *bjetty_ctx = bdp_recv_comp->comp_ctx;
    if (bjetty_ctx == NULL) {
        URMA_LOG_ERR("bjetty_ctx is NULL\n");
        return URMA_EINVAL;
    }
    if (bdp_recv_comp->comp_type != BONDP_COMP_JETTY && bdp_recv_comp->comp_type != BONDP_COMP_JFR) {
        URMA_LOG_ERR("Invalid bdp_recv_comp type: %d\n", bdp_recv_comp->comp_type);
        return URMA_EINVAL;
    }
    urma_status_t ret = post_recv_check_jfr_wr_valid(bdp_recv_comp->bondp_ctx, wr);
    if (ret != URMA_SUCCESS) {
        return ret;
    }
    return URMA_SUCCESS;
}

urma_status_t bondp_post_jetty_recv_wr(urma_jetty_t *jetty, urma_jfr_wr_t *wr, urma_jfr_wr_t **bad_wr)
{
    bondp_comp_t *bdp_jetty = CONTAINER_OF_FIELD(jetty, bondp_comp_t, v_jetty);
    urma_status_t ret = URMA_SUCCESS;
    ret = post_recv_check_valid(bdp_jetty, wr);
    if (ret != URMA_SUCCESS) {
        return ret;
    }

    /* non-null bjetty_ctx value because post_recv_check_valid performed validation. */
    bjetty_ctx_t *bjetty_ctx = bdp_jetty->comp_ctx;
    if (is_single_dev_mode(jetty->urma_ctx)) {
        return bondp_post_recv_wr_no_store(bjetty_ctx, wr, bad_wr);
    } else {
        return bondp_post_recv_wr_and_store(bjetty_ctx, bdp_jetty->recv_jfc, wr, bad_wr);
    }
}

urma_status_t bondp_post_jfr_wr(urma_jfr_t *jfr, urma_jfr_wr_t *wr, urma_jfr_wr_t **bad_wr)
{
    bondp_comp_t *bdp_jfr = CONTAINER_OF_FIELD(jfr, bondp_comp_t, v_jfr);
    urma_status_t ret = URMA_SUCCESS;
    ret = post_recv_check_valid(bdp_jfr, wr);
    if (ret != URMA_SUCCESS) {
        return ret;
    }

    /* non-null bjetty_ctx value because post_recv_check_valid performed validation. */
    bjetty_ctx_t *bjetty_ctx = bdp_jfr->comp_ctx;
    // workaround, at this point, jfr only support multipath
    bdp_jfr->is_multipath = true;
    if (is_single_dev_mode(jfr->urma_ctx)) {
        return bondp_post_recv_wr_no_store(bjetty_ctx, wr, bad_wr);
    } else {
        return bondp_post_recv_wr_and_store(bjetty_ctx, bdp_jfr->recv_jfc, wr, bad_wr);
    }
}

static inline bool is_device_error(urma_cr_status_t status)
{
    return status >= URMA_CR_ACK_TIMEOUT_ERR;
}

static inline bool is_target_device_error(urma_cr_status_t status)
{
    return false;
}

static inline bool is_local_device_error(bjetty_ctx_t *bjetty_ctx, int send_idx)
{
    return !bjetty_ctx->pjettys_valid[send_idx];
}

static inline bool is_recv_cr(const urma_cr_t *cr)
{
    return cr->flag.bs.s_r == 1;
}

/**
 * @return 0: Success, Do copy CR
 * @return 1: Success, Do Not copy CR
 * @return <0: Error,  Do copy CR
 * @return >1: Error, and Do Not copy CR
 */
static bondp_cr_handler_ret_t handle_send(urma_cr_t *cr, jfs_wr_entry_t *wr_entry)
{
    bjetty_ctx_t *bjetty_ctx = wr_entry->bjetty_ctx;
    uint32_t send_idx = wr_entry->send_idx;

    int ret = 0;
    if (bjetty_ctx->bdp_comp->comp_type != BONDP_COMP_JETTY && bjetty_ctx->bdp_comp->comp_type != BONDP_COMP_JFS) {
        URMA_LOG_ERR("Invalid bdp_comp type: %d\n", bjetty_ctx->bdp_comp->comp_type);
        return CR_HANDLER_ERR_AND_COPY;
    }
    /* Do migration */
    if (is_local_device_error(bjetty_ctx, send_idx)) {
        /*
        Do migration when the current jetty fail and at least one jetty is valid.
        Migrate all WRs from the current jetty to the next valid jetty
        ! Migration may fail, but considering it transparent to user, so we return 1 to ignore CR
        */
        int migrate_idx = (send_idx + 1) % PRIMARY_EID_NUM;
        if (!bjetty_ctx->pjettys_valid[migrate_idx]) {
            return CR_HANDLER_SUCCESS_AND_SKIP;
        }
        // TODO: resend
        // urma_jfs_wr_t *bad_wr = NULL;
        // ret = resend_error_device(bjetty_ctx, send_idx, (uint32_t)migrate_idx, &bad_wr);
        URMA_LOG_DEBUG("Migrate send from %d to %d, ret %d\n", send_idx, migrate_idx, ret);
        return CR_HANDLER_SUCCESS_AND_SKIP;
    }

    /* Handle v_conn slide window */
    bdp_v_conn_t *v_conn = wr_entry->v_conn;
    if (cr->status != URMA_CR_SUCCESS) {
        cr->user_ctx = wr_entry->user_ctx;
        return CR_HANDLER_SUCCESS_AND_COPY;
    }
    /* Sender use send_wr_id to indicate the order of send, node->key is send_wr_id */
    (void)bdp_slide_wnd_add(&v_conn->send_wnd, wr_entry->wr_id);
    cr->user_ctx = wr_entry->user_ctx;
    return CR_HANDLER_SUCCESS_AND_COPY;
}

static int parse_imm_data(uint64_t imm_data, urma_cr_opcode_t *opcode, uint32_t *msn)
{
    *msn = (imm_data >> WRITE_IMM_USER_BITS) & (BONDP_MAX_BITMAP_SIZE - 1);
    if (parse_original_cr_opcode(imm_data, opcode) != 0) {
        return -1;
    }
    return 0;
}

static urma_status_t rearm_single_wr(bjetty_ctx_t *bjetty_ctx, int recv_idx,
    uint32_t id, urma_jfr_wr_t **bad_wr)
{
    urma_jfr_wr_t *wr = jfr_wr_buf_get_wr(bjetty_ctx->jfr_bufs[recv_idx], id);
    if (wr == NULL) {
        return URMA_FAIL;
    }
    return comp_post_recv(bjetty_ctx, recv_idx, wr, bad_wr);
}

static inline bdp_r_p2v_jetty_id_type_t get_remote_id_type_by_cr(urma_cr_t *cr)
{
    return (cr->flag.bs.jetty != 0) ? REMOTE_JETTY : REMOTE_JFR;
}

/**
 * @return 0: Success, Do copy CR
 * @return 1: Success, Do Not copy CR
 * @return <0: Error,  Do copy CR
 * @return >1: Error, and Do Not copy CR
 */
static bondp_cr_handler_ret_t handle_recv(urma_cr_t *cr, jfr_wr_entry_t *wr_entry)
{
    bjetty_ctx_t *bjetty_ctx = wr_entry->bjetty_ctx;
    uint32_t recv_idx = wr_entry->recv_idx;

    urma_jfr_wr_t *bad_wr = NULL;
    int ret = 0;
    uint32_t msn = 0;
    bdp_v_conn_t *v_conn = NULL;

    if (bjetty_ctx->bdp_comp->comp_type != BONDP_COMP_JETTY && bjetty_ctx->bdp_comp->comp_type != BONDP_COMP_JFR) {
        URMA_LOG_ERR("Invalid bdp_comp type: %d\n", bjetty_ctx->bdp_comp->comp_type);
        return CR_HANDLER_ERR_AND_COPY;
    }
    /* Do migration */
    if (!bjetty_ctx->pjettys_valid[recv_idx]) {
        /*
        Do migration when the current jetty fail and at least one jetty is valid.
        Migrate all WRs from the current jetty to the next valid jetty
        ! Migration may fail, but considering it transparent to user, so we return 1 to ignore CR
        */
        int migrate_idx = find_next_valid_jetty_idx(bjetty_ctx->pjettys_valid, bjetty_ctx->dev_num, recv_idx);
        // ret = rearm_error_device(bjetty_ctx, recv_idx, migrate_idx, &bad_wr);
        URMA_LOG_DEBUG("Migrate recv from %d to %d, ret: %d\n", recv_idx, migrate_idx, ret);
        return CR_HANDLER_SUCCESS_AND_SKIP;
    }

    cr->user_ctx = wr_entry->user_ctx;

    urma_jetty_id_t target_jetty_id = {0};
    ret = bdp_r_p2v_jetty_id_table_lookup(
        &bjetty_ctx->bond_ctx->remote_p2v_jetty_id_table,
        &cr->remote_id, get_remote_id_type_by_cr(cr), &target_jetty_id
    );
    if (ret != 0) {
        URMA_LOG_ERR("Failed to get target jetty id " EID_FMT " %u %u.\n",
            EID_ARGS(cr->remote_id.eid), cr->remote_id.id, cr->remote_id.uasid);
        return CR_HANDLER_ERR_AND_COPY;
    }

    urma_cr_opcode_t original_opcode = cr->opcode;
    if (parse_imm_data(cr->imm_data, &original_opcode, &msn) != 0) {
        URMA_LOG_ERR("Failed to parse original opcode from imm metadata\n");
        return CR_HANDLER_ERR_AND_COPY;
    }
    cr->opcode = original_opcode;
    cr->imm_data &= ((uint64_t)1 << WRITE_IMM_USER_BITS) - 1;

    /* Do de-duplicating */
    v_conn = bdp_v_conn_table_lookup(&bjetty_ctx->v_conn_table, &target_jetty_id);
    if (!v_conn) {
        ret = bdp_v_conn_table_add_on_recv(&bjetty_ctx->v_conn_table, &target_jetty_id, &v_conn,
            bjetty_ctx->bond_ctx->v_ctx.aggr_mode);
        if (ret != 0) {
            /* get_comp_urma_jetty_id will return a non-null value,
               because we check bjetty_ctx->bdp_comp type at the entrance of this function. */
            urma_jetty_id_t *jfr_jetty_id = get_comp_urma_jetty_id(bjetty_ctx->bdp_comp);
            URMA_LOG_ERR("Failed to create vconn for ( "EID_FMT" %d, "EID_FMT" %d)",
                EID_ARGS(jfr_jetty_id->eid), jfr_jetty_id->id,
                EID_ARGS(target_jetty_id.eid), target_jetty_id.id
            );
            return CR_HANDLER_ERR_AND_COPY;
        }
    }
    if (!bdp_slide_wnd_seq_in_window(&v_conn->recv_wnd, msn) || bdp_slide_wnd_has(&v_conn->recv_wnd, msn)) {
        ret = rearm_single_wr(bjetty_ctx, recv_idx, wr_entry->wr_id, &bad_wr);
        URMA_LOG_DEBUG("Rearm recv WR due to: outside of window: %d or duplicate %d\n",
            !bdp_slide_wnd_seq_in_window(&v_conn->recv_wnd, msn),
            bdp_slide_wnd_has(&v_conn->recv_wnd, msn));
        return CR_HANDLER_SUCCESS_AND_SKIP;
    }
    (void)bdp_slide_wnd_add(&v_conn->recv_wnd, msn);
    return CR_HANDLER_SUCCESS_AND_COPY;
}

static inline bdp_p_vjetty_type_t get_p_vjetty_type_by_cr(const urma_cr_t *cr)
{
    if (cr->flag.bs.jetty != 0) {
        return JETTY;
    }
    if (cr->flag.bs.s_r == 0) {
        return JFS;
    }
    return JFR;
}
/**
 * Utilize certain fields in CR to get the corresponding bjetty_ctx, thereby executing failover and recovery functions.
 * It may be necessary to modify the implementation of this function in the future.
 * When calling this function, it must be ensured that the user_ctx in this CR is valid.
 * That is to say, this interface cannot be called during URMA_CR_WR_SUSPEND_DONE and URMA_CR_WR_FLUSH_ERR_DONE.
 * When obtaining the `bjetty_ctx`, this function will increment the reference count of its corresponding `bondp_comp`.
 * The caller needs to decrement the reference count when bjetty_ctx is about to go out of scope.
 * @return Return bjetty_ctx on success, return NULL on failure.
 */
static bjetty_ctx_t *get_bjetty_ctx_by_cr(bondp_context_t *bdp_ctx, int dev_idx, urma_cr_t *cr)
{
    urma_jetty_id_t pjetty_id = {
        .eid = bdp_ctx->p_ctxs[dev_idx]->eid,
        .id = cr->local_id,
    };
    pthread_rwlock_rdlock(&bdp_ctx->p_vjetty_id_table.lock);
    bondp_comp_t *comp = bdp_p_vjetty_id_table_lookup_comp_without_lock(&bdp_ctx->p_vjetty_id_table, pjetty_id,
                                                                        get_p_vjetty_type_by_cr(cr));
    if (comp == NULL) {
        pthread_rwlock_unlock(&bdp_ctx->p_vjetty_id_table.lock);
        URMA_LOG_ERR("Failed to get comp, local_id: %d\n", pjetty_id.id);
        return NULL;
    }
    if (comp->comp_ctx == NULL) {
        pthread_rwlock_unlock(&bdp_ctx->p_vjetty_id_table.lock);
        URMA_LOG_ERR("Null bjetty_ctx in bdp_comp\n");
        return NULL;
    }
    atomic_fetch_add(&comp->use_cnt.atomic_cnt, 1);
    pthread_rwlock_unlock(&bdp_ctx->p_vjetty_id_table.lock);
    return (bjetty_ctx_t *)comp->comp_ctx;
}
/**
 * After calling the `get_bjetty_xxx` function, it is necessary to call this function to decrement the reference count.
 * The encapsulation of this function is primarily aimed at making the src/urma easier to understand.
 */
static inline void put_bjetty_ctx(const bjetty_ctx_t *bjetty_ctx)
{
    if (bjetty_ctx == NULL) {
        return;
    }
    atomic_fetch_sub(&bjetty_ctx->bdp_comp->use_cnt.atomic_cnt, 1);
}
/**
 * When the cr status is URMA_CR_WR_SUSPEND_DONE or URMA_CR_WR_FLUSH_ERR_DONE,
 * it indicates that the CR is a fake one constructed by hardware.
 * At this time, the `urma_ctx` field in CR is invalid and most likely 0.
 */
static inline bool is_cr_user_ctx_valid(const urma_cr_t *cr)
{
    return !(cr->status == URMA_CR_WR_SUSPEND_DONE || cr->status == URMA_CR_WR_FLUSH_ERR_DONE);
}

/**
 * Convert the local_id field of a CR from pjetty.id to vjetty.id if possible.
 * @return: If conversion is possible, perform the conversion and return 0; otherwise, do not replace and return -1.
 */
static int restore_cr_local_id(bondp_context_t *bdp_ctx,  int dev_idx, urma_cr_t *cr)
{
    uint32_t vjetty_id;
    urma_jetty_id_t pjetty_id = {
        .eid = bdp_ctx->p_ctxs[dev_idx]->eid,
        .id = cr->local_id,
    };
    int ret = bdp_p_vjetty_id_table_lookup(&bdp_ctx->p_vjetty_id_table, pjetty_id, get_p_vjetty_type_by_cr(cr),
        &vjetty_id);
    if (ret != 0) {
        URMA_LOG_ERR("Failed to get vjetty.id of local_id: %u, ret: %d\n", pjetty_id.id, ret);
        return -1;
    }
    cr->local_id = vjetty_id;
    return 0;
}
/**
 * Convert the remote_id field of a CR from pjetty_id to vjetty_id if possible.
 * @return: If conversion is possible, perform the conversion and return 0; otherwise, do not replace and return -1.
 */
static int restore_cr_remote_id(bondp_context_t *bdp_ctx, urma_cr_t *cr)
{
    /* No need to fill remote id for send cr */
    if (cr->flag.bs.s_r == 0) {
        return 0;
    }
    urma_jetty_id_t jetty_id = {0};
    int ret = bdp_r_p2v_jetty_id_table_lookup(&bdp_ctx->remote_p2v_jetty_id_table,
        &cr->remote_id, get_remote_id_type_by_cr(cr), &jetty_id);
    if (ret != 0) {
        return -1;
    }
    cr->remote_id = jetty_id;
    return 0;
}
/**
 * In scenarios where the remote_id cannot be restored,
 * the topo_map can be used to restore only the remote_id.eid field,
 * while other fields cannot be restored and will be set to 0.
 */
static void restore_cr_remote_id_fallback(bondp_context_t *bdp_ctx, urma_cr_t *cr)
{
    urma_eid_t eid_output = {0};
    int ret = get_bonding_eid_by_target_eid(bdp_ctx->topo_map, &cr->remote_id.eid, &eid_output);
    if (ret != 0) {
        // Set EID to all zero in error case
        cr->remote_id.eid.in6.interface_id = 0;
        cr->remote_id.eid.in6.subnet_prefix = 0;
    } else {
        cr->remote_id.eid = eid_output;
    }
    cr->remote_id.uasid = 0;
    cr->remote_id.id = 0;
}

typedef enum cr_convert_ret {
    CONVERT_FAIL     = -1,
    CONVERT_SUCCESS = 0,
    CONVERT_SKIP = 1,
} cr_convert_ret_t;

/**
 * @param dev_idx: The index of pjfc used when polling jfc.
 * In fact, what we need is the index of pjetty relative to vjetty, but we cannot obtain it when calling this function.
 * Considering that the binding relationship between pjfc and pjetty keeps their indices consistent,
 * we use the index of pjfc as the index for pjetty.
 */
static cr_convert_ret_t bondp_handle_cr_no_store(bondp_context_t *bdp_ctx, int idx, urma_cr_t *cr)
{
    // Special handling is applied to the CRs constructed by the hardware of SUSPEND_DONE and FLUSH_ERROR_DONE.
    if (!is_cr_user_ctx_valid(cr)) {
        // find out the bjetty_ctx
        bjetty_ctx_t *bjetty_ctx = get_bjetty_ctx_by_cr(bdp_ctx, idx, cr);
        if (bjetty_ctx == NULL) {
            return CONVERT_FAIL;
        }
        uint8_t target_state_bit = 0;
        if (cr->status == URMA_CR_WR_SUSPEND_DONE) {
            target_state_bit = PJETTY_SUSPEND_DONE;
        } else if (cr->status == URMA_CR_WR_FLUSH_ERR_DONE) {
            target_state_bit = PJETTY_FLUSH_ERROR_DONE;
        } else {
            URMA_LOG_ERR("Invalid cr error status: %d\n", cr->status);
            put_bjetty_ctx(bjetty_ctx);
            return CONVERT_FAIL;
        }
        bjetty_ctx->pjettys_error_done[idx] |= target_state_bit;
        bool all_reported = true;
        // pjetty_idx
        for (int idx = 0; idx < URMA_UBAGG_DEV_MAX_NUM; idx++) {
            if (bjetty_ctx->pjettys[idx] == NULL) {
                continue;
            }
            if ((bjetty_ctx->pjettys_error_done[idx] & target_state_bit) == 0) {
                all_reported = false;
                break;
            }
        }
        if (all_reported) {
            // restore local_id, method #2 through bjetty_ctx
            cr->local_id = get_comp_urma_jetty_id(bjetty_ctx->bdp_comp)->id;
            // restore remote_id
            if (restore_cr_remote_id(bdp_ctx, cr) != 0) {
                restore_cr_remote_id_fallback(bdp_ctx, cr);
            }
            /* Caller should copy this CR to output array. */
            put_bjetty_ctx(bjetty_ctx);
            return CONVERT_SUCCESS;
        }
        put_bjetty_ctx(bjetty_ctx);
        return CONVERT_SKIP;
    }
    if (restore_cr_local_id(bdp_ctx, idx, cr) != 0) {
        cr->local_id = 0; /* Replace with invalid value under exceptional circumstances */
    }
    /* Perform remote_id restoration on both the sending and receiving ends.
    Although the remote_id field on the sending end cannot be guaranteed to be correct,
    according to the description of urma_cr_t.
    However, it has been found in actual testing that this field is generally valid, hence this approach is taken.
    */
    if (restore_cr_remote_id(bdp_ctx, cr) != 0) {
        restore_cr_remote_id_fallback(bdp_ctx, cr);
    }
    /* Caller should copy this CR to output array. */
    return CONVERT_SUCCESS;
}

static cr_convert_ret_t bondp_handle_cr_with_store(bondp_context_t *bdp_ctx, bondp_jfc_t *bdp_jfc, int idx, urma_cr_t *cr)
{
    /* Handle CR with status URMA_CR_WR_SUSPEND_DONE or URMA_CR_WR_FLUSH_ERR_DONE */
    if (!is_cr_user_ctx_valid(cr)) {
        /* For these CRs where the user_ctx does not exist, simply restore the necessary values and then skip them. */
        if (restore_cr_local_id(bdp_ctx, idx, cr) != 0) {
            cr->local_id = 0; /* Replace with invalid value under exceptional circumstances */
        }
        /*
        Always attempt to restore the remote_id field, as it has been found to be correct in many cases during testing.
        Even if it is incorrect, we can invalidate it using the fallback function.
        */
        if (restore_cr_remote_id(bdp_ctx, cr) != 0) {
            restore_cr_remote_id_fallback(bdp_ctx, cr);
        }

        /* Caller should copy this CR to output array. */
        return CONVERT_SUCCESS;
    }

    const uint64_t wr_id = cr->user_ctx;
    bjetty_ctx_t *bjetty_ctx;
    bondp_cr_handler_ret_t ret;
    if (is_recv_cr(cr)) {
        jfr_wr_entry_t *wr_entry = jfr_wr_buf_get(&bdp_jfc->wr_buf, wr_id);
        if (wr_entry == NULL) {
            // wr_entry could not be NULL
            return CONVERT_FAIL;
        }
        bjetty_ctx = wr_entry->bjetty_ctx;
        ret = handle_recv(cr, wr_entry);
        free_jfr_wr(&wr_entry->wr);
        jfr_wr_buf_release(wr_entry);
    } else {
        jfs_wr_entry_t *wr_entry = jfs_wr_buf_get(&bdp_jfc->wr_buf, wr_id);
        if (wr_entry == NULL) {
            // pwr could not be NULL
            return CONVERT_FAIL;
        }
        bjetty_ctx = wr_entry->bjetty_ctx;
        ret = handle_send(cr, wr_entry);
        free_jfs_wr(&wr_entry->wr);
        jfs_wr_buf_release(wr_entry);
    }

    if (bjetty_ctx->pjettys_valid[idx] && is_device_error(cr->status)) {
        bjetty_ctx->pjettys_valid[idx] = false;
    }

    if (ret != CR_HANDLER_SUCCESS_AND_SKIP && ret != CR_HANDLER_ERR_AND_SKIP) {
        /* restore cr local id with bjetty_ctx */
        /* non-null return value because bjetty_ctx->bdp_comp type is checked in `get_bjetty_ctx_by_cr` */
        cr->local_id = get_comp_urma_jetty_id(bjetty_ctx->bdp_comp)->id;
        /*
        Perform remote_id restoration on both the sending and receiving ends.
        Although the remote_id field on the sending end cannot be guaranteed to be correct,
        according to the description of urma_cr_t.
        However, it has been found in actual testing that this field is generally valid, hence this approach is taken.
        */
        if (restore_cr_remote_id(bdp_ctx, cr) != 0) {
            restore_cr_remote_id_fallback(bdp_ctx, cr);
        }
        /* Caller should copy this CR to output array. */
        return CONVERT_SUCCESS;
    }
    return (ret == CR_HANDLER_ERR_AND_COPY || ret == CR_HANDLER_ERR_AND_SKIP) ? CONVERT_FAIL : CONVERT_SKIP;
}

int bondp_poll_jfc(urma_jfc_t *jfc, int cr_cnt, urma_cr_t *cr)
{
    static thread_local urma_cr_t pcr_buf[URMA_UBAGG_MAX_CR_CNT_PER_DEV];

    bondp_context_t *bdp_ctx = CONTAINER_OF_FIELD(jfc->urma_ctx, bondp_context_t, v_ctx);
    bondp_jfc_t *bdp_jfc = CONTAINER_OF_FIELD(jfc, bondp_jfc_t, v_jfc);

    int cr_cnt_remaining = cr_cnt;

    /* Start polling from the next device index to avoid starvation. */
    int start_idx = bdp_jfc->lasted_polled_jfc_idx + 1;

    for (int i = 0; i < bdp_jfc->dev_num && cr_cnt_remaining > 0; i++) {
        int idx = ((start_idx + i) % bdp_jfc->dev_num);

        if (bdp_jfc->p_jfc[idx] == NULL) {
            continue;
        }

        int pcr_cnt_max = cr_cnt_remaining > URMA_UBAGG_MAX_CR_CNT_PER_DEV
                              ? URMA_UBAGG_MAX_CR_CNT_PER_DEV
                              : cr_cnt_remaining;
        int pcr_cnt = urma_poll_jfc(bdp_jfc->p_jfc[idx], pcr_cnt_max, pcr_buf);
        if (pcr_cnt < 0) {
            return pcr_cnt;
        }
        if (pcr_cnt == 0) {
            continue;
        }

        for (int cr_id = 0; cr_id < pcr_cnt; cr_id++) {
            urma_cr_t *pcr = &pcr_buf[cr_id];
            cr_convert_ret_t conv_ret;

            if (is_single_dev_mode(&bdp_ctx->v_ctx)) {
                conv_ret = bondp_handle_cr_no_store(bdp_ctx,idx, pcr);
            } else {
                conv_ret = bondp_handle_cr_with_store(bdp_ctx, bdp_jfc, idx, pcr);
            }
            if (conv_ret == CONVERT_FAIL) {
                return -1;
            }
            if (conv_ret == CONVERT_SUCCESS) {
                cr[cr_cnt - cr_cnt_remaining] = *pcr;
                cr_cnt_remaining--;
            }
        }

        bdp_jfc->lasted_polled_jfc_idx = idx;
    }

    return cr_cnt - cr_cnt_remaining;
}

int bondp_flush_jetty(urma_jetty_t *jetty, int cr_cnt, urma_cr_t *cr)
{
    static thread_local urma_cr_t pcr_buf[URMA_UBAGG_MAX_CR_CNT_PER_DEV];

    bondp_context_t *bdp_ctx = CONTAINER_OF_FIELD(jetty->urma_ctx, bondp_context_t, v_ctx);
    bondp_comp_t *bdp_jetty = CONTAINER_OF_FIELD(jetty, bondp_comp_t, v_jetty);

    int cr_cnt_remaining = cr_cnt;

    for (int i = 0; i < bdp_jetty->dev_num && cr_cnt_remaining > 0; i++) {
        if (bdp_jetty->p_jetty[i] == NULL) {
            continue;
        }

        int pcr_cnt_max = cr_cnt_remaining > URMA_UBAGG_MAX_CR_CNT_PER_DEV
                              ? URMA_UBAGG_MAX_CR_CNT_PER_DEV
                              : cr_cnt_remaining;
        int pcr_cnt = urma_flush_jetty(bdp_jetty->p_jetty[i], pcr_cnt_max, pcr_buf);

        if (pcr_cnt < 0) {
            URMA_LOG_ERR("Failed to flush pjetty[%d]: %d\n", i, pcr_cnt);
            return pcr_cnt;
        }
        if (pcr_cnt == 0) {
            continue;
        }

        for (int cr_id = 0; cr_id < pcr_cnt; cr_id++) {
            urma_cr_t *pcr = &pcr_buf[cr_id];
            cr_convert_ret_t conv_ret;

            if (is_single_dev_mode(&bdp_ctx->v_ctx)) {
                conv_ret = bondp_handle_cr_no_store(bdp_ctx, i, pcr);
            } else {
                conv_ret = bondp_handle_cr_with_store(bdp_ctx, bdp_jetty->send_jfc, i, pcr);
            }
            if (conv_ret == CONVERT_FAIL) {
                return -1;
            }
            if (conv_ret == CONVERT_SUCCESS) {
                cr[cr_cnt - cr_cnt_remaining] = *pcr;
                cr_cnt_remaining--;
            }
        }
    }

    return cr_cnt - cr_cnt_remaining;
}
