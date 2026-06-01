//
// Created by w00906336 on 2026/5/30.
//

#include "bondp_failback.h"
#include "bondp_link_recovery.h"

#include "bondp_context_table.h"
#include "urma_log.h"
#include "urma_api.h"

//指数退避辅助函数
static uint32_t bondp_fo_next_backoff(uint32_t cur, uint32_t max_ms)
{
    uint32_t next;
    next = cur * 2;
    if (next > max_ms) {
        next = max_ms;
    }
    return next;
}

//成功后终止failover自调度
static void bondp_fo_cancel_timer(bondp_failover_ctx_t *ctx)
{
    if (!ctx->timer_valid) {
        return;
    }
    (void)bondp_worker_cancel(ctx->timer_id);
    ctx->timer_valid = false;
}

static int bondp_fo_schedule(bondp_failover_ctx_t *ctx, uint64_t delay_ms,bondp_worker_task_fn_t fn)
{
    int ret;
    if (ctx == NULL || ctx->closed) {
        return TW_INVALID_PARAM_ERROR;
    }
    bondp_fo_cancel_timer(ctx);
    //比常规schedule多一层封装，用于保证failback严格的单线
    ret = bondp_worker_schedule(delay_ms, fn, ctx, &ctx->timer_id);
    if (ret == TW_OK) {
        ctx->timer_valid = true;
    }
    return ret;
}

//整段的入口
//通过多个ctx或许可以同时检测多条链路的回切等状态？
int bondp_failover_on_cqe(bondp_failover_ctx_t *ctx)
{
    int ret;
    if (ctx == NULL || ctx->closed) {
        return TW_INVALID_PARAM_ERROR;
    }
    /*
     * 收到新的 failover CQE，重新开始该流程。
     * 如果之前有 timer，先取消。
     */
    bondp_fo_cancel_timer(ctx);

    ctx->state = BONDP_FO_STATE_WAIT_REBUILD_START;
    ctx->retry_ms = BONDP_FO_REBUILD_RETRY_INIT_MS;
    //开始新的流程，重置回环等待时间
    ret = bondp_fo_schedule(ctx,BONDP_FO_REBUILD_START_DELAY_MS, bondp_fo_rebuild_task);
    //规划等待10s开始
    return ret;
}

static void bondp_fo_rebuild_task(void *arg);
static void bondp_fo_send_req_task(void *arg);
static void bondp_fo_req_timeout_task(void *arg);

static void bondp_fo_rebuild_task(void *arg)
{
    bondp_failover_ctx_t *ctx = arg;
    int ret;
    if (ctx == NULL || ctx->closed) {
        return;
    }
    ctx->timer_valid = false;

    if (ctx->state != BONDP_FO_STATE_WAIT_REBUILD_START &&ctx->state != BONDP_FO_STATE_REBUILDING) {
        return;
    }
    //理应不存在的异常状态

    ctx->state = BONDP_FO_STATE_REBUILDING;
    ret = bondp_rebuild_local_pjetty(ctx);
    if (ret != 0) {
        //rebuild 失败。schedule 自己。
        (void)bondp_fo_schedule(ctx, ctx->retry_ms, bondp_fo_rebuild_task);
        ctx->retry_ms = bondp_fo_next_backoff(ctx->retry_ms, BONDP_FO_REBUILD_RETRY_MAX_MS);
        return;
    }

    //rebuild 成功。准备发送 REQ。
    ctx->state = BONDP_FO_SENDING_REQ;
    //发送退避从初始值开始。
    ctx->retry_ms = BONDP_FO_SEND_REQ_INIT_MS;
    (void)bondp_fo_schedule(ctx, 0, bondp_fo_send_req_task);
}

static void bondp_fo_send_req_task(void *arg)
{
    bondp_failover_ctx_t *ctx = arg;
    if (ctx == NULL || ctx->closed) {
        return;
    }
    ctx->timer_valid = false;

    if (ctx->state != BONDP_FO_SENDING_REQ && ctx->state != BONDP_FO_STATE_WAIT_RSP) {
        return;
    }
    //检查状态
    ctx->state = BONDP_FO_SENDING_REQ;

    ctx->req_seq++;
    int ret = bondp_send_failover_req(ctx);
    if (ret != 0) {
        //send 失败。按指数退避后重新发送。
        (void)bondp_fo_schedule(ctx, ctx->retry_ms, bondp_fo_send_req_task);
        ctx->retry_ms = bondp_fo_next_backoff(ctx->retry_ms, BONDP_FO_SEND_REQ_MAX_MS);
        return;
    }

    //send 成功，进入等待 RSP 状态。
    ctx->state = BONDP_FO_STATE_WAIT_RSP;
    ctx->retry_ms = BONDP_FO_REQ_RSP_TIMEOUT_INIT_MS;
    //等待 RSP 的 timeout。如果 timeout 期间没有收到 RSP，则进入 req_timeout_task。
    (void)bondp_fo_schedule(ctx,ctx->retry_ms, bondp_fo_req_timeout_task);
}

static void bondp_fo_req_timeout_task(void *arg)
{
    bondp_failover_ctx_t *ctx = arg;
    if (ctx == NULL || ctx->closed) {
        return;
    }
    ctx->timer_valid = false;

    if (ctx->state != BONDP_FO_STATE_WAIT_RSP) {
        return;
    }

    ctx->req_seq++;
    int ret = bondp_send_failover_req(ctx);
    if (ret != 0) {
        ctx->state = BONDP_FO_SENDING_REQ;
        ctx->retry_ms = BONDP_FO_SEND_REQ_MAX_MS;
        //之前发送成功，新发送不成功，说明有变化，直接切到send最大延迟
        (void)bondp_fo_schedule(ctx, ctx->retry_ms, bondp_fo_send_req_task);
        return;
    }

    (void)bondp_fo_schedule(ctx,ctx->retry_ms, bondp_fo_req_timeout_task);
    ctx->retry_ms = bondp_fo_next_backoff(ctx->retry_ms, BONDP_FO_REQ_RSP_TIMEOUT_MAX_MS);
}

