/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bond provider link recovery implementation
 */

#include "bondp_failback_ops.h"

#include "bondp_context_table.h"
#include "bondp_failback.h"
#include "bondp_netlink.h"
#include "urma_api.h"
#include "urma_log.h"

static int bondp_update_pjetty_id_mapping(
    bondp_context_t *bdp_ctx, urma_jetty_id_t old_id, urma_jetty_id_t new_id, bondp_comp_t *bdp_jetty)
{
    int ret = BONDP_FB_OK;
    pthread_rwlock_wrlock(&bdp_ctx->p_vjetty_id_table.lock);
    ret = bdp_p_vjetty_id_table_del_without_lock(&bdp_ctx->p_vjetty_id_table, old_id, JETTY);
    if (ret != 0) {
        pthread_rwlock_unlock(&bdp_ctx->p_vjetty_id_table.lock);
        URMA_LOG_ERR("Failed to delete stale pjetty id mapping: " URMA_JETTY_ID_FMT ", ret=%d\n",
            URMA_JETTY_ID_ARGS(&old_id), ret);
        return BONDP_FB_ERR;
    }
    ret = bdp_p_vjetty_id_table_add_without_lock(
        &bdp_ctx->p_vjetty_id_table, new_id, JETTY, bdp_jetty->v_jetty.jetty_id.id, bdp_jetty);
    pthread_rwlock_unlock(&bdp_ctx->p_vjetty_id_table.lock);
    if (ret != 0) {
        URMA_LOG_ERR("Failed to add recreated pjetty id mapping: " URMA_JETTY_ID_FMT ", ret=%d\n",
            URMA_JETTY_ID_ARGS(&new_id), ret);
        return BONDP_FB_ERR;
    }
    return BONDP_FB_OK;
}

//保留rebuild部分逻辑
int bondp_failback_rebuild_local_pjetty(bondp_failback_task_t *task)
{
    int local_idx = task->failed_local_idx;
    if (task == NULL || task->bondp_jetty == NULL ||
        local_idx < 0 || local_idx >= URMA_UBAGG_DEV_MAX_NUM) {
        return BONDP_FB_ERR;
    }

    bondp_comp_t *bdp_jetty = task->bondp_jetty;
    bondp_context_t *bdp_ctx = bdp_jetty->bondp_ctx;
    if (bdp_jetty->p_jetty[local_idx] == NULL) {
        return BONDP_FB_ERR;
    }

    urma_jetty_cfg_t p_cfg = bdp_jetty->v_jetty.jetty_cfg;
    bondp_jfc_t *bdp_jfs_jfc = CONTAINER_OF_FIELD(p_cfg.jfs_cfg.jfc, bondp_jfc_t, v_jfc);
    bondp_comp_t *bdp_jfr = CONTAINER_OF_FIELD(p_cfg.shared.jfr, bondp_comp_t, base);
    bondp_jfc_t *bdp_rplc_jfc = NULL;
    if (p_cfg.shared.jfc != NULL) {
        bdp_rplc_jfc = CONTAINER_OF_FIELD(p_cfg.shared.jfc, bondp_jfc_t, v_jfc);
    }
    p_cfg.jfs_cfg.jfc = bdp_jfs_jfc->p_jfc[local_idx];
    p_cfg.shared.jfr = bdp_jfr->p_jfr[local_idx];
    if (bdp_rplc_jfc != NULL) {
        p_cfg.shared.jfc = bdp_rplc_jfc->p_jfc[local_idx];
    }
    //此处是原本是删除旧jetty的逻辑
    /*
    *urma_jetty_id_t old_id = old_jetty->jetty_id;
    bdp_jetty->p_jetty[local_idx] = NULL;
    if (urma_delete_jetty(old_jetty) != URMA_SUCCESS) {
    bdp_jetty->p_jetty[local_idx] = old_jetty;
    URMA_LOG_ERR("Failed to delete pjetty at idx=%d\n", local_idx);
    return BONDP_FB_ERR;
    }
    */
    urma_jetty_t *new_jetty = urma_create_jetty(bdp_ctx->p_ctxs[local_idx], &p_cfg);
    //这部分cfg配置同healthcheck原版

    if (new_jetty == NULL) {
        URMA_LOG_ERR("Failed to create new pjetty at idx=%d\n", local_idx);
        return BONDP_FB_ERR;
    }

    new_jetty->remote_jetty = NULL;
    new_jetty->jetty_cfg.user_ctx = (uint64_t)bdp_jetty;
    //一并移动到handleRESP
    /*
    bdp_jetty->p_jetty[local_idx] = new_jetty;
    bdp_jetty->valid[local_idx] = false;
    if (bondp_update_pjetty_id_mapping(bdp_ctx, old_id, new_jetty->jetty_id, bdp_jetty) != BONDP_FB_OK) {
        return BONDP_FB_ERR;
    }
    */

    task->new_pjetty = new_jetty;
    task->new_pjetty_id = new_jetty->jetty_id.id;

    URMA_LOG_INFO("New pjetty created for failback, idx=%d new_id=%u old_id=%u\n",
        local_idx, new_jetty->jetty_id.id, bdp_jetty->p_jetty[local_idx]->jetty_id.id);
    return BONDP_FB_OK;
}

