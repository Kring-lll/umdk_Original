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
#include "acl/acl.h"
#include "hccl/hccn_rping.h"

#define PKT_NUM 10

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
    std::queue<UrmaPingTask*> taskQueue;
}UrmaPingDevThread;

#endif