/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: SNC (Supernode Network Controller) service
 * Create: 2026-07-13
 */
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "urma_ping_inner.h"

static std::mutex g_devMutex;
static std::map<int, std::shared_ptr<UrmaPingDevThread>> g_devThread;

static std::shared_ptr<UrmaPingDevThread> FindDevThread(int devId)
{
    std::lock_guard<std::mutex> lock(g_devMutex);
    auto it = g_devThread.find(devId);
    if (it == g_devThread.end()) {
        return nullptr;
    }
    return it->second;
}

static UrmaPingEidCtx* FindEidCtx(UrmaPingDevThread* devThread, const char* eid)
{
    std::lock_guard<std::mutex> lock(devThread->eidMutex);
    for (auto& eidCtx : devThread->eidCtxs) {
        if (strcmp(eidCtx.eid, eid) == 0) {
            return &eidCtx;
        }
    }
    return nullptr;
}

struct PingContext {
    char srcEidBuf[IP_LEN];
    char dstEidBuf[IP_LEN];
    HccnRpingTargetInfo target;
    HccnRpingResultInfo hccnResult;
};

static void PreparePingTarget(UrmaPingTask* task, PingContext* ctx)
{
    memset(&ctx->target, 0, sizeof(ctx->target));
    ctx->target.srcPort = 0;
    ctx->target.sl = 0;
    ctx->target.tc = 0;
    ctx->target.port = PING_PORT;
    ctx->target.addrType = HCCN_RPING_ADDR_TYPE_EID;

    memset(ctx->srcEidBuf, 0, IP_LEN);
    memset(ctx->dstEidBuf, 0, IP_LEN);
    ctx->target.srcEid = ctx->srcEidBuf;
    ctx->target.dstEid = ctx->dstEidBuf;
    strncpy(ctx->target.srcEid, task->clientEid, IP_LEN - 1);
    ctx->target.srcEid[IP_LEN - 1] = '\0';
    strncpy(ctx->target.dstEid, task->serverEid, IP_LEN - 1);
    ctx->target.dstEid[IP_LEN - 1] = '\0';

    static const char payload[] = "hellotarget";
    strncpy(ctx->target.payload, payload, sizeof(ctx->target.payload) - 1);
    ctx->target.payload[sizeof(ctx->target.payload) - 1] = '\0';
    ctx->target.payloadLen = (uint32_t)(strlen(payload) + 1);

    memset(&ctx->hccnResult, 0, sizeof(ctx->hccnResult));
    printf("DoPing: start ping %s -> %s\n", task->clientEid, task->serverEid);
}

static int StartPingSession(UrmaPingEidCtx* eidCtx, PingContext* ctx)
{
    HccnResult ret = HccnRpingAddTarget(eidCtx->rpingCtx, 1, &ctx->target);
    if (ret != HCCN_SUCCESS) {
        printf("DoPing: add target failed, ret=%d\n", (int)ret);
        return -1;
    }

    ret = HccnRpingBatchPingStart(eidCtx->rpingCtx, (uint32_t)PKT_NUM,
        (uint32_t)PING_INTERVAL, (uint32_t)PING_TIMEOUT);
    if (ret != HCCN_SUCCESS) {
        HccnRpingRemoveTarget(eidCtx->rpingCtx, 1, &ctx->target);
        printf("DoPing: ping process failed, ret=%d\n", (int)ret);
        return -1;
    }
    return 0;
}

static int CollectPingResult(UrmaPingEidCtx* eidCtx, PingContext* ctx)
{
    HccnResult hccnRet = HCCN_E_AGAIN;
    int retries = 0;
    while (hccnRet == HCCN_E_AGAIN && retries < PING_GET_RESULT_MAX_RETRIES) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        hccnRet = HccnRpingGetResult(eidCtx->rpingCtx, 1, &ctx->target, &ctx->hccnResult);
        retries++;
    }
    if (hccnRet != HCCN_SUCCESS) {
        HccnRpingBatchPingStop(eidCtx->rpingCtx);
        HccnRpingRemoveTarget(eidCtx->rpingCtx, 1, &ctx->target);
        printf("DoPing: get result failed (retries=%d, ret=%d)\n", retries, (int)hccnRet);
        return -1;
    }
    return 0;
}

