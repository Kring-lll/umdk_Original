//
// Created by w00906336 on 2026/5/30.
//

#include "bondp_failback.h"
#include "bondp_link_recovery.h"

#include "bondp_context_table.h"
#include "urma_log.h"
#include "urma_api.h"

//指数退避辅助函数
static uint32_t bondp_failback_next_backoff(uint32_t cur, uint32_t max_ms)
{
    uint32_t next;
    next = cur * 2;
    if (next > max_ms) {
        next = max_ms;
    }
    return next;
}

//终止failback自调度
static void bondp_failback_cancel_timer(bondp_failback_ctx_t *ctx)
{
    if (!ctx->timer_valid) {
        return;
    }
    (void)bondp_worker_cancel(ctx->timer_id);
    ctx->timer_valid = false;
}

static int bondp_failback_schedule(bondp_failback_ctx_t *ctx, uint64_t delay_ms,bondp_worker_task_fn_t fn)
{
    int ret;
    if (ctx == NULL || ctx->closed) {
        return TW_INVALID_PARAM_ERROR;
    }
    bondp_failback_cancel_timer(ctx);
    //比常规schedule多一层封装，用于保证failback严格的单线
    ret = bondp_worker_schedule(delay_ms, fn, ctx, &ctx->timer_id);
    if (ret == TW_OK) {
        ctx->timer_valid = true;
    }
    return ret;
}

//整段的入口
//通过多个ctx或许可以同时检测多条链路的回切等状态？

//TODO:bondp_failback_ctx_t的构造，注册与销毁

//

//真正需要的入参，struct bondp_comp *bondp_jetty; 要重建的本端jetty  index也是本端
//

//Q 不需要指定对端的jetty吗 后续有import的话 具体import哪个？
//报文还要修改


int bondp_failback_on_cqe(bondp_failback_ctx_t *ctx)
{
    int ret;
    if (ctx == NULL || ctx->closed) {
        return TW_INVALID_PARAM_ERROR;
    }
    /*
     * 收到新的 failover CQE，重新开始该流程。
     * 如果之前有 timer，先取消。
     */
    bondp_failback_cancel_timer(ctx);

    ctx->state = BONDP_FAILBACK_STATE_WAIT_REBUILD_START;
    ctx->retry_ms = BONDP_FO_REBUILD_RETRY_INIT_MS;
    //开始新的流程，重置回环等待时间
    ret = bondp_failback_schedule(ctx,BONDP_FAILBACK_REBUILD_START_DELAY_MS, bondp_fo_rebuild_task);
    //规划等待10s开始
    return ret;
}

static void bondp_failback_rebuild_task(void *arg);
static void bondp_failback_send_req_task(void *arg);
static void bondp_failback_req_timeout_task(void *arg);

static void bondp_failback_rebuild_task(void *arg)
{
    bondp_failback_ctx_t *ctx = arg;
    int ret;
    if (ctx == NULL || ctx->closed) {
        return;
    }
    ctx->timer_valid = false;

    if (ctx->state != BONDP_FAILBACK_STATE_WAIT_REBUILD_START &&ctx->state != BONDP_FAILBACK_STATE_REBUILDING) {
        return;
    }
    //理应不存在的异常状态

    ctx->state = BONDP_FAILBACK_STATE_REBUILDING;
    ret = bondp_failback_rebuild_local_pjetty(ctx);
    if (ret != 0) {
        //rebuild 失败。schedule 自己。
        (void)bondp_failback_schedule(ctx, ctx->retry_ms, bondp_failback_rebuild_task);
        ctx->retry_ms = bondp_failback_next_backoff(ctx->retry_ms, BONDP_FO_REBUILD_RETRY_MAX_MS);
        return;
    }

    //rebuild 成功。准备发送 REQ。
    ctx->state = BONDP_FAILBACK_SENDING_REQ;
    //发送退避从初始值开始。
    ctx->retry_ms = BONDP_FO_SEND_REQ_INIT_MS;
    (void)bondp_failback_schedule(ctx, 0, bondp_failback_send_req_task);
}

static void bondp_failback_send_req_task(void *arg)
{
    bondp_failback_ctx_t *ctx = arg;
    if (ctx == NULL || ctx->closed) {
        return;
    }
    ctx->timer_valid = false;

    if (ctx->state != BONDP_FAILBACK_SENDING_REQ && ctx->state != BONDP_FAILBACK_STATE_WAIT_RSP) {
        return;
    }
    //检查状态
    ctx->state = BONDP_FAILBACK_SENDING_REQ;

    ctx->req_seq++;
    int ret = bondp_failback_send_req(ctx);
    if (ret != 0) {
        //send 失败。按指数退避后重新发送。
        (void)bondp_failback_schedule(ctx, ctx->retry_ms, bondp_failback_send_req_task);
        ctx->retry_ms = bondp_failback_next_backoff(ctx->retry_ms, BONDP_FO_SEND_REQ_MAX_MS);
        return;
    }

    //send 成功，进入等待 RSP 状态。
    ctx->state = BONDP_FAILBACK_STATE_WAIT_RSP;
    ctx->retry_ms = BONDP_FO_REQ_RSP_TIMEOUT_INIT_MS;
    //等待 RSP 的 timeout。如果 timeout 期间没有收到 RSP，则进入 req_timeout_task。
    (void)bondp_failback_schedule(ctx,ctx->retry_ms, bondp_failback_req_timeout_task);
}

