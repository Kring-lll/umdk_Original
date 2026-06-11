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

/*
 * 发送 failback REQ 消息
 *
 * REQ 消息包含三项逻辑内容, 不指定对端:
 *   vjetty_id        - 本端 vjetty ID
 *   pjetty_i_idx     - 故障 pjetty 本端 idx
 *   pjetty_i_new_id  - 新创建的 pjetty ID
 *
 * 路由: 自动选择一条非故障的活跃路径发送
 */
int bondp_failback_send_req(bondp_failback_task_t *task)
{
    if (task == NULL || task->bdp_ctx == NULL || task->bondp_jetty == NULL || task->bdp_tjetty == NULL) {
        return BONDP_FB_ERR;
    }

    // 校验 new_pjetty_id 有效性
    if (task->new_pjetty_id == 0 || task->new_pjetty_id == BONDP_FAILBACK_PRIMARY_INVALID_ID) {
        URMA_LOG_WARN("Cannot send fallback REQ, new_pjetty_id invalid, vjetty=%u\n", task->vjetty_id);
        return BONDP_FB_ERR;
    }

    bondp_comp_t *bdp_jetty = task->bondp_jetty;
    bondp_target_jetty_t *bdp_tjetty = task->bdp_tjetty;

    // 构造 REQ 消息: (vjetty_id, pjetty_i_idx, pjetty_i_new_id)
    bondp_failback_msg_t req = {
        .in = {
            .vjetty_id = task->vjetty_id,
            .pjetty_i_idx = (uint32_t)task->failed_local_idx,
            .pjetty_i_new_id = task->new_pjetty_id,
            .ctrl_type = BONDP_FAILBACK_CTRL_REQ,
            .req_seq = (uint8_t)task->req_seq,
            .reserved = 0
        },
    };
    //TODO 网络部分需要继续设计
    int ret = bondp_failback_ctrl_send(&req);
    if (ret != 0) {
        return BONDP_FB_ERR;
    }

    URMA_LOG_INFO("Fallback REQ sent, vjetty=%u pjetty_i_idx=%d new_pjetty_id=%u via route_lidx=%d route_tidx=%d\n",
        task->vjetty_id, task->failed_local_idx, task->new_pjetty_id, route_local_idx, route_target_idx);
    return BONDP_FB_OK;
}

// reimport: 独立的 unimport + import 操作, 当前 relink 已内联此逻辑
// 保留作为独立工具函数, 可供其他场景使用
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
    bondp_context_t *bdp_ctx = bdp_jetty->bondp_ctx;
    int local_idx = task->failed_local_idx;
    int target_idx = task->target_idx;

    urma_jetty_t *old_pjetty = bdp_jetty->p_jetty[local_idx];
    urma_jetty_t *new_pjetty = task->new_pjetty;
    if (new_pjetty == NULL) {
        URMA_LOG_ERR("Cannot relink, new_pjetty is NULL, idx=%d\n", local_idx);
        return BONDP_FB_ERR;
    }
    if (local_idx < 0 || target_idx < 0 || local_idx >= URMA_UBAGG_DEV_MAX_NUM ||
        target_idx >= URMA_UBAGG_DEV_MAX_NUM) {
        URMA_LOG_ERR("Cannot relink, invalid idx local=%d target=%d\n", local_idx, target_idx);
        return BONDP_FB_ERR;
    }

    // Step 1: unimport 旧 tjetty (旧 pjetty 对应的 target jetty)
    urma_target_jetty_t *old_tjetty = task->bdp_tjetty->p_tjetty[local_idx][target_idx];
    if (old_tjetty != NULL) {
        if (urma_unimport_jetty(old_tjetty) != URMA_SUCCESS) {
            URMA_LOG_ERR("Failed to unimport old ptjetty, lidx=%d tidx=%d\n", local_idx, target_idx);
            return BONDP_FB_ERR;
        }
        task->bdp_tjetty->p_tjetty[local_idx][target_idx] = NULL;
    }

    // Step 2: 删除旧 pjetty
    urma_jetty_id_t old_id = old_pjetty->jetty_id;
    if (urma_delete_jetty(old_pjetty) != URMA_SUCCESS) {
        URMA_LOG_ERR("Failed to delete old pjetty at idx=%d\n", local_idx);
        return BONDP_FB_ERR;
    }

    // Step 3: 将 new_pjetty 放入 p_jetty[local_idx], 新 jetty 上位
    bdp_jetty->p_jetty[local_idx] = new_pjetty;
    bdp_jetty->valid[local_idx] = false;
    task->new_pjetty = NULL;

    // Step 4: 用 remote_new_pjetty_id import 新 tjetty, 绑定到新 pjetty
    // 构造 rjetty: 保留旧 tjetty 的属性, 替换 jetty_id 为对端新 pjetty ID
    urma_rjetty_t rjetty = {0};
    if (old_tjetty != NULL) {
        rjetty.jetty_id = old_tjetty->id;
        rjetty.trans_mode = old_tjetty->trans_mode;
        rjetty.policy = old_tjetty->policy;
        rjetty.type = old_tjetty->type;
        rjetty.flag = old_tjetty->flag;
        rjetty.tp_type = old_tjetty->tp_type;
    }
    rjetty.jetty_id.id = task->remote_new_pjetty_id;

    urma_token_t *import_token = task->bdp_tjetty->import_token_valid ?
        &task->bdp_tjetty->import_token_value : NULL;
    urma_target_jetty_t *new_tjetty = urma_import_jetty(bdp_ctx->p_ctxs[local_idx], &rjetty, import_token);
    if (new_tjetty == NULL) {
        URMA_LOG_ERR("Failed to import new ptjetty onto new pjetty, local_idx=%d target_idx=%u remote_id=%u\n",
            local_idx, target_idx, task->remote_new_pjetty_id);
        return BONDP_FB_ERR;
    }
    task->bdp_tjetty->p_tjetty[local_idx][target_idx] = new_tjetty;

    // Step 5: 更新 pjetty ID 映射
    if (bondp_update_pjetty_id_mapping(bdp_ctx, old_id, new_pjetty->jetty_id, bdp_jetty) != BONDP_FB_OK) {
        URMA_LOG_WARN("Failed to update pjetty id mapping after relink, idx=%d\n", local_idx);
    }

    task->state = BONDP_FAILBACK_STATE_DONE;
    URMA_LOG_INFO("Fallback relink finished, idx=%d old_id=%u new_id=%u remote_new_id=%u\n",
        local_idx, old_id.id, new_pjetty->jetty_id.id, task->remote_new_pjetty_id);
    return BONDP_FB_OK;
}

