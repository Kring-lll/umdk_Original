//
// Created by w00906336 on 2026/5/30.
//

#ifndef BOUNDP_FAILBACK_H
#define BOUNDP_FAILBACK_H

#include "bondp_worker.h"
#include "bondp_types.h"
#include "bondp_hash_table.h"

#define BONDP_FB_OK                     (0)
#define BONDP_FB_ERR                    (-1)

//TODO: 对接一个合适的宏
#ifndef BOUNDP_FAILBACK_NODE_MAX_NUM
#define BOUNDP_FAILBACK_NODE_MAX_NUM         512
#endif

#define BONDP_FAILBACK_TASK_TABLE_SIZE       64

typedef enum bondp_failback_state {
    BONDP_FAILBACK_STATE_IDLE = 0,
    BONDP_FAILBACK_STATE_WAIT_REBUILD_START,
    BONDP_FAILBACK_STATE_REBUILDING,
    BONDP_FAILBACK_SENDING_REQ,
    BONDP_FAILBACK_STATE_WAIT_RSP,
    BONDP_FAILBACK_STATE_DONE,
    BONDP_FAILBACK_STATE_CLOSED,
} bondp_failback_state_t;

// NODE 通知状态 (node_states[] 数组值)
#define BONDP_FAILBACK_NODE_PENDING    0
#define BONDP_FAILBACK_NODE_SATTLED   1

typedef struct bondp_failback_task_key {
    uint32_t vjetty_id;
    uint32_t pjetty_idx;
} bondp_failback_task_key_t;

typedef struct bondp_failback_task {
    // --- worker 调度 并发安全 ---
    uint64_t timer_id;
    bool timer_valid;
    bool closed;

    // --- 状态机 ---
    bondp_failback_state_t state;
    uint64_t req_seq;
    uint32_t retry_ms;

    // --- 身份标识 ---
    bondp_context_t *bdp_ctx;
    struct bondp_comp *bondp_jetty;
    uint32_t vjetty_id;
    uint32_t pjetty_idx;

    // --- 路径信息 ---
    int target_idx;
    urma_jetty_t *new_pjetty;
    uint32_t new_pjetty_id;

    // --- 广播通知跟踪 ---
    uint32_t node_count;
    uint32_t notified_count;
    uint32_t next_send_idx;
    uint8_t node_states[BOUNDP_FAILBACK_NODE_MAX_NUM];

    // --- 哈希表挂载 ---
    hmap_node_t hmap_node;
} bondp_failback_task_t;


#define BONDP_FAILBACK_REBUILD_START_DELAY_MS     10000U  /* 收到CQE后，延迟10秒开始重建 */
#define BONDP_FAILBACK_REBUILD_RETRY_INIT_MS      1000U   /* 重建失败后，初始重试间隔1秒 */
#define BONDP_FAILBACK_REBUILD_RETRY_MAX_MS       10000U  /* 重建失败后，最大重试间隔10秒 */
#define BONDP_FAILBACK_SEND_REQ_INIT_MS           10U     /* 发送请求失败后，初始重试间隔10毫秒 */
#define BONDP_FAILBACK_SEND_REQ_MAX_MS            500U    /* 发送请求失败后，最大重试间隔500毫秒 */
#define BONDP_FAILBACK_REQ_RSP_TIMEOUT_INIT_MS    1000U   /* 等待请求响应的初始超时时间1秒 */
#define BONDP_FAILBACK_REQ_RSP_TIMEOUT_MAX_MS     10000U  /* 等待请求响应的最大超时时间10秒 */

// 广播发送节流: 两条 REQ 之间的间隔
#define BONDP_FAILBACK_SEND_INTERVAL_MS           5U      /* 5ms 间隔, 512 NODE × 5ms ≈ 2.56s 发完一轮 */
// 广播重试退避: 一轮发完后, 未响应 NODE 的重试间隔
#define BONDP_FAILBACK_BROADCAST_RETRY_INIT_MS    1000U   /* 首次重试间隔1秒 */
#define BONDP_FAILBACK_BROADCAST_RETRY_MAX_MS     60000U  /* 最大重试间隔60秒, 无限重试 */

#define BONDP_FAILBACK_CTRL_REQ                        1
#define BONDP_FAILBACK_CTRL_RESP                       2
#define BONDP_FAILBACK_PRIMARY_INVALID_ID              UINT32_MAX

int bondp_failback_on_cqe(bondp_context_t *bdp_ctx,
    struct bondp_comp *bondp_jetty,
    int failed_local_idx);

void bondp_failback_task_table_init(bondp_context_t *bdp_ctx);

int bondp_handle_netlink_events(void);

#endif
