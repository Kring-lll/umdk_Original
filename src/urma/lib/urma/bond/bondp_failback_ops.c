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

int bondp_failback_rebuild_local_pjetty(bondp_failback_task_t *task)
{
    if (task == NULL || task->bondp_jetty == NULL ||
        task->pjetty_idx >= URMA_UBAGG_DEV_MAX_NUM) {
        return BONDP_FB_ERR;
    }

    int local_idx = (int)task->pjetty_idx;
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
    urma_jetty_t *new_jetty = urma_create_jetty(bdp_ctx->p_ctxs[local_idx], &p_cfg);
    //这部分cfg配置同healthcheck原版

    if (new_jetty == NULL) {
        URMA_LOG_ERR("Failed to create new pjetty at idx=%d\n", local_idx);
        return BONDP_FB_ERR;
    }

    new_jetty->remote_jetty = NULL;
    new_jetty->jetty_cfg.user_ctx = (uint64_t)bdp_jetty;

    task->new_pjetty = new_jetty;
    task->new_pjetty_id = new_jetty->jetty_id.id;

    URMA_LOG_INFO("New pjetty created for failback, idx=%d new_id=%u old_id=%u\n",
        local_idx, new_jetty->jetty_id.id, bdp_jetty->p_jetty[local_idx]->jetty_id.id);
    return BONDP_FB_OK;
}

/*
 * 向指定 NODE 发送 failback REQ 消息
 *
 * REQ 消息包含逻辑内容:
 *   vjetty_id        - 本端 vjetty ID
 *   pjetty_i_idx     - 故障 pjetty 本端 idx
 *   pjetty_i_new_id  - 新创建的 pjetty ID
 *   node_id          - 本端 NODE ID
 *
 * node_idx: NODE 在 node_states 数组中的索引
 */
int bondp_failback_send_req_to_node(bondp_failback_task_t *task, uint32_t node_idx)
{
    if (task == NULL || task->bdp_ctx == NULL || task->bondp_jetty == NULL) {
        return BONDP_FB_ERR;
    }

    if (task->new_pjetty_id == BONDP_FAILBACK_PRIMARY_INVALID_ID) {
        URMA_LOG_WARN("Cannot send fallback REQ, new_pjetty_id invalid, vjetty=%u\n", task->vjetty_id);
        return BONDP_FB_ERR;
    }

    if (node_idx >= task->node_count) {
        URMA_LOG_WARN("Cannot send fallback REQ, node_idx=%u >= node_count=%u\n", node_idx, task->node_count);
        return BONDP_FB_ERR;
    }

    // 构造 REQ 消息: (vjetty_id, pjetty_i_idx, pjetty_i_new_id, node_id)
    bondp_failback_msg_t req = {
        .in = {
            .vjetty_id = task->vjetty_id,
            .pjetty_i_idx = task->pjetty_idx,
            .pjetty_i_new_id = task->new_pjetty_id,
            .ctrl_type = BONDP_FAILBACK_CTRL_REQ,
            .req_seq = (uint8_t)task->req_seq,
            .reserved = 0,
            .node_id = task->bdp_ctx->node_id,  // 本端 NODE ID
        },
    };
    //TODO: 对接内核态消息发送接口
    int ret = bondp_failback_ctrl_send(&req);
    if (ret != 0) {
        return BONDP_FB_ERR;
    }

    URMA_LOG_INFO("Fallback REQ sent to node[%u], vjetty=%u pjetty_i_idx=%d new_pjetty_id=%u\n",
        node_idx, task->vjetty_id, task->pjetty_idx, task->new_pjetty_id);
    return BONDP_FB_OK;
}

//TODO: 遍历对端所有vtjetty的代码，是否会找到多个vtjetty  是否改为找到一个vjetty就重连一次，发一次RESP
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
 * 对端处理 REQ 消息
 *   vjetty_id        - 发送端 vjetty ID, 对端用于匹配 bdp_tjetty->v_tjetty.id.id
 *   pjetty_i_idx     - 故障 pjetty 本端 idx, 对端用于定位 reimport 路径
 *   pjetty_i_new_id  - 新创建的 pjetty ID, 对端用于 reimport
 *   ctrl_type        - BONDP_FAILBACK_CTRL_REQ / RESP
 *   req_seq          - 序列号
 *   node_id          - 发送端 NODE ID (RESP 中标识来源, 用于更新 notified 数组)
 */
