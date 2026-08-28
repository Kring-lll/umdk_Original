/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bond provider datapath convert implementation file
 * Author: Wang Hang
 * Create: 2026-04-02
 * Note:
 * History: 2026-04-02   Create File
 */

#include <stdlib.h>
#include <string.h>

#include "ub_util.h"

#include "bondp_topo_info.h"
#include "bondp_types.h"
#include "bondp_cp_tjetty.h"
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
    } else if (is_atomic_wr(wr)) {
        if (wr->cas.src != NULL) {
            free(wr->cas.src);
            wr->cas.src = NULL;
        }
        if (wr->cas.dst != NULL) {
            free(wr->cas.dst);
            wr->cas.dst = NULL;
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

static int copy_sg_list(const urma_sg_t *src, urma_sg_t *dst, urma_sge_t *prealloc_sge, uint32_t max_sge)
{
    dst->num_sge = src->num_sge;
    dst->sge = NULL;

    if (dst->num_sge > 0) {
        if (prealloc_sge != NULL) {
            if (dst->num_sge > max_sge) {
                URMA_LOG_ERR("The number of SGE(%u) exceeds the limit(%u)",
                             dst->num_sge, max_sge);
                return -1;
            }
            dst->sge = prealloc_sge;
        } else {
            dst->sge = (urma_sge_t *)malloc(dst->num_sge * sizeof(urma_sge_t));
            if (dst->sge == NULL) {
                return -1;
            }
        }
        (void)memcpy(dst->sge, src->sge, src->num_sge * sizeof(urma_sge_t));
    }
    return 0;
}

static int copy_atomic_sge(const urma_sge_t *src, urma_sge_t **dst, urma_sge_t *prealloc_sge)
{
    if (src == NULL) {
        *dst = NULL;
        return 0;
    }

    if (prealloc_sge != NULL) {
        *dst = prealloc_sge;
    } else {
        *dst = (urma_sge_t *)malloc(sizeof(urma_sge_t));
        if (*dst == NULL) {
            return -1;
        }
    }
    (void)memcpy(*dst, src, sizeof(urma_sge_t));
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
 *   URMA_OPC_CAS, URMA_OPC_FADD
 */
urma_status_t copy_jfs_wr(const urma_jfs_wr_t *src, urma_jfs_wr_t *dst,
                          urma_sge_t *prealloc_src_sge, urma_sge_t *prealloc_dst_sge,
                          uint32_t max_sge, uint32_t max_rsge)
{
    *dst = *src;
    dst->next = NULL;

    if (is_rw_wr(src)) {
        if (copy_sg_list(&src->rw.src, &dst->rw.src, prealloc_src_sge, max_sge) != 0 ||
            copy_sg_list(&src->rw.dst, &dst->rw.dst, prealloc_dst_sge, max_rsge) != 0) {
            return URMA_ENOMEM;
        }
    } else if (is_send_wr(src)) {
        if (copy_sg_list(&src->send.src, &dst->send.src, prealloc_src_sge, max_sge) != 0) {
            return URMA_ENOMEM;
        }
    } else if (is_atomic_wr(src)) {
        if (copy_atomic_sge(src->cas.src, &dst->cas.src, prealloc_src_sge) != 0 ||
            copy_atomic_sge(src->cas.dst, &dst->cas.dst, prealloc_dst_sge) != 0) {
            return URMA_ENOMEM;
        }
    } else {
        return URMA_EINVAL;
    }
    return 0;
}

/**
 * Performs a deep copy of a JFR work request.
 */
urma_status_t copy_jfr_wr(const urma_jfr_wr_t *src, urma_jfr_wr_t *dst,
                          urma_sge_t *prealloc_src_sge, uint32_t max_sge)
{
    *dst = *src;
    dst->next = NULL;

    if (copy_sg_list(&src->src, &dst->src, prealloc_src_sge, max_sge) != 0) {
        return URMA_ENOMEM;
    }
    return URMA_SUCCESS;
}

/**
 * 64-bit imm_data format:
 *
 *   Bits   | Field       | Bits | Description
 *   -------|-------------|------|--------------------------------
 *   0-20   | user_data   | 21   | User-defined custom data
 *   21-21  | reserved    | 1    | Reserved
 *   22-37  | vjetty_id   | 16   | Virtual jetty identifier (0-65535)
 *   38-39  | cr_opcode   | 2    | Operation code tag (0-3)
 *   40-63  | msn         | 24   | Message sequence number (0-16M)
 *
 * Use encode_imm_data() and decode_imm_data() to pack/unpack fields.
 */

#define IMM_USER_BITS      21
#define IMM_RESERVED_BITS  1
#define IMM_VJETTY_ID_BITS 16
#define IMM_CR_OPCODE_BITS 2
#define IMM_MSN_BITS       24

#define IMM_USER_SHIFT      0
#define IMM_RESERVED_SHIFT  (IMM_USER_SHIFT + IMM_USER_BITS)
#define IMM_VJETTY_ID_SHIFT (IMM_RESERVED_SHIFT + IMM_RESERVED_BITS)
#define IMM_CR_OPCODE_SHIFT (IMM_VJETTY_ID_SHIFT + IMM_VJETTY_ID_BITS)
#define IMM_MSN_SHIFT       (IMM_CR_OPCODE_SHIFT + IMM_CR_OPCODE_BITS)

#define IMM_USER_MASK      ((1ULL << IMM_USER_BITS) - 1)
#define IMM_VJETTY_ID_MASK ((1ULL << IMM_VJETTY_ID_BITS) - 1)
#define IMM_CR_OPCODE_MASK ((1ULL << IMM_CR_OPCODE_BITS) - 1)
#define IMM_MSN_MASK       ((1ULL << IMM_MSN_BITS) - 1)

static inline uint64_t encode_imm_data(uint32_t cr_opcode, uint32_t msn, uint32_t vjetty_id,
                                       uint64_t user_data, bool msn_enable)
{
    uint64_t imm_data = 0;

    imm_data |= ((uint64_t)cr_opcode & IMM_CR_OPCODE_MASK) << IMM_CR_OPCODE_SHIFT;
    if (msn_enable) {
        imm_data |= ((uint64_t)msn & IMM_MSN_MASK) << IMM_MSN_SHIFT;
    } else {
        imm_data |= (((uint64_t)user_data >> IMM_MSN_SHIFT) & IMM_MSN_MASK) << IMM_MSN_SHIFT;
    }
    imm_data |= ((uint64_t)vjetty_id & IMM_VJETTY_ID_MASK) << IMM_VJETTY_ID_SHIFT;
    imm_data |= ((uint64_t)user_data & IMM_USER_MASK) << IMM_USER_SHIFT;
    return imm_data;
}

static inline void decode_imm_data(uint64_t imm_data, uint32_t *cr_opcode, uint32_t *msn,
                                   uint32_t *vjetty_id, uint64_t *user_data, bool msn_enable)
{
    *cr_opcode = (uint32_t)((imm_data >> IMM_CR_OPCODE_SHIFT) & IMM_CR_OPCODE_MASK);
    *vjetty_id = (uint32_t)((imm_data >> IMM_VJETTY_ID_SHIFT) & IMM_VJETTY_ID_MASK);
    *user_data = (uint64_t)((imm_data >> IMM_USER_SHIFT) & IMM_USER_MASK);
    if (msn_enable) {
        *msn = (uint32_t)((imm_data >> IMM_MSN_SHIFT) & IMM_MSN_MASK);
    } else {
        *user_data |= (uint64_t)(((imm_data >> IMM_MSN_SHIFT) & IMM_MSN_MASK) << IMM_MSN_SHIFT);
    }
}

static inline urma_target_jetty_t *get_p_tjetty(urma_target_jetty_t *tjetty, int send_idx, int target_idx)
{
    if (tjetty == NULL) {
        return NULL;
    }
    bondp_target_jetty_t *bdp_tjetty = CONTAINER_OF_FIELD(tjetty, bondp_target_jetty_t, v_tjetty);
    const bondp_p_target_jetty_t *p_tjetty = bondp_find_p_tjetty_const(bdp_tjetty, send_idx, target_idx);
    return p_tjetty != NULL ? p_tjetty->p_tjetty : NULL;
}

static inline urma_target_seg_t *get_p_tseg(urma_target_seg_t *tseg, int local_idx, int remote_idx)
{
    if (tseg == NULL) {
        return NULL;
    }
    /* Use token_id to distinguish local register seg and imported seg
       This is useful for write ops */
    if (tseg->token_id != NULL) {
        return CONTAINER_OF_FIELD(tseg, bondp_tseg_t, v_tseg)->p_tseg[local_idx];
    } else {
        return CONTAINER_OF_FIELD(tseg, bondp_import_tseg_t, v_tseg)->p_tseg[local_idx][remote_idx];
    }
}

static inline urma_target_seg_t *get_v_tseg(urma_target_seg_t *tseg)
{
    if (tseg == NULL) {
        URMA_LOG_WARN_RL("get_v_tseg called with NULL tseg; bind wrote a NULL p_tseg (path not ready or seg freed)\n");
        return NULL;
    }
    return (urma_target_seg_t *)(uintptr_t)tseg->handle;
}

uint32_t bondp_user_tseg_total_len(const urma_user_tseg_t *ut)
{
    if (ut == NULL || ut->attr.bs.has_user_info == 0) {
        return 0;
    }

    const urma_user_info_ext_hdr_t *ext_hdr =
        (const urma_user_info_ext_hdr_t *)((uintptr_t)ut + sizeof(*ut));
    uint64_t total = (uint64_t)sizeof(*ut) + sizeof(*ext_hdr) + ext_hdr->len;
    if (total > BONDP_USER_TSEG_MAX_LEN) {
        return 0;
    }
    return (uint32_t)total;
}

static bool bondp_user_tseg_peer_ctx_valid(const urma_bond_user_tseg_ext_v0_t *ext)
{
    const bondp_user_tseg_peer_ctx_t *peer_ctx =
        (const bondp_user_tseg_peer_ctx_t *)((uintptr_t)ext + sizeof(*ext));

    for (uint32_t i = 0; i < ext->peer_cnt; ++i) {
        if (peer_ctx[i].peer_idx >= URMA_UBAGG_DEV_MAX_NUM) {
            return false;
        }
        for (uint32_t j = 0; j < i; ++j) {
            if (peer_ctx[j].peer_idx == peer_ctx[i].peer_idx) {
                return false;
            }
        }
    }
    return true;
}

const urma_bond_user_tseg_ext_v0_t *bondp_user_tseg_get_ext(const urma_user_tseg_t *ut)
{
    if (bondp_user_tseg_total_len(ut) == 0) {
        return NULL;
    }

    const urma_user_info_ext_hdr_t *ext_hdr =
        (const urma_user_info_ext_hdr_t *)((uintptr_t)ut + sizeof(*ut));
    const urma_bond_user_tseg_ext_v0_t *ext = (const urma_bond_user_tseg_ext_v0_t *)ext_hdr->data;
    if (ext_hdr->len < sizeof(*ext) ||
        ext_hdr->len < sizeof(*ext) + (uint64_t)ext->peer_cnt * sizeof(bondp_user_tseg_peer_ctx_t) ||
        ext->version != 0 || ext->peer_cnt == 0 || ext->peer_cnt > URMA_UBAGG_DEV_MAX_NUM ||
        !bondp_user_tseg_peer_ctx_valid(ext)) {
        return NULL;
    }
    return ext;
}

int bondp_user_tseg_lookup_token(const urma_bond_user_tseg_ext_v0_t *ext, int target_idx,
                                 uint32_t *token_id)
{
    if (ext == NULL || token_id == NULL || target_idx < 0 || target_idx >= URMA_UBAGG_DEV_MAX_NUM) {
        return -1;
    }

    const bondp_user_tseg_peer_ctx_t *peer_ctx =
        (const bondp_user_tseg_peer_ctx_t *)((uintptr_t)ext + sizeof(*ext));
    for (uint32_t i = 0; i < ext->peer_cnt; ++i) {
        if (peer_ctx[i].peer_idx == (uint8_t)target_idx) {
            *token_id = peer_ctx[i].token_id;
            return 0;
        }
    }
    return -1;
}

static bool sge_is_import_free(const urma_sge_t *sge)
{
    return sge != NULL && sge->tseg == NULL && sge->user_tseg != NULL;
}

uint32_t jfs_wr_count_remote_user_tseg(const urma_jfs_wr_t *wr)
{
    if (wr == NULL) {
        return 0;
    }

    uint32_t count = 0;
    if (is_rw_wr(wr)) {
        if (jfs_wr_sge_is_remote(wr, true)) {
            for (uint32_t i = 0; i < wr->rw.src.num_sge; ++i) {
                count += sge_is_import_free(&wr->rw.src.sge[i]) ? 1 : 0;
            }
        }
        if (jfs_wr_sge_is_remote(wr, false)) {
            for (uint32_t i = 0; i < wr->rw.dst.num_sge; ++i) {
                count += sge_is_import_free(&wr->rw.dst.sge[i]) ? 1 : 0;
            }
        }
    } else if (is_atomic_wr(wr)) {
        const urma_sge_t *dst = (wr->opcode == URMA_OPC_CAS) ? wr->cas.dst : wr->faa.dst;
        count += sge_is_import_free(dst) ? 1 : 0;
    }
    /* SEND variants have no remote SGE. */
    return count;
}

static urma_status_t bondp_clone_sge_user_tseg(urma_sge_t *sge, bool remote, uint8_t *slots,
                                               uint32_t slot_stride, uint32_t *slot_idx,
                                               uint32_t max_slots)
{
    if (sge->tseg != NULL) {
        /* Import path takes precedence; user_tseg is ignored per API contract.
         * Drop it so the stored WR never keeps a caller-owned pointer. */
        sge->user_tseg = NULL;
        return URMA_SUCCESS;
    }
    if (sge->user_tseg == NULL || !remote) {
        /* Local SGEs forbid user_tseg; check_jfs_wr_path rejects them. */
        return URMA_SUCCESS;
    }

    uint32_t total = bondp_user_tseg_total_len(sge->user_tseg);
    if (total == 0) {
        URMA_LOG_ERR("Invalid bonding user_tseg buffer (no extension or oversize).\n");
        return URMA_EINVAL;
    }
    if (*slot_idx >= max_slots) {
        URMA_LOG_ERR("Bonding user_tseg slots exhausted, slot_idx=%u, max_slots=%u.\n",
                     *slot_idx, max_slots);
        return URMA_EINVAL;
    }

    uint8_t *slot = slots + (uint64_t)(*slot_idx) * slot_stride;
    (void)memcpy(slot, sge->user_tseg, total);
    sge->user_tseg = (urma_user_tseg_t *)slot;
    (*slot_idx)++;
    return URMA_SUCCESS;
}

urma_status_t bondp_clone_wr_user_tseg(urma_jfs_wr_t *wr, uint8_t *ut_ext_slots,
                                       uint32_t slot_stride, uint32_t *slot_idx, uint32_t max_slots)
{
    if (wr == NULL || ut_ext_slots == NULL || slot_idx == NULL) {
        return URMA_SUCCESS;
    }

    urma_status_t ret;
    if (is_rw_wr(wr)) {
        bool src_remote = jfs_wr_sge_is_remote(wr, true);
        for (uint32_t i = 0; i < wr->rw.src.num_sge; ++i) {
            ret = bondp_clone_sge_user_tseg(&wr->rw.src.sge[i], src_remote, ut_ext_slots,
                                            slot_stride, slot_idx, max_slots);
            if (ret != URMA_SUCCESS) {
                return ret;
            }
        }
        bool dst_remote = jfs_wr_sge_is_remote(wr, false);
        for (uint32_t i = 0; i < wr->rw.dst.num_sge; ++i) {
            ret = bondp_clone_sge_user_tseg(&wr->rw.dst.sge[i], dst_remote, ut_ext_slots,
                                            slot_stride, slot_idx, max_slots);
            if (ret != URMA_SUCCESS) {
                return ret;
            }
        }
        return URMA_SUCCESS;
    }
    if (is_atomic_wr(wr)) {
        /* Atomic src is local, dst is remote. */
        urma_sge_t *src = (wr->opcode == URMA_OPC_CAS) ? wr->cas.src : wr->faa.src;
        urma_sge_t *dst = (wr->opcode == URMA_OPC_CAS) ? wr->cas.dst : wr->faa.dst;
        if (src != NULL) {
            ret = bondp_clone_sge_user_tseg(src, false, ut_ext_slots, slot_stride, slot_idx,
                                            max_slots);
            if (ret != URMA_SUCCESS) {
                return ret;
            }
        }
        if (dst != NULL) {
            ret = bondp_clone_sge_user_tseg(dst, true, ut_ext_slots, slot_stride, slot_idx,
                                            max_slots);
            if (ret != URMA_SUCCESS) {
                return ret;
            }
        }
        return URMA_SUCCESS;
    }
    /* SEND variants only have local src SGEs; sanitize their user_tseg too. */
    if (is_send_wr(wr) && wr->send.src.sge != NULL) {
        for (uint32_t i = 0; i < wr->send.src.num_sge; ++i) {
            ret = bondp_clone_sge_user_tseg(&wr->send.src.sge[i], false, ut_ext_slots,
                                            slot_stride, slot_idx, max_slots);
            if (ret != URMA_SUCCESS) {
                return ret;
            }
        }
    }
    return URMA_SUCCESS;
}

static int check_local_sge(const urma_sge_t *sge, int send_idx, int target_idx)
{
    /* Local SGEs forbid user_tseg: tseg must be present and bound on the path. */
    if (sge == NULL || sge->tseg == NULL ||
        get_p_tseg(sge->tseg, send_idx, target_idx) == NULL) {
        URMA_LOG_ERR("Failed to bind local seg to path, send_idx=%d, target_idx=%d.\n",
                     send_idx, target_idx);
        return -1;
    }
    return 0;
}

static int check_remote_sge(const urma_sge_t *sge, int send_idx, int target_idx)
{
    if (sge == NULL) {
        return -1;
    }
    if (sge->tseg != NULL) {
        /* Import path: user_tseg is ignored when both are set (API contract). */
        if (get_p_tseg(sge->tseg, send_idx, target_idx) == NULL) {
            URMA_LOG_ERR("Failed to bind remote seg to path, send_idx=%d, target_idx=%d.\n",
                         send_idx, target_idx);
            return -1;
        }
        return 0;
    }
    if (sge->user_tseg == NULL) {
        URMA_LOG_ERR("Remote sge has neither tseg nor user_tseg.\n");
        return -1;
    }

    /* Import-free path: the bonding extension must be valid and contain the
     * token_id of the slave device selected for this path. */
    const urma_bond_user_tseg_ext_v0_t *ext = bondp_user_tseg_get_ext(sge->user_tseg);
    uint32_t token_id = 0;
    if (ext == NULL) {
        URMA_LOG_ERR("Import-free seg ext invalid (outer token_id=%u, has_user_info=%u).\n",
                     sge->user_tseg->token_id, sge->user_tseg->attr.bs.has_user_info);
        return -1;
    }
    if (bondp_user_tseg_lookup_token(ext, target_idx, &token_id) != 0) {
        const bondp_user_tseg_peer_ctx_t *peer_ctx =
            (const bondp_user_tseg_peer_ctx_t *)((uintptr_t)ext + sizeof(*ext));
        for (uint32_t i = 0; i < ext->peer_cnt && i < URMA_UBAGG_DEV_MAX_NUM; ++i) {
            URMA_LOG_ERR("Import-free ext peer[%u]: peer_idx=%u, token_id=%u "
                         "(need target_idx=%d).\n", i, peer_ctx[i].peer_idx,
                         peer_ctx[i].token_id, target_idx);
        }
        URMA_LOG_ERR("Failed to bind import-free seg to path, no peer for target_idx=%d, "
                     "send_idx=%d, peer_cnt=%u.\n", target_idx, send_idx, ext->peer_cnt);
        return -1;
    }
    return 0;
}

static int check_sg_path(const urma_sg_t *sg, bool remote, int send_idx, int target_idx)
{
    for (uint32_t i = 0; i < sg->num_sge; ++i) {
        int ret = remote ? check_remote_sge(&sg->sge[i], send_idx, target_idx)
                         : check_local_sge(&sg->sge[i], send_idx, target_idx);
        if (ret != 0) {
            return -1;
        }
    }
    return 0;
}

static int check_send_wr_path(urma_jfs_wr_t *send_wr, int send_idx, int target_idx)
{
    /* SEND variants only have local src SGEs. */
    if (send_wr->send.src.num_sge > 0 && send_wr->send.src.sge != NULL &&
        check_sg_path(&send_wr->send.src, false, send_idx, target_idx) != 0) {
        return -1;
    }
    return 0;
}

static int check_write_wr_path(urma_jfs_wr_t *send_wr, int send_idx, int target_idx)
{
    /* READ: src is remote, dst is local; WRITE variants: the other way round. */
    bool src_remote = jfs_wr_sge_is_remote(send_wr, true);
    if (check_sg_path(&send_wr->rw.src, src_remote, send_idx, target_idx) != 0 ||
        check_sg_path(&send_wr->rw.dst, !src_remote, send_idx, target_idx) != 0) {
        return -1;
    }
    return 0;
}

static int check_atomic_wr_path(const urma_sge_t *src, const urma_sge_t *dst, int send_idx, int target_idx)
{
    /* Atomic src is local, dst is remote (possibly import-free). */
    if (src == NULL || dst == NULL ||
        check_local_sge(src, send_idx, target_idx) != 0 ||
        check_remote_sge(dst, send_idx, target_idx) != 0) {
        return -1;
    }
    return 0;
}

urma_status_t check_jfs_wr_path(urma_jfs_wr_t *wr, int send_idx, int target_idx)
{
    if (get_p_tjetty(wr->tjetty, send_idx, target_idx) == NULL) {
        URMA_LOG_ERR("Failed to bind WR to path, pjetty is NULL, send_idx=%d, target_idx=%d.\n",
                     send_idx, target_idx);
        return URMA_EINVAL;
    }

    switch (wr->opcode) {
        case URMA_OPC_SEND:
        case URMA_OPC_SEND_IMM:
        case URMA_OPC_SEND_INVALIDATE:
            return (check_send_wr_path(wr, send_idx, target_idx) == 0) ? URMA_SUCCESS : URMA_EINVAL;
        case URMA_OPC_WRITE:
        case URMA_OPC_WRITE_IMM:
        case URMA_OPC_WRITE_NOTIFY:
        case URMA_OPC_READ:
            return (check_write_wr_path(wr, send_idx, target_idx) == 0) ? URMA_SUCCESS : URMA_EINVAL;
        case URMA_OPC_CAS:
            return (check_atomic_wr_path(wr->cas.src, wr->cas.dst, send_idx, target_idx) == 0)
                       ? URMA_SUCCESS
                       : URMA_EINVAL;
        case URMA_OPC_FADD:
            return (check_atomic_wr_path(wr->faa.src, wr->faa.dst, send_idx, target_idx) == 0)
                       ? URMA_SUCCESS
                       : URMA_EINVAL;
        default:
            return URMA_SUCCESS;
    }
}

/*
 * Map one import-free remote SGE to the physical path: resolve the per-slave
 * token_id from the deep-copied extension and fill a bare urma_user_tseg_t in
 * bare_ut_scratch[]. The slot cursor advances exactly once per import-free
 * SGE, in the same fixed order as bondp_clone_wr_user_tseg, so restore stays
 * in sync even when the mapping fails (check_jfs_wr_path should have caught
 * such WRs earlier).
 */
static void map_user_tseg_sge_to_path(urma_sge_t *sge, int target_idx,
                                      urma_user_tseg_t *bare_ut_scratch,
                                      urma_user_tseg_t **ut_ext_ptr_save, uint32_t *slot_idx)
{
    if (sge->tseg != NULL || sge->user_tseg == NULL) {
        return; /* import path (or empty SGE): handled by the tseg mapping */
    }
    if (bare_ut_scratch == NULL || ut_ext_ptr_save == NULL) {
        URMA_LOG_ERR("No import-free scratch slots available.\n");
        return;
    }

    urma_user_tseg_t *copied = sge->user_tseg;
    ut_ext_ptr_save[*slot_idx] = copied; /* restore target, recorded even on failure */

    uint32_t peer_token_id = 0;
    const urma_bond_user_tseg_ext_v0_t *ext = bondp_user_tseg_get_ext(copied);
    if (ext == NULL || bondp_user_tseg_lookup_token(ext, target_idx, &peer_token_id) != 0) {
        /* Unreachable after check_jfs_wr_path; keep the virtual-form copy so
         * the provider fails the WR instead of crashing on a NULL deref. */
        URMA_LOG_ERR("No peer token for target_idx=%d on import-free seg "
                     "(ext=%p, outer token_id=%u, has_user_info=%u).\n",
                     target_idx, ext, copied->token_id, copied->attr.bs.has_user_info);
        (*slot_idx)++;
        return;
    }

    urma_user_tseg_t *bare = &bare_ut_scratch[*slot_idx];
    *bare = *copied;
    bare->attr.bs.has_user_info = 0; /* bare device semantics: no extension */
    bare->token_id = peer_token_id;  /* outer token_id is virtual, never used */
    sge->user_tseg = bare;
    sge->tseg = NULL;
    (*slot_idx)++;
    /* Import-free data path trace: the slave device receives a bare user_tseg
     * with the per-path token resolved from the bonding extension. DEBUG-level
     * to keep the post hot path quiet. */
    URMA_LOG_DEBUG("import-free seg mapped: target_idx=%d, peer_token_id=%u, "
                   "token_policy=%u.\n", target_idx, peer_token_id,
                   bare->attr.bs.token_policy);
}

/* Restore one import-free remote SGE back to the deep-copied extension. */
static void restore_user_tseg_sge(urma_sge_t *sge, urma_user_tseg_t **ut_ext_ptr_save,
                                  uint32_t *slot_idx)
{
    if (sge->tseg != NULL || sge->user_tseg == NULL) {
        return;
    }
    if (ut_ext_ptr_save == NULL) {
        sge->user_tseg = NULL; /* no saved copy: drop the stale bare pointer */
        return;
    }
    sge->user_tseg = ut_ext_ptr_save[*slot_idx];
    (*slot_idx)++;
}

static void map_send_vwr_to_path(urma_jfs_wr_t *send_wr, int send_idx, int target_idx)
{
    /* SEND variants only have local src SGEs. */
    if (send_wr->send.src.num_sge > 0 && send_wr->send.src.sge != NULL) {
        for (uint32_t i = 0; i < send_wr->send.src.num_sge; ++i) {
            send_wr->send.src.sge[i].tseg = get_p_tseg(send_wr->send.src.sge[i].tseg, send_idx, target_idx);
        }
    }
    send_wr->tjetty = get_p_tjetty(send_wr->tjetty, send_idx, target_idx);
}

static void restore_send_pwr_to_vwr(urma_jfs_wr_t *send_wr, urma_target_jetty_t *vtjetty)
{
    if (send_wr->send.src.num_sge > 0 && send_wr->send.src.sge != NULL) {
        for (uint32_t i = 0; i < send_wr->send.src.num_sge; ++i) {
            if (send_wr->send.src.sge[i].tseg != NULL) {
                send_wr->send.src.sge[i].tseg = get_v_tseg(send_wr->send.src.sge[i].tseg);
            }
        }
    }
    send_wr->tjetty = vtjetty;
}

static void map_write_vwr_to_path(urma_jfs_wr_t *send_wr, int send_idx, int target_idx,
                                  urma_user_tseg_t *bare_ut_scratch, urma_user_tseg_t **ut_ext_ptr_save,
                                  uint32_t *slot_idx)
{
    /* READ: src is remote, dst is local; WRITE variants: the other way round. */
    bool src_remote = jfs_wr_sge_is_remote(send_wr, true);
    for (uint32_t i = 0; i < send_wr->rw.src.num_sge; ++i) {
        if (src_remote) {
            map_user_tseg_sge_to_path(&send_wr->rw.src.sge[i], target_idx, bare_ut_scratch,
                                      ut_ext_ptr_save, slot_idx);
        }
        send_wr->rw.src.sge[i].tseg = get_p_tseg(send_wr->rw.src.sge[i].tseg, send_idx, target_idx);
    }
    for (uint32_t i = 0; i < send_wr->rw.dst.num_sge; ++i) {
        if (!src_remote) {
            map_user_tseg_sge_to_path(&send_wr->rw.dst.sge[i], target_idx, bare_ut_scratch,
                                      ut_ext_ptr_save, slot_idx);
        }
        send_wr->rw.dst.sge[i].tseg = get_p_tseg(send_wr->rw.dst.sge[i].tseg, send_idx, target_idx);
    }
    send_wr->tjetty = get_p_tjetty(send_wr->tjetty, send_idx, target_idx);
}

static void restore_write_pwr_to_vwr(urma_jfs_wr_t *send_wr, urma_target_jetty_t *vtjetty,
                                     urma_user_tseg_t **ut_ext_ptr_save, uint32_t *slot_idx)
{
    for (uint32_t i = 0; i < send_wr->rw.src.num_sge; ++i) {
        restore_user_tseg_sge(&send_wr->rw.src.sge[i], ut_ext_ptr_save, slot_idx);
        if (send_wr->rw.src.sge[i].tseg != NULL) {
            send_wr->rw.src.sge[i].tseg = get_v_tseg(send_wr->rw.src.sge[i].tseg);
        }
    }
    for (uint32_t i = 0; i < send_wr->rw.dst.num_sge; ++i) {
        restore_user_tseg_sge(&send_wr->rw.dst.sge[i], ut_ext_ptr_save, slot_idx);
        if (send_wr->rw.dst.sge[i].tseg != NULL) {
            send_wr->rw.dst.sge[i].tseg = get_v_tseg(send_wr->rw.dst.sge[i].tseg);
        }
    }
    send_wr->tjetty = vtjetty;
}

static void restore_cas_pwr_to_vwr(urma_jfs_wr_t *send_wr, urma_target_jetty_t *vtjetty,
                                   urma_user_tseg_t **ut_ext_ptr_save, uint32_t *slot_idx)
{
    if (send_wr->cas.src != NULL) {
        if (send_wr->cas.src->tseg != NULL) {
            send_wr->cas.src->tseg = get_v_tseg(send_wr->cas.src->tseg);
        }
    }
    if (send_wr->cas.dst != NULL) {
        restore_user_tseg_sge(send_wr->cas.dst, ut_ext_ptr_save, slot_idx);
        if (send_wr->cas.dst->tseg != NULL) {
            send_wr->cas.dst->tseg = get_v_tseg(send_wr->cas.dst->tseg);
        }
    }
    send_wr->tjetty = vtjetty;
}

static void restore_faa_pwr_to_vwr(urma_jfs_wr_t *send_wr, urma_target_jetty_t *vtjetty,
                                   urma_user_tseg_t **ut_ext_ptr_save, uint32_t *slot_idx)
{
    if (send_wr->faa.src != NULL) {
        if (send_wr->faa.src->tseg != NULL) {
            send_wr->faa.src->tseg = get_v_tseg(send_wr->faa.src->tseg);
        }
    }
    if (send_wr->faa.dst != NULL) {
        restore_user_tseg_sge(send_wr->faa.dst, ut_ext_ptr_save, slot_idx);
        if (send_wr->faa.dst->tseg != NULL) {
            send_wr->faa.dst->tseg = get_v_tseg(send_wr->faa.dst->tseg);
        }
    }
    send_wr->tjetty = vtjetty;
}

static void map_cas_vwr_to_path(urma_jfs_wr_t *send_wr, int send_idx, int target_idx,
                                urma_user_tseg_t *bare_ut_scratch, urma_user_tseg_t **ut_ext_ptr_save,
                                uint32_t *slot_idx)
{
    /* Atomic src is local, dst is remote (possibly import-free). */
    map_user_tseg_sge_to_path(send_wr->cas.dst, target_idx, bare_ut_scratch, ut_ext_ptr_save,
                              slot_idx);
    send_wr->cas.src->tseg = get_p_tseg(send_wr->cas.src->tseg, send_idx, target_idx);
    send_wr->cas.dst->tseg = get_p_tseg(send_wr->cas.dst->tseg, send_idx, target_idx);
    send_wr->tjetty = get_p_tjetty(send_wr->tjetty, send_idx, target_idx);
}

static void map_fadd_vwr_to_path(urma_jfs_wr_t *send_wr, int send_idx, int target_idx,
                                 urma_user_tseg_t *bare_ut_scratch, urma_user_tseg_t **ut_ext_ptr_save,
                                 uint32_t *slot_idx)
{
    map_user_tseg_sge_to_path(send_wr->faa.dst, target_idx, bare_ut_scratch, ut_ext_ptr_save,
                              slot_idx);
    send_wr->faa.src->tseg = get_p_tseg(send_wr->faa.src->tseg, send_idx, target_idx);
    send_wr->faa.dst->tseg = get_p_tseg(send_wr->faa.dst->tseg, send_idx, target_idx);
    send_wr->tjetty = get_p_tjetty(send_wr->tjetty, send_idx, target_idx);
}

void encode_jfs_wr_msn(urma_jfs_wr_t *wr, bondp_comp_t *bdp_comp, uint32_t msn, bool msn_enable)
{
    uint64_t opcode_tag = 0;

    switch (wr->opcode) {
        case URMA_OPC_SEND:
        case URMA_OPC_SEND_IMM:
        case URMA_OPC_SEND_INVALIDATE:
            if (wr->opcode == URMA_OPC_SEND) {
                opcode_tag = URMA_CR_OPC_SEND;
            } else if (wr->opcode == URMA_OPC_SEND_IMM) {
                opcode_tag = URMA_CR_OPC_SEND_WITH_IMM;
            } else if (wr->opcode == URMA_OPC_SEND_INVALIDATE) {
                opcode_tag = URMA_CR_OPC_SEND_WITH_INV;
            }
            wr->opcode = URMA_OPC_SEND_IMM;
            wr->send.imm_data = encode_imm_data(
                opcode_tag,
                msn,
                bdp_comp->v_jetty.jetty_id.id,
                wr->send.imm_data,
                msn_enable);
            return;
        case URMA_OPC_WRITE_IMM:
            opcode_tag = URMA_CR_OPC_WRITE_WITH_IMM;
            wr->rw.notify_data = encode_imm_data(
                opcode_tag,
                msn,
                bdp_comp->v_jetty.jetty_id.id,
                wr->rw.notify_data,
                msn_enable);
            return;
        case URMA_OPC_WRITE:
        case URMA_OPC_WRITE_NOTIFY:
        case URMA_OPC_READ:
        case URMA_OPC_CAS:
        case URMA_OPC_FADD:
            /* No MSN encoding needed for these opcodes */
            return;
        default:
            URMA_LOG_ERR("Unsupported send opcode\n");
            return;
    }
}

void convert_jfs_vwr_to_pwr(urma_jfs_wr_t *wr, int send_idx, int target_idx,
                            urma_user_tseg_t *bare_ut_scratch, urma_user_tseg_t **ut_ext_ptr_save)
{
    uint32_t slot_idx = 0;
    switch (wr->opcode) {
        case URMA_OPC_SEND:
        case URMA_OPC_SEND_IMM:
        case URMA_OPC_SEND_INVALIDATE:
            map_send_vwr_to_path(wr, send_idx, target_idx);
            return;
        case URMA_OPC_WRITE:
        case URMA_OPC_WRITE_IMM:
        case URMA_OPC_WRITE_NOTIFY:
        case URMA_OPC_READ:
            map_write_vwr_to_path(wr, send_idx, target_idx, bare_ut_scratch, ut_ext_ptr_save,
                                  &slot_idx);
            return;
        case URMA_OPC_CAS:
            map_cas_vwr_to_path(wr, send_idx, target_idx, bare_ut_scratch, ut_ext_ptr_save,
                                &slot_idx);
            return;
        case URMA_OPC_FADD:
            map_fadd_vwr_to_path(wr, send_idx, target_idx, bare_ut_scratch, ut_ext_ptr_save,
                                 &slot_idx);
            return;
        default:
            return;
    }
}

void convert_jfs_pwr_to_vwr(urma_jfs_wr_t *wr, urma_target_jetty_t *vtjetty,
                            urma_user_tseg_t **ut_ext_ptr_save)
{
    uint32_t slot_idx = 0;
    switch (wr->opcode) {
        case URMA_OPC_SEND:
        case URMA_OPC_SEND_IMM:
        case URMA_OPC_SEND_INVALIDATE:
            restore_send_pwr_to_vwr(wr, vtjetty);
            return;
        case URMA_OPC_WRITE:
        case URMA_OPC_WRITE_IMM:
        case URMA_OPC_WRITE_NOTIFY:
        case URMA_OPC_READ:
            restore_write_pwr_to_vwr(wr, vtjetty, ut_ext_ptr_save, &slot_idx);
            return;
        case URMA_OPC_CAS:
            restore_cas_pwr_to_vwr(wr, vtjetty, ut_ext_ptr_save, &slot_idx);
            return;
        case URMA_OPC_FADD:
            restore_faa_pwr_to_vwr(wr, vtjetty, ut_ext_ptr_save, &slot_idx);
            return;
        default:
            return;
    }
}

void convert_jfr_vwr_to_pwr(urma_jfr_wr_t *wr, int recv_idx)
{
    for (int i = 0; i < wr->src.num_sge; ++i) {
        wr->src.sge[i].tseg = get_p_tseg(wr->src.sge[i].tseg, recv_idx, 0);
    }
}

void convert_pcr_to_vcr(urma_cr_t *cr, bondp_context_t *bdp_ctx, uint32_t *msn)
{
    bool msn_enable = bdp_ctx->msn_enable;

    if (is_recv_cr(cr)) {
        decode_imm_data(cr->imm_data, &cr->opcode, msn, &cr->remote_id.id, &cr->imm_data, msn_enable);

        urma_eid_t bonding_eid;
        if (bondp_topo_query_bonding_eid(&cr->remote_id.eid, &bonding_eid) == 0) {
            cr->remote_id.eid = bonding_eid;
        }
    } else {
        /*
         * NOTE: imm_data should only be valid for RECV CR.
         * However, for some reason, it is also valid for SEND CR.
         * This unexpected behavior is intentionally used to convert SEND CR.
         */
        decode_imm_data(cr->imm_data, &cr->opcode, msn, &cr->remote_id.id, &cr->imm_data, msn_enable);
    }
}
