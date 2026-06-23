/*
* SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bond provider failback netlink helpers.
 */
#include <errno.h>
#include <netlink/genl/genl.h>
#include <netlink/msg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bondp_failback.h"
#include "bondp_failback_ops.h"
#include "bondp_netlink.h"
#include "bondp_types.h"
#include "ub_hmap.h"
#include "ubagg_ioctl.h"
#include "bondp_context_table.h"
#include "urma_api.h"
#include "urma_log.h"
#include "urma_provider.h"
#include "urma_types.h"
#include "ub_hash.h"

#define BONDP_FAILBACK_TASK_HASH_BASIS 0x9d4f21U
#define BONDP_FAILBACK_TASK_TABLE_SIZE 1024U

static struct ub_hmap g_task_map;
static pthread_rwlock_t g_task_lock = PTHREAD_RWLOCK_INITIALIZER;
static atomic_uint g_failback_request_id = 0;

static bondp_hash_table_t failback_task_table;

//全局静态
static void bondp_failback_rebuild_task(void *arg);
static void bondp_failback_send_next_req_task(void *arg);
static void bondp_failback_task_destroy(bondp_failback_task_t *task);
static void bondp_failback_cancel_timer(bondp_failback_task_t *task);
static bondp_failback_task_t *bondp_failback_task_table_lookup(
    const urma_eid_t *src_eid, uint32_t vjetty_id, uint32_t pjetty_idx);

//request辅助函数
static uint32_t bondp_failback_next_request_id(void)
{
    return atomic_fetch_add(&g_failback_request_id, 1) + 1;
}

static void bondp_failback_init_request_id(void)
{
    struct timespec ts = {0};
    uint32_t seed;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        seed = (uint32_t)getpid();
    } else {
        seed = (uint32_t)ts.tv_nsec ^ (uint32_t)ts.tv_sec ^ (uint32_t)getpid();
    }

    atomic_store(&g_failback_request_id, seed);
}


//hash表三函数
static uint32_t bondp_failback_task_hash_f(void *key)
{
    return ub_hash_bytes(key, sizeof(bondp_failback_task_key_t), BONDP_FAILBACK_TASK_HASH_BASIS);
}

static bool bondp_failback_task_comp_f(hmap_node_t *node, void *key)
{
    bondp_failback_task_t *task = CONTAINER_OF_FIELD(node, bondp_failback_task_t, hmap_node);
    bondp_failback_task_key_t *k = (bondp_failback_task_key_t *)key;
    return memcmp(&task->src_eid, &k->src_eid, sizeof(urma_eid_t)) == 0 &&
           (task->vjetty_id == k->vjetty_id) && (task->pjetty_idx == k->pjetty_idx);
}

static void bondp_failback_task_free_f(hmap_node_t *node)
{
    bondp_failback_task_t *task = CONTAINER_OF_FIELD(node, bondp_failback_task_t, hmap_node);
    (void)node;
}

static int bondp_failback_task_table_create(void)
{
    return ub_hmap_init(&g_task_map, BONDP_FAILBACK_TASK_TABLE_SIZE);
}


static void bondp_failback_task_free(bondp_fb_task_t *task)
{
    free(task);
}


void bondp_failback_task_table_init(void)
{
    bondp_failback_init_request_id();
    (void)bondp_hash_table_create(&failback_task_table,
        BONDP_FAILBACK_TASK_TABLE_SIZE,
        bondp_failback_task_comp_f,
        bondp_failback_task_free_f,
        bondp_failback_task_hash_f);
}

static bondp_failback_task_t *bondp_failback_task_table_lookup(
    const urma_eid_t *src_eid, uint32_t vjetty_id, uint32_t pjetty_idx)
{
    bondp_failback_task_key_t key = {0};
    if (src_eid != NULL) {
        key.src_eid = *src_eid;
    }
    key.vjetty_id = vjetty_id;
    key.pjetty_idx = pjetty_idx;
    uint32_t hash = bondp_failback_task_hash_f(&key);
    hmap_node_t *node = bondp_hash_table_lookup_without_lock(&failback_task_table, &key, hash);
    if (node == NULL) {
        return NULL;
    }
    bondp_failback_task_t *task = CONTAINER_OF_FIELD(node, bondp_failback_task_t, hmap_node);
    (void)atomic_fetch_add(&task->use_cnt.atomic_cnt, 1);
    return task;
}

