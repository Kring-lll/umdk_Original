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
static void bondp_failback_send_req_task(void *arg);
static void bondp_failback_req_timeout_task(void *arg);

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

    //TODO: 故障jetty 对端 可以通过find target找到
    /*
    // 从 v_jetty.remote_jetty 推导 bdp_tjetty 和 target_idx
    if (bondp_jetty->v_jetty.remote_jetty != NULL) {
        task->bdp_tjetty = CONTAINER_OF_FIELD(
            bondp_jetty->v_jetty.remote_jetty, bondp_target_jetty_t, v_tjetty);
        (void)bondp_find_target_idx_by_local_idx(
            task->bdp_tjetty, failed_local_idx, &task->target_idx);
    }
    */

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
    bondp_failback_task_t *task = bondp_failback_task_create(bdp_ctx, bondp_jetty, failed_local_idx);
    if (task == NULL) {
        return TW_NO_MEMORY_ERROR;
    }

    //TODO: 多个CQE的交互，同时failed_local_idx相等的 task只有一个  原版有一个task的hash表实现
    /*
    // 校验 bdp_tjetty 和 target_idx 必须有效才能启动 failback
    if (task->bdp_tjetty == NULL || task->target_idx < 0) {
        URMA_LOG_ERR("Cannot start failback, bdp_tjetty or target_idx invalid, vjetty=%u idx=%d\n",
            task->vjetty_id, failed_local_idx);
        bondp_failback_task_destroy(task);
        return BONDP_FB_ERR;
    }
    */
    task->state = BONDP_FAILBACK_STATE_WAIT_REBUILD_START;
    task->retry_ms = BONDP_FAILBACK_REBUILD_RETRY_INIT_MS;
    int ret = bondp_failback_schedule(task, BONDP_FAILBACK_REBUILD_START_DELAY_MS, bondp_failback_rebuild_task);
    return ret;
}

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
    //理应不存在的异常状态

    task->state = BONDP_FAILBACK_STATE_REBUILDING;
    int ret = bondp_failback_rebuild_local_pjetty(task);
    if (ret != 0) {
        //rebuild 失败。schedule 自己。
        (void)bondp_failback_schedule(task, task->retry_ms, bondp_failback_rebuild_task);
        task->retry_ms = bondp_failback_next_backoff(task->retry_ms, BONDP_FAILBACK_REBUILD_RETRY_MAX_MS);
        return;
    }

    //rebuild 成功。准备发送 REQ。
    task->state = BONDP_FAILBACK_SENDING_REQ;
    //发送退避从初始值开始。
    task->retry_ms = BONDP_FAILBACK_SEND_REQ_INIT_MS;
    (void)bondp_failback_schedule(task, 0, bondp_failback_send_req_task);
}

static void bondp_failback_send_req_task(void *arg)
{
    bondp_failback_task_t *task = arg;
    if (task == NULL || task->closed) {
        return;
    }
    task->timer_valid = false;

    if (task->state != BONDP_FAILBACK_SENDING_REQ && task->state != BONDP_FAILBACK_STATE_WAIT_RSP) {
        return;
    }

    task->state = BONDP_FAILBACK_SENDING_REQ;

    task->req_seq++;
    int ret = bondp_failback_send_req(task);
    if (ret != 0) {
        //send 失败。按指数退避后重新发送。
        (void)bondp_failback_schedule(task, task->retry_ms, bondp_failback_send_req_task);
        task->retry_ms = bondp_failback_next_backoff(task->retry_ms, BONDP_FAILBACK_SEND_REQ_MAX_MS);
        return;
    }

    task->state = BONDP_FAILBACK_STATE_WAIT_RSP;
    task->retry_ms = BONDP_FAILBACK_REQ_RSP_TIMEOUT_INIT_MS;
    (void)bondp_failback_schedule(task, task->retry_ms, bondp_failback_req_timeout_task);
}

static void bondp_failback_req_timeout_task(void *arg)
{
    bondp_failback_task_t *task = arg;
    if (task == NULL || task->closed) {
        return;
    }
    task->timer_valid = false;

    if (task->state != BONDP_FAILBACK_STATE_WAIT_RSP) {
        return;
    }

    task->req_seq++;
    int ret = bondp_failback_send_req(task);
    if (ret != 0) {
        task->state = BONDP_FAILBACK_SENDING_REQ;
        task->retry_ms = BONDP_FAILBACK_SEND_REQ_MAX_MS;
        (void)bondp_failback_schedule(task, task->retry_ms, bondp_failback_send_req_task);
        return;
    }

    (void)bondp_failback_schedule(task, task->retry_ms, bondp_failback_req_timeout_task);
    task->retry_ms = bondp_failback_next_backoff(task->retry_ms, BONDP_FAILBACK_REQ_RSP_TIMEOUT_MAX_MS);
}


static bool bondp_handle_message(bondp_context_t *bdp_ctx, const bondp_failback_msg_t *msg,
    bool silent_unmatched)
{
    uint8_t ctrl_type = msg->in.ctrl_type;
    uint8_t req_seq = msg->in.req_seq;
    uint32_t vjetty_id = msg->in.vjetty_id;
    uint32_t pjetty_i_idx = msg->in.pjetty_i_idx;
    uint32_t pjetty_i_new_id = msg->in.pjetty_i_new_id;

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
        // TODO: 实现对端 REQ 处理逻辑
        URMA_LOG_INFO("Fallback REQ received, vjetty_id=%u pjetty_i_idx=%u pjetty_i_new_id=%u seq=%u\n",
            vjetty_id, pjetty_i_idx, pjetty_i_new_id, req_seq);
        return true;
    }

    if (ctrl_type == BONDP_FAILBACK_CTRL_RESP) {
        // 第三步: 本端处理 RESP
        // 通过 vjetty_id 匹配本端的 failback task
        bondp_failback_task_t *ctx = NULL;
        // TODO: 根据 vjetty_id 查找对应的 failback task

        if (ctx == NULL) {
            if (!silent_unmatched) {
                URMA_LOG_WARN("Fallback RESP not matched by vjetty_id=%u\n", vjetty_id);
            }
            return false;
        }

        if (ctx->req_seq == req_seq) {
            // RESP 中的 pjetty_i_new_id 是对端的 pjetty ID
            ctx->remote_new_pjetty_id = pjetty_i_new_id;
            URMA_LOG_INFO("Fallback RESP accepted, seq=%u vjetty_id=%u remote_new_pjetty_id=%u\n",
                req_seq, vjetty_id, ctx->remote_new_pjetty_id);
            int ret = bondp_failback_relink(ctx);
            if (ret == BONDP_FB_OK) {
                bondp_failback_cancel_timer(ctx);
            }
        } else {
            URMA_LOG_WARN("Fallback RESP dropped due to seq mismatch, recv_seq=%u expect_seq=%u\n",
                req_seq, (unsigned)ctx->req_seq);
        }
    }

    return true;
}