//TODO: 遍历对端所有vjetty的代码，找到存在的vjetty 是否会找到多个vjetty
/*
 * 通过远端 vjetty_id 查找本地的 bondp_comp_t 和 bdp_tjetty
 *
 * 遍历 p_vjetty_id_table 中的所有 comp, 检查 comp->v_jetty.remote_jetty
 * 找到 bdp_tjetty->v_tjetty.id.id == remote_vjetty_id 的匹配项
 *
 * 对端收到 REQ 后, REQ.vjetty_id 是发送端的 vjetty ID,
 * 本端通过 import 持有 bdp_tjetty, 其 v_tjetty.id.id 即为远端 vjetty ID
 */


static bondp_comp_t *bondp_find_comp_by_remote_vjetty_id(
    bondp_context_t *bdp_ctx, uint32_t remote_vjetty_id,
    bondp_target_jetty_t **out_bdp_tjetty)
{
    bondp_hash_table_t *tbl = &bdp_ctx->p_vjetty_id_table;
    pthread_rwlock_rdlock(&tbl->lock);

    bondp_comp_t *found_comp = NULL;
    bondp_target_jetty_t *found_tjetty = NULL;
    // 去重: 同一个 comp 可能在 table 中有多条 pjetty 记录
    bondp_comp_t *seen_comps[URMA_UBAGG_DEV_MAX_NUM] = {NULL};
    int seen_count = 0;

    bdp_p_vjetty_id_t *entry = NULL;
    HMAP_FOR_EACH(entry, hmap_node, &tbl->hmap) {
        bondp_comp_t *comp = entry->comp;
        if (comp == NULL || comp->comp_type != BONDP_COMP_JETTY) {
            continue;
        }
        // 去重: 跳过已检查过的 comp
        bool already_seen = false;
        for (int i = 0; i < seen_count; ++i) {
            if (seen_comps[i] == comp) {
                already_seen = true;
                break;
            }
        }
        if (already_seen) {
            continue;
        }
        if (seen_count < URMA_UBAGG_DEV_MAX_NUM) {
            seen_comps[seen_count++] = comp;
        }

        // 检查 comp 是否绑定了 remote_jetty, 且 v_tjetty.id.id 匹配
        if (comp->v_jetty.remote_jetty == NULL) {
            continue;
        }
        bondp_target_jetty_t *bdp_tjetty = CONTAINER_OF_FIELD(
            comp->v_jetty.remote_jetty, bondp_target_jetty_t, v_tjetty);
        if (bdp_tjetty->v_tjetty.id.id == remote_vjetty_id) {
            found_comp = comp;
            found_tjetty = bdp_tjetty;
            break;
        }
    }

    pthread_rwlock_unlock(&tbl->lock);

    if (out_bdp_tjetty != NULL) {
        *out_bdp_tjetty = found_tjetty;
    }
    return found_comp;
}