static int bondp_failback_task_table_add(bondp_failback_task_t *task)
{
    bondp_failback_task_key_t key = {0};
    key.src_eid = task->src_eid;
    key.vjetty_id = task->vjetty_id;
    key.pjetty_idx = task->pjetty_idx;
    uint32_t hash = bondp_failback_task_hash_f(&key);
    return bondp_hash_table_add_with_hash_without_lock(&failback_task_table, &task->hmap_node, hash);
}

//TODO: 当前改到这里
static void bondp_failback_task_table_remove(bondp_failback_task_t *task)
{
    bondp_hash_table_remove_without_lock(&failback_task_table, &task->hmap_node);
}

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
    int failed_local_idx,
    const urma_eid_t *src_eid)
{
    bondp_failback_task_t *task = (bondp_failback_task_t *)calloc(1, sizeof(bondp_failback_task_t));
    if (task == NULL) {
        return NULL;
    }
    // --- worker 调度 ---
    task->timer_id = 0;
    task->timer_valid = false;
    task->closed = false;
    atomic_init(&task->use_cnt.atomic_cnt, 1);

    // --- 状态机 (IDLE, 等待 on_cqe 触发) ---
    task->state = BONDP_FAILBACK_STATE_IDLE;
    task->req_seq = 0;
    task->retry_ms = 0;
    task->request_id = bondp_failback_next_request_id();

    // --- 身份标识 ---
    task->bdp_ctx = bdp_ctx;
    task->bondp_jetty = bondp_jetty;
    if (src_eid != NULL) {
        task->src_eid = *src_eid;
    }
    task->vjetty_id = bondp_jetty->v_jetty.jetty_id.id;
    task->pjetty_idx = (uint32_t)failed_local_idx;

    // --- 路径信息 ---
    task->target_idx = -1;
    task->new_pjetty = NULL;
    task->new_pjetty_id = 0;

    // --- 广播通知跟踪 ---
    //node_count 从 bdp_ctx 获取实际 NODE 数量
    task->node_count = BOUNDP_FAILBACK_NODE_MAX_NUM;
    task->notified_count = 0;
    task->next_send_idx = 0;
    return task;
}

static void bondp_failback_task_free(bondp_failback_task_t *task)
{
    if (task == NULL) {
        return;
    }
    if (task->state != BONDP_FAILBACK_STATE_DONE && task->new_pjetty != NULL) {
        (void)urma_delete_jetty(task->new_pjetty);
        task->new_pjetty = NULL;
    }
    if (task->bdp_ctx != NULL) {
        bondp_failback_task_table_remove(task);
    }
    task->closed = true;
    free(task);
}

static void bondp_failback_task_put(bondp_failback_task_t *task)
{
    if (task == NULL) {
        return;
    }
    (void)pthread_rwlock_wrlock(&g_task_lock);
    unsigned long use_cnt = atomic_fetch_sub(&task->use_cnt.atomic_cnt, 1);
    (void)pthread_rwlock_unlock(&g_task_lock);
    if (use_cnt == 1) {
        bondp_failback_task_free(task);
    }
}

static void bondp_failback_task_destroy(bondp_failback_task_t *task)
{
    if (task == NULL) {
        return;
    }

    if (task->closed) {
        return;
    }

    (void)pthread_rwlock_wrlock(&g_task_lock);
    HMAP_FOR_EACH_SAFE (task, next, hmap_node, &g_task_map) {
        ub_hmap_remove(&g_task_map, &task->hmap_node);
        bondp_failback_task_free(task);
    }
    ub_hmap_destroy(&g_task_map);
    (void)pthread_rwlock_unlock(&g_task_lock);

    bondp_failback_cancel_timer(task);
    bondp_failback_task_put(task);
}

