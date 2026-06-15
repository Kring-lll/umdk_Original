//
// Created by w00906336 on 2026/5/30.
//

#include "bondp_failback.h"
#include "bondp_failback_ops.h"
#include "bondp_netlink.h"

#include "bondp_context_table.h"
#include "urma_api.h"
#include "urma_log.h"

static void bondp_failback_cancel_timer(bondp_failback_task_t *task);
static void bondp_failback_rebuild_task(void *arg);
static void bondp_failback_send_next_req_task(void *arg);

static int bondp_find_target_idx_by_local_idx(
    const bondp_target_jetty_t *bdp_tjetty, int local_idx, int *target_idx)
{
    for (uint32_t n = 0; n < bdp_tjetty->active_count; ++n) {
        if ((int)bdp_tjetty->local_active_indices[n] == local_idx) {
            *target_idx = (int)bdp_tjetty->active_indices[n];
            return BONDP_FB_OK;
        }
    }
    return BONDP_FB_ERR;
}

bondp_failback_task_t *bondp_failback_task_create(
    bondp_context_t *bdp_ctx,
    struct bondp_comp *bondp_jetty,
    int failed_local_idx)
{
    bondp_failback_task_t *task = (bondp_failback_task_t *)calloc(1, sizeof(bondp_failback_task_t));
    if (task == NULL) {
        return NULL;
    }
    // --- worker 调度 ---
    task->timer_id = 0;
    task->timer_valid = false;
    task->closed = false;

    // --- 状态机 (IDLE, 等待 on_cqe 触发) ---
    task->state = BONDP_FAILBACK_STATE_IDLE;
    task->req_seq = 0;
    task->retry_ms = 0;

    // --- 身份标识 ---
    task->bdp_ctx = bdp_ctx;
    task->bondp_jetty = bondp_jetty;
    task->vjetty_id = bondp_jetty->v_jetty.jetty_id.id;

    // --- 路径信息 ---
    task->failed_local_idx = failed_local_idx;
    task->target_idx = -1;
    task->new_pjetty = NULL;
    task->new_pjetty_id = 0;
    task->bdp_tjetty = NULL;
    task->remote_new_pjetty_id = 0;

    // --- 广播通知跟踪 ---
    // TODO: node_count 从 bdp_ctx 获取实际 NODE 数量
    task->node_count = 0;
    task->sent_count = 0;
    task->notified_count = 0;
    task->next_send_idx = 0;
    // node_states[] 已由 calloc 初始化为 BONDP_FAILBACK_NODE_PENDING (0)

    return task;
}

void bondp_failback_task_destroy(bondp_failback_task_t *task)
{
    if (task == NULL) {
        return;
    }
    bondp_failback_cancel_timer(task);
    if (task->new_pjetty != NULL) {
        //新增重建失败，任务取消时的newjetty回收
        (void)urma_delete_jetty(task->new_pjetty);
        task->new_pjetty = NULL;
    }
    // 从 bondp_comp_t 的 failback_tasks 数组中移除
    if (task->bondp_jetty != NULL && task->failed_local_idx >= 0 &&
        task->failed_local_idx < URMA_UBAGG_DEV_MAX_NUM) {
        task->bondp_jetty->failback_tasks[task->failed_local_idx] = NULL;
    }
    free(task);
}


//指数退避辅助函数
static uint32_t bondp_failback_next_backoff(uint32_t cur, uint32_t max_ms)
{
    uint32_t next = cur * 2;
    if (next > max_ms) {
        next = max_ms;
    }
    return next;
}

//终止failback自调度
static void bondp_failback_cancel_timer(bondp_failback_task_t *task)
{
    if (!task->timer_valid) {
        return;
    }
    (void)bondp_worker_cancel(task->timer_id);
    task->timer_valid = false;
}

