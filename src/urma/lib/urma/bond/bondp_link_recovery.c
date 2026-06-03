/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bond provider link recovery implementation
 */

#include "bondp_link_recovery.h"

#include "bondp_failback.h"
#include "bondp_context_table.h"
#include "urma_log.h"
#include "urma_api.h"
#include "bondp_netlink.h"

static int bondp_get_target_idx_by_local_idx(const bondp_failback_ctx_t *task, int local_idx, int *target_idx)
{
    for (int i = 0; i < URMA_UBAGG_DEV_MAX_NUM; ++i) {
        for (int j = 0; j < URMA_UBAGG_DEV_MAX_NUM; ++j) {
            const bondp_failover_sub_task_t *sub = &task->sub_tasks[i][j];
            if (!sub->valid || sub->local_idx != local_idx) {
                continue;
            }
            *target_idx = sub->target_idx;
            return 0;
        }
    }
    return -1;
}

static int bondp_update_pjetty_id_mapping(
    bondp_context_t *bdp_ctx, urma_jetty_id_t old_id, urma_jetty_id_t new_id, bondp_comp_t *bdp_jetty)
{
    int ret = 0;
    pthread_rwlock_wrlock(&bdp_ctx->p_vjetty_id_table.lock);
    ret = bdp_p_vjetty_id_table_del_without_lock(&bdp_ctx->p_vjetty_id_table, old_id, JETTY);
    if (ret != 0) {
        pthread_rwlock_unlock(&bdp_ctx->p_vjetty_id_table.lock);
        URMA_LOG_ERR("Failed to delete stale pjetty id mapping: " URMA_JETTY_ID_FMT ", ret=%d\n",
            URMA_JETTY_ID_ARGS(&old_id), ret);
        return -1;
    }
    ret = bdp_p_vjetty_id_table_add_without_lock(
        &bdp_ctx->p_vjetty_id_table, new_id, JETTY, bdp_jetty->v_jetty.jetty_id.id, bdp_jetty);
    pthread_rwlock_unlock(&bdp_ctx->p_vjetty_id_table.lock);
    if (ret != 0) {
        URMA_LOG_ERR("Failed to add recreated pjetty id mapping: " URMA_JETTY_ID_FMT ", ret=%d\n",
            URMA_JETTY_ID_ARGS(&new_id), ret);
        return -1;
    }
    return 0;
}

//保留rebuild部分逻辑
int bondp_failback_rebuild_local_pjetty(bondp_failback_ctx_t *task)
{
    int local_idx = task->primary_local_idx;
    if (task == NULL || task->bondp_jetty == NULL ||
        local_idx < 0 || local_idx >= URMA_UBAGG_DEV_MAX_NUM) {
        return -1;
    }

    bondp_comp_t *bdp_jetty = task->bondp_jetty;
    bondp_context_t *bdp_ctx = bdp_jetty->bondp_ctx;
    if (bdp_jetty->p_jetty[local_idx] == NULL) {
        return -1;
    }

    int target_idx = -1;
    bondp_failover_sub_task_t *sub_task = NULL;
    if (bondp_get_target_idx_by_local_idx(task, local_idx, &target_idx) == 0 &&
        target_idx >= 0 && target_idx < URMA_UBAGG_DEV_MAX_NUM) {
        sub_task = &task->sub_tasks[local_idx][target_idx];
        sub_task->valid = false;
        sub_task->probe_pending = false;
    }

    urma_jetty_t *old_jetty = bdp_jetty->p_jetty[local_idx];

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

    urma_jetty_id_t old_id = old_jetty->jetty_id;
    /* Clear the slot before delete so concurrent readers never observe a freed jetty. */
    bdp_jetty->p_jetty[local_idx] = NULL;
    if (urma_delete_jetty(old_jetty) != URMA_SUCCESS) {
        bdp_jetty->p_jetty[local_idx] = old_jetty;
        URMA_LOG_ERR("Failed to delete pjetty at idx=%d\n", local_idx);
        return -1;
    }

    urma_jetty_t *new_jetty = urma_create_jetty(bdp_ctx->p_ctxs[local_idx], &p_cfg);
    if (new_jetty == NULL) {
        URMA_LOG_ERR("Failed to recreate pjetty at idx=%d\n", local_idx);
        return -1;
    }

    /* Recreated pjetty should not keep stale peer binding. */
    new_jetty->remote_jetty = NULL;
    new_jetty->jetty_cfg.user_ctx = (uint64_t)bdp_jetty;
    bdp_jetty->p_jetty[local_idx] = new_jetty;
    bdp_jetty->valid[local_idx] = false;

    if (bondp_update_pjetty_id_mapping(bdp_ctx, old_id, new_jetty->jetty_id, bdp_jetty) != 0) {
        return -1;
    }

    if (sub_task != NULL) {
        sub_task->valid = true;
        sub_task->probe_pending = false;
        atomic_store(&sub_task->link_ok, true);
    }
    URMA_LOG_INFO("link pjetty rebuilt, idx=%d old=" URMA_JETTY_ID_FMT " new=" URMA_JETTY_ID_FMT "\n",
        local_idx, URMA_JETTY_ID_ARGS(&old_id), URMA_JETTY_ID_ARGS(&new_jetty->jetty_id));
    return 0;
}

