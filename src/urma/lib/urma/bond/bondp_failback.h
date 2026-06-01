//
// Created by w00906336 on 2026/5/30.
//

#ifndef BOUNDP_FAILBACK_H
#define BOUNDP_FAILBACK_H

#include "bondp_worker.h"
#include "bondp_types.h"


#define BONDP_FO_REBUILD_START_DELAY_MS     10000U  /* 收到 CQE 后等待 10s 再 rebuild */
#define BONDP_FO_REBUILD_RETRY_INIT_MS     1000U   /* rebuild 失败后 1s 再试 */
#define BONDP_FO_REBUILD_RETRY_MAX_MS     10000U  /* rebuild 最多每10s 尝试一次*/
#define BONDP_FO_SEND_REQ_INIT_MS       10U     /* 发 REQ 初始重试间隔 */
#define BONDP_FO_SEND_REQ_MAX_MS        500U    /* 发 REQ 最大重试间隔 */
#define BONDP_FO_REQ_RSP_TIMEOUT_INIT_MS         1000U   /* 发出 REQ 后等待 RSP 的初始超时时间 */
#define BONDP_FO_REQ_RSP_TIMEOUT_MAX_MS         10000U   /* 发出 REQ 后等待 RSP 的初始超时时间 */

#endif