// 封装 worker_schedule: 先 cancel 旧定时器再注册新的, 保证同一 task 有且仅有一个待执行定时器
static int bondp_failback_schedule(bondp_failback_task_t *task, uint64_t delay_ms, bondp_worker_task_fn_t fn)
{
    int ret;
    if (task == NULL || task->closed) {
        return TW_INVALID_PARAM_ERROR;
    }
    bondp_failback_cancel_timer(task);
    //比常规schedule多一层封装，用于保证failback严格的单线
    ret = bondp_worker_schedule(delay_ms, fn, task, &task->timer_id);
    if (ret == TW_OK) {
        task->timer_valid = true;
    }
    return ret;
}

int bondp_failback_on_cqe(bondp_context_t *bdp_ctx,
    struct bondp_comp *bondp_jetty,
    int failed_local_idx)
{
    // 校验: 同一 vjetty 同一 failed_local_idx 只能有一个活跃 task
    if (failed_local_idx < 0 || failed_local_idx >= URMA_UBAGG_DEV_MAX_NUM) {
        return BONDP_FB_ERR;
    }
    if (bondp_jetty->failback_tasks[failed_local_idx] != NULL) {
        URMA_LOG_WARN("Failback task already exists for vjetty=%u idx=%d, skip\n",
            bondp_jetty->v_jetty.jetty_id.id, failed_local_idx);
        return BONDP_FB_OK;
    }

    bondp_failback_task_t *task = bondp_failback_task_create(bdp_ctx, bondp_jetty, failed_local_idx);
    if (task == NULL) {
        return TW_NO_MEMORY_ERROR;
    }

    // TODO: 初始化 node_count 从 bdp_ctx 获取实际 NODE 数量/标号  标记不需要使用的nodeid
    // task->node_count = bdp_ctx->node_count;


    // 注册 task 到 bondp_comp_t, 便于 RESP 到达时查找
    bondp_jetty->failback_tasks[failed_local_idx] = task;

    task->state = BONDP_FAILBACK_STATE_WAIT_REBUILD_START;
    task->retry_ms = BONDP_FAILBACK_REBUILD_RETRY_INIT_MS;
    int ret = bondp_failback_schedule(task, BONDP_FAILBACK_REBUILD_START_DELAY_MS, bondp_failback_rebuild_task);
    return ret;
}

// ========== 状态机: rebuild ==========

static void bondp_failback_rebuild_task(void *arg)
{
    bondp_failback_task_t *task = arg;
    if (task == NULL || task->closed) {
        return;
    }
    task->timer_valid = false;

    if (task->state != BONDP_FAILBACK_STATE_WAIT_REBUILD_START &&
        task->state != BONDP_FAILBACK_STATE_REBUILDING) {
        return;
    }

    task->state = BONDP_FAILBACK_STATE_REBUILDING;
    int ret = bondp_failback_rebuild_local_pjetty(task);
    if (ret != 0) {
        //rebuild 失败。schedule 自己。
        (void)bondp_failback_schedule(task, task->retry_ms, bondp_failback_rebuild_task);
        task->retry_ms = bondp_failback_next_backoff(task->retry_ms, BONDP_FAILBACK_REBUILD_RETRY_MAX_MS);
        return;
    }

    //rebuild 成功。准备逐个发送 REQ。
    task->state = BONDP_FAILBACK_SENDING_REQ;
    task->next_send_idx = 0;
    task->sent_count = 0;
    task->notified_count = 0;
    task->retry_ms = BONDP_FAILBACK_BROADCAST_RETRY_INIT_MS;
    // 立即开始发送第一条 REQ
    (void)bondp_failback_schedule(task, 0, bondp_failback_send_next_req_task);
}

/*
 * 逐个发送 REQ
 * 每次调用发送一条 REQ 给 next_send_idx 对应的 NODE,
 * 然后以 SEND_INTERVAL_MS 间隔调度下一次发送
 * 当 next_send_idx 达到 node_count 时, 一轮发送完成
 */