/*
 * 通过 vjetty_id 查找本端的 bondp_comp_t
 *
 * 遍历 p_vjetty_id_table, 找到 comp_type == BONDP_COMP_JETTY 且
 * comp->v_jetty.jetty_id.id == vjetty_id 的 bondp_comp_t
 *
 * 用于 RESP 到达时找回对应的 failback task
 */
bondp_comp_t *bondp_find_comp_by_vjetty_id(bondp_context_t *bdp_ctx, uint32_t vjetty_id)
{
    bondp_hash_table_t *tbl = &bdp_ctx->p_vjetty_id_table;
    pthread_rwlock_rdlock(&tbl->lock);

    bondp_comp_t *found_comp = NULL;
    // 去重: 同一个 comp 可能在 table 中有多条 pjetty 记录
    bondp_comp_t *seen_comps[URMA_UBAGG_DEV_MAX_NUM] = {NULL};
    int seen_count = 0;

    bdp_p_vjetty_id_t *entry = NULL;
    HMAP_FOR_EACH(entry, hmap_node, &tbl->hmap) {
        bondp_comp_t *comp = entry->comp;
        if (comp == NULL || comp->comp_type != BONDP_COMP_JETTY) {
            continue;
        }
        // 去重: 跳过已检查过的 comp
        bool already_seen = false;
        for (int i = 0; i < seen_count; ++i) {
            if (seen_comps[i] == comp) {
                already_seen = true;
                break;
            }
        }
        if (already_seen) {
            continue;
        }
        if (seen_count < URMA_UBAGG_DEV_MAX_NUM) {
            seen_comps[seen_count++] = comp;
        }

        // 匹配本端 vjetty_id
        if (comp->v_jetty.jetty_id.id == vjetty_id) {
            found_comp = comp;
            break;
        }
    }

    pthread_rwlock_unlock(&tbl->lock);
    return found_comp;
}



/*
 * 对端处理 REQ 消息
 *
 * 流程:
 *   1. vjetty_id → 遍历本地 bdp_tjetty, 匹配 v_tjetty.id.id
 *   2. pjetty_i_idx → 匹配 bdp_tjetty->active_indices[n], 得到 local_active_indices[n]
 *   3. unimport 旧 p_tjetty[my_local][my_target]
 *   4. 用 pjetty_i_new_id 重新 import
 *   5. 发送 RESP(vjetty_id, pjetty_i_idx, my_pjetty_id)
 */
