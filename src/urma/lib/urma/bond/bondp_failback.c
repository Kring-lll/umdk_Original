//
// Created by w00906336 on 2026/5/30.
//

#include "bondp_failback.h"
#include "bondp_failback_ops.h"

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
    task->node_id = node_id;

    // --- 路径信息 ---
    task->failed_local_idx = failed_local_idx;
    task->primary_target_idx = -1;
    task->new_pjetty = NULL;
    task->new_pjetty_id = 0;
    task->bdp_tjetty = NULL;
    task->remote_new_pjetty_id = 0;

    // sub_tasks: calloc 已清零, 无需额外初始化
    //TODO: hmap_node: 注册到 task_table 时由 hash_table 填入 有必要接入吗

    return task;
}

void bondp_failback_task_destroy(bondp_failback_task_t *task)
{
    if (task == NULL) {
        return;
    }
    bondp_failback_cancel_timer(task);
    if (task->new_pjetty != NULL) {
        (void)urma_delete_jetty(task->new_pjetty);
        task->new_pjetty = NULL;
    }
    //新增重建失败，任务取消时的newjetty回收
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

    //TODO: 多个CQE的交互，同时failed_local_idx相等的 task只有一个

    //bondp_failback_cancel_timer(task);

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
    task->retry_ms = BONDP_FO_SEND_REQ_INIT_MS;
    (void)bondp_failback_schedule(ctx, 0, bondp_failback_send_req_task);
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
    bondp_failback_task_t *ctx = arg;
    if (ctx == NULL || ctx->closed) {
        return;
    }
    ctx->timer_valid = false;

    if (ctx->state != BONDP_FAILBACK_STATE_WAIT_RSP) {
        return;
    }

    ctx->req_seq++;
    int ret = bondp_failback_send_req(ctx);
    if (ret != 0) {
        ctx->state = BONDP_FAILBACK_SENDING_REQ;
        ctx->retry_ms = BONDP_FAILBACK_SEND_REQ_MAX_MS;
        (void)bondp_failback_schedule(ctx, ctx->retry_ms, bondp_failback_send_req_task);
        return;
    }

    (void)bondp_failback_schedule(ctx, ctx->retry_ms, bondp_failback_req_timeout_task);
    ctx->retry_ms = bondp_failback_next_backoff(ctx->retry_ms, BONDP_FAILBACK_REQ_RSP_TIMEOUT_MAX_MS);
}

static bool bondp_handle_message(bondp_context_t *bdp_ctx, uint32_t recv_local_id,
    uint8_t ctrl_type, uint8_t req_seq, uint32_t payload, bool silent_unmatched)
{
    if (ctrl_type != BONDP_FAILBACK_CTRL_REQ && ctrl_type != BONDP_FAILBACK_CTRL_RESP) {
        if (!silent_unmatched) {
            URMA_LOG_WARN("Invalid fallback ctrl type=%u local_id=%u\n", ctrl_type, recv_local_id);
        }
        return false;
    }

    bondp_failback_task_t *ctx = NULL;
    // TODO: 根据 recv_local_id 查找对应的 failback task

    if (ctx == NULL) {
        if (!silent_unmatched) {
            URMA_LOG_WARN("Fallback ctrl not matched by local_id=%u\n", recv_local_id);
        }
        return false;
    }

    if (ctrl_type == BONDP_FAILBACK_CTRL_REQ) {
        // TODO: 第二步 - 对端处理 REQ
    }

    if (ctrl_type == BONDP_FAILBACK_CTRL_RESP) {
        if (ctx->req_seq == req_seq) {
            ctx->remote_new_pjetty_id = payload;
            URMA_LOG_INFO("Fallback RESP accepted, seq=%u remote_new_pjetty_id=%u\n",
                req_seq, ctx->remote_new_pjetty_id);
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