static void FillPingResult(UrmaPingTask* task, const HccnRpingResultInfo* hccnResult)
{
    std::lock_guard<std::mutex> resLock(task->cvMutex);
    if (task->canceled.load()) {
        printf("DoPing: task canceled by caller before filling result, skip filling\n");
    } else if (task->result != nullptr) {
        task->result->txPkt = hccnResult->txPkt;
        task->result->rxPkt = hccnResult->rxPkt;
        task->result->minRTT = hccnResult->minRTT;
        task->result->maxRTT = hccnResult->maxRTT;
        task->result->avgRTT = hccnResult->avgRTT;
        task->result->state = (uint32_t)hccnResult->state;

        printf("DoPing: txPkt[%u] rxPkt[%u] minRTT[%u] maxRTT[%u] avgRTT[%u] state[%u]\n",
            task->result->txPkt, task->result->rxPkt, task->result->minRTT,
            task->result->maxRTT, task->result->avgRTT, task->result->state);
    } else {
        printf("DoPing: result pointer is null, skip filling\n");
    }
}

static int StopAndRemoveTarget(UrmaPingEidCtx* eidCtx, PingContext* ctx)
{
    HccnResult ret = HccnRpingBatchPingStop(eidCtx->rpingCtx);
    if (ret != HCCN_SUCCESS) {
        HccnRpingRemoveTarget(eidCtx->rpingCtx, 1, &ctx->target);
        printf("DoPing: stop ping failed, ret=%d\n", (int)ret);
        return -1;
    }

    ret = HccnRpingRemoveTarget(eidCtx->rpingCtx, 1, &ctx->target);
    if (ret != HCCN_SUCCESS) {
        printf("DoPing: remove target failed, ret=%d\n", (int)ret);
        return -1;
    }
    return 0;
}

static int DoPing(UrmaPingDevThread* devThread, UrmaPingEidCtx* eidCtx, UrmaPingTask* task)
{
    (void)devThread;
    PingContext ctx;
    PreparePingTarget(task, &ctx);

    if (StartPingSession(eidCtx, &ctx) != 0) {
        return -1;
    }
    if (CollectPingResult(eidCtx, &ctx) != 0) {
        return -1;
    }

    FillPingResult(task, &ctx.hccnResult);
    if (StopAndRemoveTarget(eidCtx, &ctx) != 0) {
        return -1;
    }
    return task->canceled.load() ? -1 : 0;
}

static void InitEidCtxs(UrmaPingDevThread* devThread)
{
    std::lock_guard<std::mutex> lock(devThread->eidMutex);
    for (auto& eidCtx : devThread->eidCtxs) {
        HccnRpingInitAttr initAttr;
        memset(&initAttr, 0, sizeof(initAttr));
        initAttr.mode = HCCN_RPING_MODE_UB_RC;
        initAttr.port = PING_PORT;
        initAttr.npuNum = PING_NPU_NUM;
        size_t needSize = (size_t)PKT_NUM * PING_BUFFER_SIZE_PER_PKT;
        initAttr.bufferSize =
            (uint32_t)(((needSize + PING_PAGE_SIZE - 1) / PING_PAGE_SIZE + 1) * PING_PAGE_SIZE);
        char eidBuf[IP_LEN] = {0};
        strncpy(eidBuf, eidCtx.eid, IP_LEN - 1);
        eidBuf[IP_LEN - 1] = '\0';
        initAttr.eid = eidBuf;
        HccnRpingCtx rpingCtx = nullptr;
        HccnResult ret = HccnRpingInit((uint32_t)devThread->devId, &initAttr, &rpingCtx);
        if (ret != HCCN_SUCCESS) {
            printf("device[%d,%s] init failed, ret=%d\n", devThread->devId, eidCtx.eid, (int)ret);
            continue;
        }
        eidCtx.rpingCtx = rpingCtx;
        eidCtx.isInited = true;
        printf("device[%d,%s] init success, listening...\n", devThread->devId, eidCtx.eid);
    }
}

static void DeinitEidCtxs(UrmaPingDevThread* devThread)
{
    std::lock_guard<std::mutex> lock(devThread->eidMutex);
    for (auto& eidCtx : devThread->eidCtxs) {
        if (eidCtx.isInited) {
            HccnResult ret = HccnRpingDeinit(eidCtx.rpingCtx);
            if (ret != HCCN_SUCCESS) {
                printf("device[%d,%s] deinit failed, ret=%d\n", devThread->devId, eidCtx.eid, (int)ret);
            } else {
                printf("device[%d,%s] deinit success\n", devThread->devId, eidCtx.eid);
            }
            eidCtx.isInited = false;
        }
    }
}