static int bondp_send_fallback_ctrl_msg(
    bondp_context_t *bdp_ctx, bondp_failback_task_t *task,
    int local_idx, int target_idx, uint8_t ctrl_type, uint8_t req_seq, uint64_t payload)
//TODO: sendmessage 具体修改
{
    if (local_idx < 0 || target_idx < 0 || payload > UINT32_MAX) {
        return BONDP_FB_ERR;
    }

    int ret = bondp_fallback_ctrl_send_default(bdp_ctx, task->vjetty_id, local_idx, target_idx,
        ctrl_type, req_seq, (uint32_t)payload);
    if (ret != 0) {
        URMA_LOG_WARN("Failed to send fallback ctrl msg by netlink, vjetty=%u lidx=%d tidx=%d type=%u seq=%u ret=%d\n",
            task->vjetty_id, local_idx, target_idx, ctrl_type, req_seq, ret);
        return BONDP_FB_ERR;
    }
    URMA_LOG_INFO("Fallback ctrl sent by netlink, vjetty=%u lidx=%d tidx=%d type=%u seq=%u payload=%u\n",
        task->vjetty_id, local_idx, target_idx, ctrl_type, req_seq, (uint32_t)payload);
    return BONDP_FB_OK;
}

int bondp_failback_send_req(bondp_failback_task_t *task)
{
    //TODO REQ要发给谁？
    //原failed jetty的 tjetty吗
    if (task == NULL || task->bdp_ctx == NULL || task->bondp_jetty == NULL || task->bdp_tjetty == NULL) {
        return BONDP_FB_ERR;
    }

    if (task->new_pjetty_id == 0 || task->new_pjetty_id == BONDP_FAILBACK_PRIMARY_INVALID_ID) {
        URMA_LOG_WARN("Cannot send fallback REQ, new_pjetty_id invalid, vjetty=%u\n", task->vjetty_id);
        return BONDP_FB_ERR;
    }

    bondp_context_t *bdp_ctx = task->bdp_ctx;
    int pjetty_old_idx = task->failed_local_idx;
    if (bondp_get_target_idx_by_local_idx(task, active_local_idx, &active_target_idx) != BONDP_FB_OK) {
        return BONDP_FB_ERR;
    }

    if (bondp_send_fallback_ctrl_msg(bdp_ctx, task, send_local_idx, send_target_idx,
        BONDP_FAILBACK_CTRL_REQ, (uint8_t)task->req_seq, task->new_pjetty_id) != BONDP_FB_OK) {
        return BONDP_FB_ERR;
    }

    URMA_LOG_INFO("Fallback REQ sent, vjetty=%u failed_idx=%d new_pjetty_id=%u via lidx=%d tidx=%d\n",
        task->vjetty_id, task->failed_local_idx, task->new_pjetty_id, send_local_idx, send_target_idx);
    return BONDP_FB_OK;
}