static void bondp_failback_req_timeout_task(void *arg)
{
    bondp_failback_ctx_t *ctx = arg;
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
        ctx->retry_ms = BONDP_FO_SEND_REQ_MAX_MS;
        //之前发送成功，新发送不成功，说明有变化，直接切到send最大延迟
        (void)bondp_failback_schedule(ctx, ctx->retry_ms, bondp_failback_send_req_task);
        return;
    }

    (void)bondp_failback_schedule(ctx,ctx->retry_ms, bondp_failback_req_timeout_task);
    ctx->retry_ms = bondp_failback_next_backoff(ctx->retry_ms, BONDP_FO_REQ_RSP_TIMEOUT_MAX_MS);
}

/*
//TODO:这里还需要补上批量处理netlink事件的逻辑，修改参数传递
创建netlink fd addfd 将
收包

DONE:统一关键字，这部分逻辑有fallback、failover、switchback混用
static void bondp_handle_netlink_events(void)
{
    while (true) {
        //遍历所有消息
        bondp_switchback_msg_t msg = {0};
        int ret = bondp_nl_recv_switchback_msg(&msg);
        if (ret == -EAGAIN) {
            break;
        }
        //处理完了则结束
        if (ret != 0) {
            URMA_LOG_WARN("Failed to recv switchback msg from netlink, ret=%d\n", ret);
            break;
        }
        bool consumed = false;
        bondp_global_context_t *global_ctx = g_bondp_global_ctx;
        if (global_ctx == NULL) {
            break;
        }
        pthread_rwlock_rdlock(&global_ctx->health_thread_ctx.health_ctx_lock);
        //注意并发安全
        bondp_health_ctx_node_t *ctx_node = NULL;
        UB_LIST_FOR_EACH(ctx_node, node, &global_ctx->health_thread_ctx.health_ctx_list) {
            if (ctx_node->bdp_ctx == NULL) {
                continue;
            }
            if (bondp_health_handle_fallback_ctrl_rx_impl(ctx_node->bdp_ctx, msg.in.recv_local_id,
                msg.in.ctrl_type, msg.in.req_seq, msg.in.payload, true)) {
                //实际的处理函数
                consumed = true;
                break;
            }
        }
        pthread_rwlock_unlock(&global_ctx->health_thread_ctx.health_ctx_lock);
        if (!consumed) {
            URMA_LOG_WARN("Drop switchback netlink msg: local_id=%u type=%u seq=%u payload=%u not matched\n",
                msg.in.recv_local_id, msg.in.ctrl_type, msg.in.req_seq, msg.in.payload);
            //记录没处理的信息
        }
    }
}
*/

//这个里面要做的事情
//入口流程
static bool bondp_handle_message(bondp_context_t *bdp_ctx, uint32_t recv_local_id,
    uint8_t ctrl_type, uint8_t req_seq, uint32_t payload, bool silent_unmatched)
{
    //health_check没开，则直接退出
    if (ctrl_type != BONDP_FO_CTRL_REQ && ctrl_type != BONDP_FO_CTRL_RESP) {
        if (!silent_unmatched) {
            URMA_LOG_WARN("Invalid fallback ctrl type=%u local_id=%u\n", ctrl_type, recv_local_id);
        }
        return false;
    }
    //检查消息的处理，只能处理上述两种消息 BONDP_FALLBACK_CTRL_REQ就是healthcheck发出来的
    //TODO:此处本质上是要将一个外部知晓的id 转换为本地实际应当响应消息的ctx 视实际情况而定
    bondp_failback_ctx_t *ctx;
    //pthread_rwlock_lock(&health->task_table.lock); 此处注意并发

    if (ctx == NULL) {
        if (!silent_unmatched) {
            URMA_LOG_WARN("Fallback ctrl not matched by local_id=%u\n", recv_local_id);
        }
        return false;
    }
    //找不到则报错


    if (ctrl_type == BONDP_FAILBACK_CTRL_REQ) {
        //TODO:根据消息修改respounder端的ctx，可能要改primary_local_idx等一系列

        //可能REQ需求对接的id和当前使用的不一致，此处之前使用有主备切换的残留，备切主要切回去
        // uint32_t req_expected_primary_id = payload;
        // uint32_t local_primary_id = BONDP_FALLBACK_PRIMARY_INVALID_ID;
        // urma_jetty_t *local_primary_jetty = task->bondp_jetty->p_jetty[task->primary_local_idx];
        // bool need_rebuild = (local_primary_id == BONDP_FALLBACK_PRIMARY_INVALID_ID ||
        //     req_expected_primary_id != local_primary_id);

        bool need_rebuild;
        int ret = BONDP_FB_OK;
        if (need_rebuild) {
            ret = bondp_failback_rebuild_local_pjetty(ctx);
        }
        if (ret == BONDP_FB_OK) {
            ret = bondp_failback_relink(ctx);
        }
        if (ret == BONDP_FB_OK) {
            ctx->state = BONDP_FAILBACK_STATE_DONE;
        }
    }

    if (ctrl_type == BONDP_FAILBACK_CTRL_RESP) {
        int ret = BONDP_FB_ERR;
        if (ctx->req_seq == req_seq) {
            ctx->remote_primary_pjetty_id = payload;
            URMA_LOG_INFO("Fallback RESP accepted, seq=%u remote_primary_id=%u\n",
                req_seq, ctx->remote_primary_pjetty_id);
            ret = bondp_failback_relink(ctx);
            if (ret == BONDP_FB_OK) {
                bondp_failback_cancel_timer(ctx);
            }
        } else {
            URMA_LOG_WARN("Fallback RESP dropped due to seq mismatch, recv_seq=%u expect_seq=%u\n",
                req_seq, task->fallback_task.req_seq);
        }
    }
    //pthread_rwlock_unlock(&health->task_table.lock);
    //
    return true;
}