static std::shared_ptr<UrmaPingTask> PopTaskFromQueue(UrmaPingDevThread* devThread)
{
    std::shared_ptr<UrmaPingTask> task;
    std::unique_lock<std::mutex> lock(devThread->queueMutex);
    devThread->queueCv.wait_for(lock, std::chrono::seconds(PING_QUEUE_WAIT_TIMEOUT_SEC),
        [devThread]() {
            return !devThread->taskQueue.empty() || devThread->isStop.load();
        });
    if (!devThread->taskQueue.empty()) {
        task = devThread->taskQueue.front();
        devThread->taskQueue.pop();
    }
    return task;
}

static void ProcessTask(UrmaPingDevThread* devThread, const std::shared_ptr<UrmaPingTask>& task)
{
    if (task->canceled.load()) {
        printf("WorkerThread: task already canceled, skip processing for devId=%d, eid=%s\n",
            task->clientDevId, task->clientEid);
    } else {
        UrmaPingEidCtx* eidCtx = FindEidCtx(devThread, task->clientEid);
        if (eidCtx == nullptr || !eidCtx->isInited) {
            printf("WorkerThread: eidCtx not found or not inited for devId=%d, eid=%s\n",
                task->clientDevId, task->clientEid);
            task->ret.store(-1);
        } else {
            int doPingRet = DoPing(devThread, eidCtx, task.get());
            task->ret.store(doPingRet);
        }
    }
    task->done.store(true);
    task->cv.notify_one();
}

static void WorkerThreadFunc(UrmaPingDevThread* devThread)
{
    aclError setDevRet = aclrtSetDevice(devThread->devId);
    if (setDevRet != ACL_SUCCESS) {
        printf("WorkerThread: aclrtSetDevice failed for devId=%d, ret=%d, eidCtx init will likely fail\n",
            devThread->devId, (int)setDevRet);
    }

    InitEidCtxs(devThread);
    while (devThread->isStop.load() == false) {
        std::shared_ptr<UrmaPingTask> task = PopTaskFromQueue(devThread);
        if (!task) {
            continue;
        }
        ProcessTask(devThread, task);
    }

    DeinitEidCtxs(devThread);
}

int UrmaPingInit(UrmaPingEntity* entities, int count)
{
    if (entities == nullptr) {
        printf("UrmaPingInit: invalid input, entities is null\n");
        return -1;
    }
    if (count <= 0) {
        printf("UrmaPingInit: invalid input, count=%d (must be > 0)\n", count);
        return -1;
    }

    std::lock_guard<std::mutex> lock(g_devMutex);

    std::map<int, std::vector<UrmaPingEntity>> devToEids;
    for (int i = 0; i < count; i++) {
        devToEids[entities[i].devId].push_back(entities[i]);
    }

    for (auto& entry : devToEids) {
        int devId = entry.first;
        if (g_devThread.find(devId) != g_devThread.end()) {
            printf("UrmaPingInit: device[%d] already inited, skipping %zu new eids\n",
                devId, entry.second.size());
            continue;
        }
        auto devThread = std::make_shared<UrmaPingDevThread>();
        devThread->devId = devId;
        devThread->isStop.store(false);
        devThread->workerThread = nullptr;

        for (auto& entity : entry.second) {
            UrmaPingEidCtx eidCtx;
            memset(&eidCtx, 0, sizeof(eidCtx));
            strncpy(eidCtx.eid, entity.eid, IP_LEN - 1);
            eidCtx.eid[IP_LEN - 1] = '\0';
            devThread->eidCtxs.push_back(eidCtx);
        }

        devThread->workerThread = new std::thread(WorkerThreadFunc, devThread.get());
        g_devThread[devId] = devThread;
        printf("UrmaPingInit: added devThread for device[%d] with %zu eids\n",
            devId, entry.second.size());
    }

    printf("UrmaPingInit: total %zu devThreads\n", g_devThread.size());
    return 0;
}

static std::shared_ptr<UrmaPingTask> BuildPingTask(int clientDevId, const char* clientEid,
    const char* serverEid, UrmaPingResult* result)
{
    auto task = std::make_shared<UrmaPingTask>();
    task->clientDevId = clientDevId;
    memset(task->clientEid, 0, IP_LEN);
    memset(task->serverEid, 0, IP_LEN);
    if (clientEid != nullptr) {
        strncpy(task->clientEid, clientEid, IP_LEN - 1);
        task->clientEid[IP_LEN - 1] = '\0';
    }
    if (serverEid != nullptr) {
        strncpy(task->serverEid, serverEid, IP_LEN - 1);
        task->serverEid[IP_LEN - 1] = '\0';
    }
    task->result = result;
    task->done.store(false);
    task->canceled.store(false);
    task->ret.store(0);
    return task;
}