int bondp_failback_handle_req(bondp_context_t *bdp_ctx, const bondp_failback_msg_t *msg)
{
    if (bdp_ctx == NULL || msg == NULL) {
        return BONDP_FB_ERR;
    }

    uint32_t vjetty_id = msg->in.vjetty_id;
    uint32_t pjetty_idx = msg->in.pjetty_idx;
    uint32_t pjetty_new_id = msg->in.pjetty_new_id;
    uint8_t req_seq = msg->in.req_seq;

    // 通过 vjetty_id 查找本地的 bondp_comp_t 和 bdp_tjetty
    bondp_target_jetty_t *bdp_tjetty = NULL;
    //bdp_jetty即为找到的和本端建立过链接的对端vtjetty
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
    for (uint32_t ptjetty_idx = 0; ptjetty_idx < bdp_tjetty->active_count; ++ptjetty_idx) {
        //TODO: 确认p_tjetty二维参数的前后关系，第一维是local_idx 这里的local是相对于什么说的
        if (1) {
            //是否需要一个全量链接的条件
            urma_target_jetty_t *old_ptjetty = bdp_tjetty->p_tjetty[pjetty_idx][ptjetty_idx];
            if (old_ptjetty == NULL) {
                //URMA_LOG_WARN("Fallback REQ: old ptjetty is NULL at [%d][%d], vjetty_id=%u\n", my_local_idx, my_target_idx, vjetty_id);
                //是否保持全量链接？  原实现中直接返回了-1
                continue;
            }
            urma_rjetty_t rjetty = {
                .jetty_id = old_ptjetty->id,
                .trans_mode = old_ptjetty->trans_mode,
                .policy = old_ptjetty->policy,
                .type = old_ptjetty->type,
                .flag = old_ptjetty->flag,
                .tp_type = old_ptjetty->tp_type,
            };
            rjetty.jetty_id.id = pjetty_new_id;
            //原为remote_primary_pjetty_id 应当替换为 新jetty_id  需要考虑与unimport的先后关系？
            if (urma_unimport_jetty(old_ptjetty) != URMA_SUCCESS) {
                URMA_LOG_ERR("Failback REQ: failed to unimport old ptjetty [%d][%d], vjetty_id=%u\n",
                    my_local_idx, my_target_idx, vjetty_id);
                return BONDP_FB_ERR;
            }
            bdp_tjetty->p_tjetty[pjetty_idx][ptjetty_idx] = NULL;

            urma_token_t *import_token = bdp_tjetty->import_token_valid ? &bdp_tjetty->import_token_value : NULL;
            urma_target_jetty_t *new_ptjetty = urma_import_jetty(bdp_ctx->p_ctxs[ptjetty_idx], &rjetty, import_token);
            if (new_ptjetty == NULL) {
                URMA_LOG_ERR("Failback REQ: failed to import new ptjetty [%d][%d], vjetty_id=%u new_id=%u\n",
                    pjetty_idx, ptjetty_idx, vjetty_id, pjetty_new_id);
                return BONDP_FB_ERR;
            }
            bdp_tjetty->p_tjetty[pjetty_idx][ptjetty_idx] == new_ptjetty;
            //TODO: 参考 bondp_health_check.c:717 前后逻辑编写 正确性待校验
        }
    }

    URMA_LOG_INFO("Fallback REQ: reimport done, vjetty_id=%u [%d][%d] old_id=? new_id=%u\n",
        vjetty_id, my_local_idx, my_target_idx, pjetty_new_id);

    bondp_failback_msg_t resp = {
        .in = {
            .vjetty_id = vjetty_id,
            .pjetty_i_idx = pjetty_idx,
            .pjetty_i_new_id = my_pjetty_id,
            .ctrl_type = BONDP_FAILBACK_CTRL_RESP,
            .req_seq = req_seq,
            .reserved = 0,
            .node_id = bdp_ctx->node_id,  // 本端 NODE ID, 用于 End A 更新 notified 数组
        }
    };
    //TODO: RESP的发送
    int ret = bondp_failback_ctrl_send(&resp);
    if (ret != 0) {
        URMA_LOG_ERR("Fallback REQ: failed to send RESP, vjetty_id=%u ret=%d\n", vjetty_id, ret);
        return BONDP_FB_ERR;
    }

    URMA_LOG_INFO("Fallback RESP sent, vjetty_id=%u pjetty_i_idx=%u my_pjetty_id=%u seq=%u\n",
        vjetty_id, pjetty_idx, my_pjetty_id, req_seq);
    return BONDP_FB_OK;
}



/*
 * 去重 → 标记 SATTLED → notified_count++
 */
int bondp_failback_handle_node_notify(bondp_failback_task_t *task, uint32_t node_id)
{

    if (task->node_states[node_id] == BONDP_FAILBACK_NODE_SATTLED) {
        URMA_LOG_DEBUG("Fallback RESP: node[%u] already settled, skip\n", node_id);
        return BONDP_FB_ERR;
    }

    task->node_states[node_id] = BONDP_FAILBACK_NODE_SATTLED;
    task->notified_count++;

    URMA_LOG_INFO("Fallback RESP accepted, node[%u] vjetty_id=%u notified=%u/%u\n",
        node_id, task->vjetty_id, task->notified_count, task->node_count);

    return BONDP_FB_OK;
}

void bondp_failback_destroy_old_jetty(bondp_failback_task_t *task)
{
    urma_jetty_t *old_jetty = task->bondp_jetty->p_jetty[task->pjetty_idx];
    urma_delete_jetty(old_jetty);
    return;
}