int bondp_failback_handle_req(bondp_context_t *bdp_ctx, const bondp_failback_msg_t *msg)
{
    if (bdp_ctx == NULL || msg == NULL) {
        return BONDP_FB_ERR;
    }

    uint32_t vjetty_id = msg->in.vjetty_id;
    uint32_t pjetty_i_idx = msg->in.pjetty_i_idx;
    uint32_t pjetty_i_new_id = msg->in.pjetty_i_new_id;
    uint8_t req_seq = msg->in.req_seq;

    // 通过 vjetty_id 查找本地的 bondp_comp_t 和 bdp_tjetty
    bondp_target_jetty_t *bdp_tjetty = NULL;
    bondp_comp_t *bdp_jetty = bondp_find_comp_by_remote_vjetty_id(bdp_ctx, vjetty_id, &bdp_tjetty);
    if (bdp_jetty == NULL || bdp_tjetty == NULL) {
        URMA_LOG_WARN("Fallback REQ: no matching bdp_tjetty for vjetty_id=%u\n", vjetty_id);
        return BONDP_FB_ERR;
    }

    // 通过 pjetty_i_idx 匹配 active_indices[n], 定位本端路径
    // active_indices[n] = 对端 (发送端) 的 local_idx
    // local_active_indices[n] = 本端的 local_idx
    int my_local_idx = -1;
    int my_target_idx = -1;
    for (uint32_t n = 0; n < bdp_tjetty->active_count; ++n) {
        if (bdp_tjetty->active_indices[n] == pjetty_i_idx) {
            my_local_idx = (int)bdp_tjetty->local_active_indices[n];
            my_target_idx = (int)bdp_tjetty->active_indices[n];
            break;
        }
    }

    if (my_local_idx < 0 || my_target_idx < 0 ||
        my_local_idx >= URMA_UBAGG_DEV_MAX_NUM || my_target_idx >= URMA_UBAGG_DEV_MAX_NUM) {
        URMA_LOG_WARN("Fallback REQ: no matching path for pjetty_i_idx=%u in bdp_tjetty vjetty_id=%u\n",
            pjetty_i_idx, vjetty_id);
        return BONDP_FB_ERR;
    }

    // unimport 旧 ptjetty
    urma_target_jetty_t *old_ptjetty = bdp_tjetty->p_tjetty[my_local_idx][my_target_idx];
    if (old_ptjetty == NULL) {
        URMA_LOG_WARN("Fallback REQ: old ptjetty is NULL at [%d][%d], vjetty_id=%u\n",
            my_local_idx, my_target_idx, vjetty_id);
        return BONDP_FB_ERR;
    }

    // 保存旧 ptjetty 的属性, 用于 reimport
    urma_rjetty_t rjetty = {
        .jetty_id = old_ptjetty->id,
        .trans_mode = old_ptjetty->trans_mode,
        .policy = old_ptjetty->policy,
        .type = old_ptjetty->type,
        .flag = old_ptjetty->flag,
        .tp_type = old_ptjetty->tp_type,
    };
    // 用发送端的新 pjetty ID 替换
    rjetty.jetty_id.id = pjetty_i_new_id;

    if (urma_unimport_jetty(old_ptjetty) != URMA_SUCCESS) {
        URMA_LOG_ERR("Fallback REQ: failed to unimport old ptjetty [%d][%d], vjetty_id=%u\n",
            my_local_idx, my_target_idx, vjetty_id);
        return BONDP_FB_ERR;
    }
    bdp_tjetty->p_tjetty[my_local_idx][my_target_idx] = NULL;

    //用 pjetty_i_new_id 重新 import
    urma_token_t *import_token = bdp_tjetty->import_token_valid ? &bdp_tjetty->import_token_value : NULL;
    urma_target_jetty_t *new_ptjetty = urma_import_jetty(bdp_ctx->p_ctxs[my_local_idx], &rjetty, import_token);
    if (new_ptjetty == NULL) {
        URMA_LOG_ERR("Fallback REQ: failed to import new ptjetty [%d][%d], vjetty_id=%u new_id=%u\n",
            my_local_idx, my_target_idx, vjetty_id, pjetty_i_new_id);
        return BONDP_FB_ERR;
    }
    bdp_tjetty->p_tjetty[my_local_idx][my_target_idx] = new_ptjetty;
    bdp_tjetty->valid[my_target_idx] = true;

    URMA_LOG_INFO("Fallback REQ: reimport done, vjetty_id=%u [%d][%d] old_id=? new_id=%u\n",
        vjetty_id, my_local_idx, my_target_idx, pjetty_i_new_id);

    // 发送 RESP(vjetty_id, pjetty_i_idx, my_pjetty_id)
    // RESP 的 pjetty_i_new_id 填本端对应路径的 pjetty ID
    uint32_t my_pjetty_id = 0;
    if (bdp_jetty->p_jetty[my_local_idx] != NULL) {
        my_pjetty_id = bdp_jetty->p_jetty[my_local_idx]->jetty_id.id;
    }

    // 选择一条活跃路径发送 RESP (优先使用当前路径)
    int resp_route_local_idx = my_local_idx;
    int resp_route_target_idx = my_target_idx;
    // 如果当前路径不可用, 选择任意活跃路径 模仿原healthcheck的选路
    if (bdp_jetty->p_jetty[resp_route_local_idx] == NULL) {
        resp_route_local_idx = -1;
        resp_route_target_idx = -1;
        for (uint32_t n = 0; n < bdp_tjetty->active_count; ++n) {
            uint32_t lidx = bdp_tjetty->local_active_indices[n];
            if (lidx >= URMA_UBAGG_DEV_MAX_NUM || bdp_jetty->p_jetty[lidx] == NULL) {
                continue;
            }
            resp_route_local_idx = (int)lidx;
            resp_route_target_idx = (int)bdp_tjetty->active_indices[n];
            break;
        }
    }

    if (resp_route_local_idx < 0 || resp_route_target_idx < 0) {
        URMA_LOG_ERR("Fallback REQ: no active path to send RESP, vjetty_id=%u\n", vjetty_id);
        return BONDP_FB_ERR;
    }

    bondp_failback_msg_t resp = {
        .in = {
            .vjetty_id = vjetty_id,
            .pjetty_i_idx = pjetty_i_idx,
            .pjetty_i_new_id = my_pjetty_id,
            .ctrl_type = BONDP_FAILBACK_CTRL_RESP,
            .req_seq = req_seq,
            .reserved = 0,
        },
    };
    //TODO: RESP的发送模式
    int ret = bondp_failback_ctrl_send(&resp);
    if (ret != 0) {
        URMA_LOG_ERR("Fallback REQ: failed to send RESP, vjetty_id=%u ret=%d\n", vjetty_id, ret);
        return BONDP_FB_ERR;
    }

    URMA_LOG_INFO("Fallback RESP sent, vjetty_id=%u pjetty_i_idx=%u my_pjetty_id=%u seq=%u\n",
        vjetty_id, pjetty_i_idx, my_pjetty_id, req_seq);
    return BONDP_FB_OK;
}