static int EnqueueTask(const std::shared_ptr<UrmaPingDevThread>& devThread,
    const std::shared_ptr<UrmaPingTask>& task, int clientDevId)
{
    std::lock_guard<std::mutex> lock(devThread->queueMutex);
    if (devThread->isStop.load()) {
        printf("UrmaPingOne: devThread is being torn down for devId=%d, reject new task\n",
            clientDevId);
        return -1;
    }
    devThread->taskQueue.push(task);
    devThread->queueCv.notify_one();
    return 0;
}

static void WaitForTaskCompletion(const std::shared_ptr<UrmaPingTask>& task,
    int clientDevId, const char* clientEid, const char* serverEid)
{
    std::unique_lock<std::mutex> lock(task->cvMutex);
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(PING_CALLER_WAIT_TIMEOUT_SEC);
    while (!task->done.load() && !task->canceled.load()) {
        if (task->cv.wait_until(lock, deadline) != std::cv_status::timeout) {
            continue;
        }
        if (task->done.load() || task->canceled.load()) {
            break;
        }
        task->canceled.store(true);
        printf("UrmaPingOne: task timed out after %d sec, devId=%d, clientEid=%s, serverEid=%s\n",
            PING_CALLER_WAIT_TIMEOUT_SEC, clientDevId,
            clientEid ? clientEid : "(null)",
            serverEid ? serverEid : "(null)");
        break;
    }
}

int UrmaPingOne(int clientDevId, const char* clientEid, const char* serverEid, UrmaPingResult* result)
{
    std::shared_ptr<UrmaPingDevThread> devThread = FindDevThread(clientDevId);
    if (devThread == nullptr) {
        printf("UrmaPingOne: devThread not found for devId=%d\n", clientDevId);
        return -1;
    }
    auto task = BuildPingTask(clientDevId, clientEid, serverEid, result);
    if (EnqueueTask(devThread, task, clientDevId) != 0) {
        return -1;
    }
    WaitForTaskCompletion(task, clientDevId, clientEid, serverEid);
    if (task->canceled.load()) {
        printf("UrmaPingOne: task canceled for devId=%d\n", clientDevId);
        return -1;
    }
    return task->ret.load();
}

static void StopWorkerThread(const std::shared_ptr<UrmaPingDevThread>& devThread)
{
    if (devThread->workerThread != nullptr) {
        devThread->isStop.store(true);
        devThread->queueCv.notify_all();
        devThread->workerThread->join();
        delete devThread->workerThread;
        devThread->workerThread = nullptr;
    }
}

static void CancelPendingTasks(const std::shared_ptr<UrmaPingDevThread>& devThread)
{
    std::lock_guard<std::mutex> qLock(devThread->queueMutex);
    while (!devThread->taskQueue.empty()) {
        std::shared_ptr<UrmaPingTask> t = devThread->taskQueue.front();
        devThread->taskQueue.pop();
        t->canceled.store(true);
        t->cv.notify_one();
    }
}

static void TearDownDevThread(const std::shared_ptr<UrmaPingDevThread>& devThread)
{
    StopWorkerThread(devThread);
    CancelPendingTasks(devThread);
}

int UrmaPingDeinit(UrmaPingEntity* entities, int count)
{
    if (entities == nullptr) {
        printf("UrmaPingDeinit: invalid input, entities is null\n");
        return -1;
    }
    if (count <= 0) {
        printf("UrmaPingDeinit: invalid input, count=%d (must be > 0)\n", count);
        return -1;
    }

    std::lock_guard<std::mutex> lock(g_devMutex);

    std::set<int> processed;
    for (int i = 0; i < count; i++) {
        int devId = entities[i].devId;
        if (processed.count(devId) > 0) {
            printf("UrmaPingDeinit: duplicate device[%d] in input, skipping\n", devId);
            continue;
        }
        processed.insert(devId);

        auto it = g_devThread.find(devId);
        if (it == g_devThread.end()) {
            printf("UrmaPingDeinit: devThread not found for device[%d]\n", devId);
            continue;
        }

        TearDownDevThread(it->second);
        g_devThread.erase(it);
        printf("UrmaPingDeinit: removed device[%d]\n", devId);
    }

    printf("UrmaPingDeinit: remaining %zu devThreads\n", g_devThread.size());
    return 0;
}
