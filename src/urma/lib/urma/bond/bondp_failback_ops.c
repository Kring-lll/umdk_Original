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

/*
 * 对单个 vtjetty 执行 reimport: unimport 旧 ptjetty + import 新 ptjetty
 */
static int bondp_failback_reimport_single(bondp_context_t *bdp_ctx,
    bondp_target_jetty_t *bdp_tjetty, const bondp_failback_msg_t *msg)
{
    uint32_t pjetty_idx = msg->in.pjetty_idx;
    uint32_t pjetty_new_id = msg->in.pjetty_new_id;

    for (uint32_t ptjetty_idx = 0; ptjetty_idx < bdp_tjetty->active_count; ++ptjetty_idx) {
        urma_target_jetty_t *old_ptjetty = bdp_tjetty->p_tjetty[pjetty_idx][ptjetty_idx];
        if (old_ptjetty == NULL) {
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

        if (urma_unimport_jetty(old_ptjetty) != URMA_SUCCESS) {
            URMA_LOG_ERR("Failback REQ: failed to unimport old ptjetty [%u][%u]\n",
                pjetty_idx, ptjetty_idx);
            return BONDP_FB_ERR;
        }
        bdp_tjetty->p_tjetty[pjetty_idx][ptjetty_idx] = NULL;

        urma_token_t *import_token = bdp_tjetty->import_token_valid ? &bdp_tjetty->import_token_value : NULL;
        urma_target_jetty_t *new_ptjetty = urma_import_jetty(bdp_ctx->p_ctxs[pjetty_idx], &rjetty, import_token);
        //TODO: 这里用到了CTX 查询一个urma_import_jetty 的参数  是否为healthcheck的残留  bondp_health_check.c:739
        //urma_context_t是否正确
        if (new_ptjetty == NULL) {
            URMA_LOG_ERR("Failback REQ: failed to import new ptjetty [%u][%u] new_id=%u\n",
                pjetty_idx, ptjetty_idx, pjetty_new_id);
            return BONDP_FB_ERR;
        }
        bdp_tjetty->p_tjetty[pjetty_idx][ptjetty_idx] = new_ptjetty;
    }
    return BONDP_FB_OK;
}

/*
 * 对端处理 REQ 消息
 *
 * 遍历 p_vjetty_id_table, 找到所有匹配远端 vjetty_id 的 comp,
 * 对每个 comp 执行 reimport, 最后发送一次 RESP
 */
int bondp_failback_handle_req(bondp_context_t *bdp_ctx, const bondp_failback_msg_t *msg)
{
    if (bdp_ctx == NULL || msg == NULL) {
        return BONDP_FB_ERR;
    }

    uint32_t vjetty_id = msg->in.vjetty_id;
    uint32_t pjetty_idx = msg->in.pjetty_idx;
    uint8_t req_seq = msg->in.req_seq;


    uint32_t node_id = msg->in.node_id;
    //TODO: 基于node_id 查找应当处理消息的node

    bondp_hash_table_t *tbl = &bdp_ctx->p_vjetty_id_table;
    //TODO: 寻找一个合理的数据结构，遍历一个node内的所有vtjetty，在此处设置其表头
    //hcv2 提供了 bondp_hcv2_tjetty_map 但是疑似是以vtjetty做为key 而非value   需要一个更合适的数据结构

    pthread_rwlock_rdlock(&tbl->lock);

    bondp_comp_t *seen_comps[URMA_UBAGG_DEV_MAX_NUM] = {NULL};
    int seen_count = 0;
    int reimport_count = 0;

    bdp_p_vjetty_id_t *entry = NULL;
    HMAP_FOR_EACH(entry, hmap_node, &tbl->hmap) {

        //bondp_comp_t *comp = entry->comp;
        if (comp == NULL || comp->comp_type != BONDP_COMP_JETTY) {
            continue;
        }
        bondp_target_jetty_t *bdp_tjetty = CONTAINER_OF_FIELD(entry, bondp_target_jetty_t, v_tjetty);
        //TODO: HMAP_FOR_EACH一个合理的数据结构 能够遍历特定node内的所有vtjetty

        if (bdp_tjetty->v_tjetty.id.id != vjetty_id) {
            continue;
        }

        int ret = bondp_failback_reimport_single(bdp_ctx, bdp_tjetty, msg);
        //实际reimport

        if (ret == BONDP_FB_OK) {
            reimport_count++;
        } else {
            URMA_LOG_WARN("Failback REQ: reimport failed for comp, vjetty_id=%u\n", vjetty_id);
        }
    }

    pthread_rwlock_unlock(&tbl->lock);


    URMA_LOG_INFO("Fallback REQ: reimport done, vjetty_id=%u reimported=%u\n",
        vjetty_id, reimport_count);

    bondp_failback_msg_t resp = {
        .in = {
            .vjetty_id = vjetty_id,
            .pjetty_idx = pjetty_idx,
            .pjetty_new_id = 0,
            .ctrl_type = BONDP_FAILBACK_CTRL_RESP,
            .req_seq = req_seq,
            .reserved = 0,
            .node_id = node_id
        }
    };
    //TODO: 实际的RESP发送
    int ret = bondp_failback_ctrl_send(&resp);
    if (ret != 0) {
        URMA_LOG_ERR("Fallback REQ: failed to send RESP, vjetty_id=%u ret=%d\n", vjetty_id, ret);
        return BONDP_FB_ERR;
    }

    if (reimport_count == 0) {
        URMA_LOG_INFO("Fallback REQ: no matching comp for vjetty_id=%u\n", vjetty_id);
        return BONDP_FB_ERR;
        //这里不一定要报错  也可能不需要统计reimport_count(reimport故障的vjetty数量)
    }

    URMA_LOG_INFO("Fallback RESP sent, vjetty_id=%u pjetty_idx=%u node_id=%u seq=%u\n",
        vjetty_id, pjetty_idx, node_id, req_seq);
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
