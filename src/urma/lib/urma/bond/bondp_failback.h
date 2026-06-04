//
// Created by w00906336 on 2026/5/30.
//

#ifndef BOUNDP_FAILBACK_H
#define BOUNDP_FAILBACK_H

#include "bondp_worker.h"
#include "bondp_types.h"

#define BONDP_FB_OK                     (0)
#define BONDP_FB_ERR                    (-1)

typedef struct bondp_failover_sub_task {
    int local_idx;
    int target_idx;
    bool valid;
    bool probe_pending;
    bool need_check;
#ifndef __cplusplus
    atomic_bool link_ok;
#else
    std::atomic_bool link_ok;
#endif
    uint64_t user_ctx;
} bondp_failover_sub_task_t;

typedef enum bondp_failback_state {
    BONDP_FAILBACK_STATE_IDLE = 0,
    BONDP_FAILBACK_STATE_WAIT_REBUILD_START,
    BONDP_FAILBACK_STATE_REBUILDING,
    BONDP_FAILBACK_SENDING_REQ,
    BONDP_FAILBACK_STATE_WAIT_RSP,
    BONDP_FAILBACK_STATE_DONE,
    BONDP_FAILBACK_STATE_CLOSED,
} bondp_failback_state_t;

typedef struct bondp_failback_ctx {
    uint64_t timer_id;

    bondp_failback_state_t state;
    uint64_t req_seq;
    uint32_t remote_primary_pjetty_id;
    uint32_t primary_target_idx;

    bool timer_valid;
    uint32_t retry_ms;

    struct bondp_target_jetty *bdp_tjetty;
    struct bondp_comp *bondp_jetty;

    int primary_local_idx;
    int active_local_idx;
    bondp_health_mode_t mode;
    uint32_t backoff_cnt;
    bondp_fallback_task_t fallback_task;
    uint32_t vjetty_id;
    bondp_failover_sub_task_t sub_tasks[URMA_UBAGG_DEV_MAX_NUM][URMA_UBAGG_DEV_MAX_NUM];

    hmap_node_t hmap_node;

    bondp_context_t *bdp_ctx;

} bondp_failback_task_t;

#define BONDP_FAILBACK_REBUILD_START_DELAY_MS     10000U  /* 收到CQE后，延迟10秒开始重建 */
#define BONDP_FAILBACK_REBUILD_RETRY_INIT_MS      1000U   /* 重建失败后，初始重试间隔1秒 */
#define BONDP_FAILBACK_REBUILD_RETRY_MAX_MS       10000U  /* 重建失败后，最大重试间隔10秒 */
#define BONDP_FAILBACK_SEND_REQ_INIT_MS           10U     /* 发送请求失败后，初始重试间隔10毫秒 */
#define BONDP_FAILBACK_SEND_REQ_MAX_MS            500U    /* 发送请求失败后，最大重试间隔500毫秒 */
#define BONDP_FAILBACK_REQ_RSP_TIMEOUT_INIT_MS    1000U   /* 等待请求响应的初始超时时间1秒 */
#define BONDP_FAILBACK_REQ_RSP_TIMEOUT_MAX_MS     10000U  /* 等待请求响应的最大超时时间10秒 */
#define BONDP_FAILBACK_CTRL_REQ                        1
#define BONDP_FAILBACK_CTRL_RESP                       2
#define BONDP_FAILBACK_PRIMARY_INVALID_ID              UINT32_MAX

int bondp_failback_on_cqe(bondp_failback_task_t *ctx);

int bondp_handle_netlink_events(void);

#endif