static void bondp_failback_send_next_req_task(void *arg)
{
    bondp_failback_task_t *task = arg;
    if (task == NULL || task->closed) {
        return;
    }
    task->timer_valid = false;

    if (task->state != BONDP_FAILBACK_SENDING_REQ &&
        task->state != BONDP_FAILBACK_STATE_WAIT_RSP) {
        return;
    }

    // 找到下一个 PENDING 的 NODE (未收到 RESP)
    uint32_t idx = task->next_send_idx;
    while (idx < task->node_count && task->node_states[idx] == BONDP_FAILBACK_NODE_SATTLED) {
        idx++;
    }

    //如果找不到，则说明一轮结束，增加序列号，回到0 执行指数退避
    if (idx >= task->node_count) {
        task->req_seq = task->req_seq + 1;
        task->next_send_idx = 0;
        task->retry_ms = bondp_failback_next_backoff(task->retry_ms, BONDP_FAILBACK_BROADCAST_RETRY_MAX_MS);
        URMA_LOG_INFO("Failback broadcast round done, vjetty=%u notified=%u/%u retry_ms=%u\n",
            task->vjetty_id, task->notified_count, task->node_count, task->retry_ms);
        (void)bondp_failback_schedule(task, task->retry_ms, bondp_failback_send_next_req_task);
        return;
    }

    int ret = bondp_failback_send_req_to_node(task, idx);
    //idx可以建立一个更加快速的映射表(优先级不高)
    if (ret != BONDP_FB_OK) {
        URMA_LOG_WARN("Failback REQ send failed, node[%u] vjetty=%u pjetty_i_idx=%d new_pjetty_id=%u\n",
            idx, task->vjetty_id, task->failed_local_idx, task->new_pjetty_id);
    }
    task->next_send_idx = idx + 1;
    (void)bondp_failback_schedule(task, BONDP_FAILBACK_SEND_INTERVAL_MS, bondp_failback_send_next_req_task);

}

// ========== RESP 处理 ==========

/*
 * 通过 vjetty_id 查找本端的 bondp_comp_t (实现在 bondp_failback_ops.c)
 */
extern bondp_comp_t *bondp_find_comp_by_vjetty_id(bondp_context_t *bdp_ctx, uint32_t vjetty_id);

/*
 * 处理收到的 netlink 消息 (REQ / RESP)
 *
 * 使用消息格式 bondp_failback_msg_t:
 *   vjetty_id       - 发送端 vjetty ID
 *   pjetty_i_idx    - 发送端故障 pjetty idx
 *   pjetty_i_new_id - REQ: 新 pjetty ID (用于 reimport); RESP: 对端 pjetty ID
 *   ctrl_type       - BONDP_FAILBACK_CTRL_REQ / RESP
 *   req_seq         - 序列号
 *
 * RESP 查找链路:
 *   vjetty_id → p_vjetty_id_table → bondp_comp_t → failback_tasks[pjetty_i_idx]
 *
 * RESP 广播收集:
 *   RESP.node_id → 更新 node_states[node_id] = NOTIFIED
 *   全部 NOTIFIED → relink → done
 */
