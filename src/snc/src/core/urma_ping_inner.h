/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: SNC (Supernode Network Controller) service
 * Create: 2026-07-13
 */
#ifndef URMA_PING_INNER_H
#define URMA_PING_INNER_H

#include "urma_ping.h"
#include <map>
#include <string>
#include <vector>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <memory>
#include "acl/acl.h"
#include "hccl/hccn_rping.h"

#define PKT_NUM 10
#define PING_PORT 13886
#define PING_NPU_NUM 128
#define PING_BUFFER_SIZE_PER_PKT 2048
#define PING_PAGE_SIZE 4096
#define PING_INTERVAL 1
#define PING_TIMEOUT 100
#define PING_GET_RESULT_MAX_RETRIES 120
#define PING_CALLER_WAIT_TIMEOUT_SEC 60
#define PING_QUEUE_WAIT_TIMEOUT_SEC 1

typedef struct{
    char eid[IP_LEN];
    HccnRpingCtx rpingCtx;
    bool isInited;
}UrmaPingEidCtx;

typedef struct{
    int clientDevId;
    char clientEid[IP_LEN];
    char serverEid[IP_LEN];
    UrmaPingResult* result;
    std::atomic<bool> done;
    std::atomic<bool> canceled;
    std::atomic<int> ret;
    std::mutex cvMutex;
    std::condition_variable cv;
}UrmaPingTask;

typedef struct{
    int devId;
    std::vector<UrmaPingEidCtx> eidCtxs;
    std::atomic<bool> isStop;
    std::thread* workerThread;
    std::mutex eidMutex;
    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::queue<std::shared_ptr<UrmaPingTask>> taskQueue;
}UrmaPingDevThread;

#endif