//搬移send_back 部分逻辑
#define BONDP_FALLBACK_CTRL_REQ                        1
#define BONDP_FALLBACK_CTRL_RESP                       2
#define BONDP_FALLBACK_PRIMARY_INVALID_ID              UINT32_MAX

static uint64_t bondp_fallback_build_req_payload(const bondp_failback_ctx_t *task)
{
    int primary_local_idx = task->primary_local_idx;
    if (primary_local_idx < 0 || primary_local_idx >= URMA_UBAGG_DEV_MAX_NUM) {
        return BONDP_FALLBACK_PRIMARY_INVALID_ID;
    }
    urma_jetty_t *primary_pjetty = task->bondp_jetty->p_jetty[primary_local_idx];
    if (primary_pjetty == NULL || primary_pjetty->remote_jetty == NULL) {
        return BONDP_FALLBACK_PRIMARY_INVALID_ID;
    }
    return (uint64_t)primary_pjetty->remote_jetty->id.id;
}

static int bondp_send_fallback_ctrl_msg(
    bondp_context_t *bdp_ctx, bondp_failback_ctx_t *task,
    int local_idx, int target_idx, uint8_t ctrl_type, uint8_t req_seq, uint64_t payload)
{
    if (local_idx < 0 || target_idx < 0 || payload > UINT32_MAX) {
        return -1;
    }

    int ret = bondp_fallback_ctrl_send_default(bdp_ctx, task->vjetty_id, local_idx, target_idx,
        ctrl_type, req_seq, (uint32_t)payload);
    if (ret != 0) {
        URMA_LOG_WARN("Failed to send fallback ctrl msg by netlink, vjetty=%u lidx=%d tidx=%d type=%u seq=%u ret=%d\n",
            task->vjetty_id, local_idx, target_idx, ctrl_type, req_seq, ret);
        return -1;
    }
    URMA_LOG_INFO("Fallback ctrl sent by netlink, vjetty=%u lidx=%d tidx=%d type=%u seq=%u payload=%u\n",
        task->vjetty_id, local_idx, target_idx, ctrl_type, req_seq, (uint32_t)payload);
    return 0;
}

static int bondp_failback_send_req(bondp_failback_ctx_t *task)
{
    bondp_context_t *bdp_ctx = task->bdp_ctx;
    int active_local_idx = task->active_local_idx;
    int active_target_idx = -1;
    if (bondp_get_target_idx_by_local_idx(task, active_local_idx, &active_target_idx) != 0) {
        return -1;
    }
    uint64_t req_payload = bondp_fallback_build_req_payload(task);
    if (bondp_send_fallback_ctrl_msg(bdp_ctx, task, active_local_idx, active_target_idx,
        BONDP_FALLBACK_CTRL_REQ, task->req_seq, req_payload) != 0) {
        return -1;
    }
    return 0;
}

//重新import的部分
//
static int bondp_failback_reimport(bondp_failback_ctx_t *task)
{
    bondp_comp_t *bdp_jetty = task->bondp_jetty;
    bondp_context_t *bdp_ctx = bdp_jetty->bondp_ctx;
    int local_idx = task->primary_local_idx;
    int target_idx = (int)task->fallback_task.primary_target_idx;
    if (local_idx < 0 || target_idx < 0 || local_idx >= URMA_UBAGG_DEV_MAX_NUM ||
        target_idx >= URMA_UBAGG_DEV_MAX_NUM) {
        return -1;
        }

    urma_target_jetty_t *old_tjetty = task->bdp_tjetty->p_tjetty[local_idx][target_idx];
    if (old_tjetty == NULL) {
        return -1;
    }

    urma_rjetty_t rjetty = {
        .jetty_id = old_tjetty->id,
        .trans_mode = old_tjetty->trans_mode,
        .policy = old_tjetty->policy,
        .type = old_tjetty->type,
        .flag = old_tjetty->flag,
        .tp_type = old_tjetty->tp_type,
    };
    rjetty.jetty_id.id = task->fallback_task.remote_primary_pjetty_id;

    if (urma_unimport_jetty(old_tjetty) != URMA_SUCCESS) {
        URMA_LOG_ERR("Failed to unimport old primary ptjetty, lidx=%d tidx=%d\n", local_idx, target_idx);
        return -1;
    }
    task->bdp_tjetty->p_tjetty[local_idx][target_idx] = NULL;

    urma_token_t *import_token = task->bdp_tjetty->import_token_valid ? &task->bdp_tjetty->import_token_value : NULL;
    urma_target_jetty_t *new_tjetty = urma_import_jetty(bdp_ctx->p_ctxs[local_idx], &rjetty, import_token);
    if (new_tjetty == NULL) {
        URMA_LOG_ERR("Failed to import recreated primary ptjetty, local_idx=%d target_idx=%d pjetty_id=%u\n",
            local_idx, target_idx, rjetty.jetty_id.id);
        return -1;
    }
    task->bdp_tjetty->p_tjetty[local_idx][target_idx] = new_tjetty;
    task->bondp_jetty->valid[local_idx] = false;
    return 0;
}