int bondp_failback_reimport(bondp_failback_task_t *task)
{
    bondp_comp_t *bdp_jetty = task->bondp_jetty;
    bondp_context_t *bdp_ctx = bdp_jetty->bondp_ctx;
    int local_idx = task->failed_local_idx;
    int target_idx = task->target_idx;
    if (local_idx < 0 || target_idx < 0 || local_idx >= URMA_UBAGG_DEV_MAX_NUM ||
        target_idx >= URMA_UBAGG_DEV_MAX_NUM) {
        return BONDP_FB_ERR;
    }

    urma_target_jetty_t *old_tjetty = task->bdp_tjetty->p_tjetty[local_idx][target_idx];
    if (old_tjetty == NULL) {
        return BONDP_FB_ERR;
    }

    urma_rjetty_t rjetty = {
        .jetty_id = old_tjetty->id,
        .trans_mode = old_tjetty->trans_mode,
        .policy = old_tjetty->policy,
        .type = old_tjetty->type,
        .flag = old_tjetty->flag,
        .tp_type = old_tjetty->tp_type,
    };
    rjetty.jetty_id.id = task->remote_new_pjetty_id;

    if (urma_unimport_jetty(old_tjetty) != URMA_SUCCESS) {
        URMA_LOG_ERR("Failed to unimport old primary ptjetty, lidx=%d tidx=%d\n", local_idx, target_idx);
        return BONDP_FB_ERR;
    }
    task->bdp_tjetty->p_tjetty[local_idx][target_idx] = NULL;

    urma_token_t *import_token = task->bdp_tjetty->import_token_valid ? &task->bdp_tjetty->import_token_value : NULL;
    urma_target_jetty_t *new_tjetty = urma_import_jetty(bdp_ctx->p_ctxs[local_idx], &rjetty, import_token);
    if (new_tjetty == NULL) {
        URMA_LOG_ERR("Failed to import recreated primary ptjetty, local_idx=%d target_idx=%d pjetty_id=%u\n",
            local_idx, target_idx, rjetty.jetty_id.id);
        return BONDP_FB_ERR;
    }
    task->bdp_tjetty->p_tjetty[local_idx][target_idx] = new_tjetty;
    task->bondp_jetty->valid[local_idx] = false;
    return BONDP_FB_OK;
}

int bondp_failback_relink(bondp_failback_task_t *task)
{
    bondp_comp_t *bdp_jetty = task->bondp_jetty;
    int local_idx = task->failed_local_idx;

    urma_jetty_t *old_pjetty = bdp_jetty->p_jetty[local_idx];
    urma_jetty_t *new_pjetty = task->new_pjetty;
    if (new_pjetty == NULL) {
        URMA_LOG_ERR("Cannot relink, new_pjetty is NULL, idx=%d\n", local_idx);
        return BONDP_FB_ERR;
    }

    if (bondp_failback_reimport(task) != BONDP_FB_OK) {
        return BONDP_FB_ERR;
    }

    urma_jetty_id_t old_id = old_pjetty->jetty_id;
    if (urma_delete_jetty(old_pjetty) != URMA_SUCCESS) {
        URMA_LOG_ERR("Failed to delete old pjetty at idx=%d\n", local_idx);
        return BONDP_FB_ERR;
    }

    bdp_jetty->p_jetty[local_idx] = new_pjetty;
    bdp_jetty->valid[local_idx] = false;
    task->new_pjetty = NULL;

    if (bondp_update_pjetty_id_mapping(task->bdp_ctx, old_id, new_pjetty->jetty_id, bdp_jetty) != BONDP_FB_OK) {
        URMA_LOG_WARN("Failed to update pjetty id mapping after relink, idx=%d\n", local_idx);
    }

    task->state = BONDP_FAILBACK_STATE_DONE;
    URMA_LOG_INFO("Fallback relink finished, idx=%d old_id=%u new_id=%u\n",
        local_idx, old_id.id, new_pjetty->jetty_id.id);
    return BONDP_FB_OK;
}