static bool bondp_handle_message(bondp_context_t *bdp_ctx, const bondp_failback_msg_t *msg,
    bool silent_unmatched)
{
    uint8_t ctrl_type = msg->in.ctrl_type;
    uint8_t req_seq = msg->in.req_seq;
    uint32_t vjetty_id = msg->in.vjetty_id;
    uint32_t pjetty_i_idx = msg->in.pjetty_i_idx;
    uint32_t pjetty_i_new_id = msg->in.pjetty_i_new_id;
    uint32_t node_id = msg->in.node_id;

    if (ctrl_type != BONDP_FAILBACK_CTRL_REQ && ctrl_type != BONDP_FAILBACK_CTRL_RESP) {
        if (!silent_unmatched) {
            URMA_LOG_WARN("Invalid fallback ctrl type=%u vjetty_id=%u\n", ctrl_type, vjetty_id);
        }
        return false;
    }

    if (ctrl_type == BONDP_FAILBACK_CTRL_REQ) {
        // 第二步: 对端处理 REQ
        // 通过 vjetty_id 遍历本地 bdp_tjetty, 找到匹配的 target jetty
        // 然后通过 pjetty_i_idx (发送端故障 idx) 匹配 active_indices 定位 reimport 路径
        // unimport 旧 ptjetty, 用 pjetty_i_new_id 重新 import, 发送 RESP
        URMA_LOG_INFO("Fallback REQ received, vjetty_id=%u pjetty_i_idx=%u pjetty_i_new_id=%u seq=%u\n",
            vjetty_id, pjetty_i_idx, pjetty_i_new_id, req_seq);
        int ret = bondp_failback_handle_req(bdp_ctx, msg);
        return (ret == BONDP_FB_OK);
    }

    if (ctrl_type == BONDP_FAILBACK_CTRL_RESP) {
        // 第三步: 本端处理 RESP (广播收集式)
        //
        // 查找链路: vjetty_id → bondp_comp_t → failback_tasks[pjetty_i_idx]

        // Step 1: 通过 vjetty_id 找到本端的 bondp_comp_t
        bondp_comp_t *bdp_jetty = bondp_find_comp_by_vjetty_id(bdp_ctx, vjetty_id);
        if (bdp_jetty == NULL) {
            if (!silent_unmatched) {
                URMA_LOG_WARN("Fallback RESP: no matching comp for vjetty_id=%u\n", vjetty_id);
            }
            return false;
        }

        // Step 2: 通过 pjetty_i_idx 从 failback_tasks 数组中直接索引
        if (pjetty_i_idx >= URMA_UBAGG_DEV_MAX_NUM) {
            if (!silent_unmatched) {
                URMA_LOG_WARN("Fallback RESP: invalid pjetty_i_idx=%u vjetty_id=%u\n",
                    pjetty_i_idx, vjetty_id);
            }
            return false;
        }

        bondp_failback_task_t *ctx = bdp_jetty->failback_tasks[pjetty_i_idx];
        if (ctx == NULL) {
            if (!silent_unmatched) {
                URMA_LOG_WARN("Fallback RESP: no active task for vjetty_id=%u pjetty_i_idx=%u\n",
                    vjetty_id, pjetty_i_idx);
            }
            return false;
        }

        // Step 3: 校验序列号
        if (ctx->req_seq != req_seq) {
            URMA_LOG_WARN("Fallback RESP dropped due to seq mismatch, recv_seq=%u expect_seq=%u vjetty_id=%u\n",
                req_seq, (unsigned)ctx->req_seq, vjetty_id);
            return false;
        }

        // Step 4: 更新 node_states
        // node_id 作为 node_states 数组的索引
        if (node_id >= ctx->node_count) {
            URMA_LOG_WARN("Fallback RESP: node_id=%u >= node_count=%u, ignore\n",
                node_id, ctx->node_count);
            return false;
        }

        // 去重: 已经 NOTIFIED 的 NODE 不重复计数
        if (ctx->node_states[node_id] == BONDP_FAILBACK_NODE_SATTLED) {
            URMA_LOG_DEBUG("Fallback RESP: node[%u] already notified, skip\n", node_id);
            return true;
        }

        ctx->node_states[node_id] = BONDP_FAILBACK_NODE_SATTLED;
        ctx->notified_count++;

        // 记录对端 pjetty ID (非 0 表示需要 reimport)
        if (pjetty_i_new_id != 0) {
            // TODO: 多 NODE 都 reimport 时, 需要按 node_id 索引存储
            ctx->remote_new_pjetty_id = pjetty_i_new_id;
        }

        URMA_LOG_INFO("Fallback RESP accepted, node[%u] vjetty_id=%u notified=%u/%u remote_id=%u\n",
            node_id, vjetty_id, ctx->notified_count, ctx->node_count, pjetty_i_new_id);

        // Step 5: 检查是否全部 NODE 已响应
        if (ctx->notified_count >= ctx->node_count) {
            // 全部 NODE 已响应, 执行 relink
            bondp_failback_cancel_timer(ctx);
            int ret = bondp_failback_relink(ctx);
            if (ret == BONDP_FB_OK) {
                bondp_failback_task_destroy(ctx);
            } else {
                URMA_LOG_ERR("Fallback RESP: relink failed, vjetty_id=%u pjetty_i_idx=%u\n",
                    vjetty_id, pjetty_i_idx);
            }
        }
        // 否则: 继续等待其他 NODE 的 RESP, 定时器驱动的发送流程会继续
    }

    return true;
}