static int bondp_failback_relink(bondp_failback_ctx_t *task)
{
    if (bondp_failback_reimport(task) == 0) {
        task->state = BONDP_FAILBACK_STATE_DONE;
        for (uint32_t n = 0; n < task->bdp_tjetty->active_count; ++n) {
            uint32_t local_idx = task->bdp_tjetty->local_active_indices[n];
            uint32_t target_idx = task->bdp_tjetty->active_indices[n];
            if (local_idx >= URMA_UBAGG_DEV_MAX_NUM || target_idx >= URMA_UBAGG_DEV_MAX_NUM) {
                continue;
            }
            if (task->bondp_jetty->p_jetty[local_idx] == NULL ||
                task->bdp_tjetty->p_tjetty[local_idx][target_idx] == NULL ||
                task->bdp_tjetty->p_check_tseg[local_idx][target_idx] == NULL)
            {
                continue;
            }
            bondp_failover_sub_task_t *sub = &task->sub_tasks[local_idx][target_idx];
            //标记为可用
            sub->local_idx = (int)local_idx;
            sub->target_idx = (int)target_idx;
            sub->valid = true;
            sub->probe_pending = false;
            atomic_store(&sub->link_ok, true);
            //恢复健康状态下的task
        }
        URMA_LOG_INFO(
            "Fallback relink finished, health subtasks resumed, waiting health probe to validate primary idx=%d\n",
            task->primary_local_idx);
        //urma_ubagg_switch_inc();
        //考虑在这里添加计数器
        return 0;
    }
    return -1;
}

/*
static int bondp_failback_responder_rebuild(bondp_failover_ctx_t *task)
{
    uint32_t req_expected_primary_id = payload;
    uint32_t local_primary_id = BONDP_FAILBACK_PRIMARY_INVALID_ID;
    urma_jetty_t *local_primary_jetty = ctx->bondp_jetty->p_jetty[ctx->primary_local_idx];
    if (local_primary_jetty != NULL) {
        local_primary_id = local_primary_jetty->jetty_id.id;
    }
    bool need_rebuild = (local_primary_id == BONDP_FAILBACK_PRIMARY_INVALID_ID || req_expected_primary_id != local_primary_id);
    //判断本端要不要尝试重建本地primary_pjetty  本地不存在 或者 对端期望的和本地的不一致
    if (need_rebuild && !task->fallback_task.local_rebuilt && !task->fallback_task.relink_done) {
        //TODO:此处有维护状态标记，用于避免重复localrebuild
        //要重建 且还没重建好 且还没整体重建好
        if (bondp_failback_rebuild_local_pjetty(task) != 0) {
            URMA_LOG_WARN("Fallback REQ rebuild failed, skip RESP for this round, seq=%u\n", req_seq);
            pthread_rwlock_unlock(&health->task_table.lock);
            return true;
        }
        task->fallback_task.local_rebuilt = true;
        // if (task->active_local_idx == task->primary_local_idx) {
        //     //当前如果是主则要切到备
        //     int backup_local_idx = -1;
        //     if (bondp_select_backup_local_idx(task, &backup_local_idx) == 0) {
        //         task->active_local_idx = backup_local_idx;
        //         URMA_LOG_INFO(
        //             "Fallback REQ rebuild primary active link, switch active idx to backup, old=%d new=%d\n",
        //             task->primary_local_idx, backup_local_idx);
        //     } else {
        //         URMA_LOG_WARN(
        //             "Fallback REQ rebuild primary active link, but no backup route available, keep active idx=%d\n",
        //             task->active_local_idx);
        //     }
        // }
        local_primary_jetty = task->bondp_jetty->p_jetty[task->primary_local_idx];
        local_primary_id = (local_primary_jetty == NULL) ?
            BONDP_FALLBACK_PRIMARY_INVALID_ID : local_primary_jetty->jetty_id.id;
    } else if (!need_rebuild) {
        URMA_LOG_INFO("Fallback REQ matched local primary pjetty id=%u, skip rebuild\n", local_primary_id);
    }
    return 0;
}
*/

