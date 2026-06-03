//
// Created by w00906336 on 2026/5/30.
//

#ifndef BOUNDP_FAILBACK_H
#define BOUNDP_FAILBACK_H

#include "bondp_worker.h"
#include "bondp_types.h"


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

int bondp_failback_on_cqe(bondp_failback_ctx_t *ctx);

int bondp_handle_netlink_events(void);

#endif