static void bondp_fb_task_table_destroy(void)
{
    bondp_fb_task_t *fb_task = NULL;
    bondp_fb_task_t *next = NULL;

    (void)pthread_rwlock_wrlock(&g_task_lock);
    HMAP_FOR_EACH_SAFE (fb_task, next, hmap_node, &g_task_map) {
        ub_hmap_remove(&g_task_map, &fb_task->hmap_node);
        bondp_fb_task_free(fb_task);
    }
    ub_hmap_destroy(&g_task_map);
    (void)pthread_rwlock_unlock(&g_task_lock);
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

int bondp_failback_on_cqe(struct bondp_comp *bondp_jetty, int failed_local_idx, const urma_eid_t *src_eid)
{
    if (failed_local_idx < 0 || failed_local_idx >= URMA_UBAGG_DEV_MAX_NUM) {
        return BONDP_FB_ERR;
    }

    bondp_context_t *bdp_ctx = bondp_jetty->bondp_ctx;
    uint32_t vjetty_id = bondp_jetty->v_jetty.jetty_id.id;
    bondp_failback_task_t *existing = bondp_failback_task_table_lookup(src_eid, vjetty_id, (uint32_t)failed_local_idx);
    if (existing != NULL) {
        URMA_LOG_WARN("Failback task already exists for vjetty=%u idx=%d, skip\n",
            vjetty_id, failed_local_idx);
        bondp_failback_task_put(existing);
        return BONDP_FB_OK;
    }

    bondp_failback_task_t *task = bondp_failback_task_create(bdp_ctx, bondp_jetty, failed_local_idx, src_eid);
    if (task == NULL) {
        return TW_NO_MEMORY_ERROR;
    }

    //初始化 node_count 从 bdp_ctx 获取实际 NODE 数量/标号  标记不需要使用的node_id   先写死
    // task->node_count = bdp_ctx->node_count;
    for (uint32_t i = 0; i < task->node_count; i++) {
        task->node_states[i] = BONDP_FAILBACK_NODE_PENDING;
    }

    // 注册 task 到 failback_task_table
    bondp_failback_task_table_add(task);

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
        URMA_LOG_WARN("Failback REQ send failed, node[%u] vjetty=%u pjetty_idx=%d new_pjetty_id=%u\n",
            idx, task->vjetty_id, task->pjetty_idx, task->new_pjetty_id);
    }
    task->next_send_idx = idx + 1;
    (void)bondp_failback_schedule(task, BONDP_FAILBACK_SEND_INTERVAL_MS, bondp_failback_send_next_req_task);

}

// ========== 消息分发 ==========

/*
 * 处理收到的 netlink 消息 (REQ / RESP)
 *
 * 仅做 ctrl_type 分流, 具体业务逻辑在 bondp_failback_ops.c 中:
 *   REQ  → bondp_failback_handle_req()
 *   RESP → bondp_failback_handle_resp()
 */
static bool bondp_handle_message(const bondp_failback_msg_t *msg, bool silent_unmatched)
{
    uint8_t ctrl_type = msg->in.ctrl_type;
    uint32_t vjetty_id = msg->in.vjetty_id;
    uint32_t node_id = msg->in.node_id;

    if (ctrl_type != BONDP_FAILBACK_CTRL_REQ && ctrl_type != BONDP_FAILBACK_CTRL_RESP) {
        if (!silent_unmatched) {
            URMA_LOG_WARN("Invalid fallback ctrl type=%u vjetty_id=%u\n", ctrl_type, vjetty_id);
        }
        return false;
    }

    if (ctrl_type == BONDP_FAILBACK_CTRL_REQ) {
        URMA_LOG_INFO("Fallback REQ received, vjetty_id=%u pjetty_idx=%u pjetty_new_id=%u node_id=%u seq=%u\n",
            msg->in.vjetty_id, msg->in.pjetty_idx, msg->in.pjetty_new_id, msg->in.node_id, msg->in.req_seq);

        int ret = bondp_failback_handle_req(msg);
        return (ret == BONDP_FB_OK);
    }

    if (ctrl_type == BONDP_FAILBACK_CTRL_RESP) {
        URMA_LOG_INFO("Fallback RESP received, vjetty_id=%u pjetty_idx=%u pjetty_new_id=%u node_id=%u seq=%u\n",
            msg->in.vjetty_id, msg->in.pjetty_idx, msg->in.pjetty_new_id, msg->in.node_id, msg->in.req_seq);

        //TODO: src_eid 应从消息中获取, 当前消息格式未携带 src_eid, 暂传 NULL
        bondp_failback_task_t *task = bondp_failback_task_table_lookup(NULL, vjetty_id, msg->in.pjetty_idx);
        if (task == NULL || task->closed) {
            URMA_LOG_WARN("Fallback RESP: no active task for vjetty_id=%u pjetty_idx=%u\n",
                vjetty_id, msg->in.pjetty_idx);
            return false;
        }

        int ret = bondp_failback_handle_node_notify(task, node_id);
        if (task->node_count == task->notified_count) {
            bondp_failback_destroy_old_jetty(task);
            task->bondp_jetty->failback_rebuilt[task->pjetty_idx] = true;
            bondp_failback_task_destroy(task);
        } else {
            bondp_failback_task_put(task);
        }

        return (ret == BONDP_FB_OK);
    }

    return true;
}
