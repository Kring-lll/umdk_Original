/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: SNC (Supernode Network Controller) service
 * Create: 2026-07-13
 */
#include <chrono>
#include <thread>
#include <atomic>
#include <map>
#include <string>
#include <mutex>
#include <vector>
#include <queue>
#include <condition_variable>
#include "urma_ping_inner.h"

static std::mutex g_devMutex;
static std::map<int , UrmaPingDevThread*> g_devThread;

static UrmaPingDevThread* FindDevThread(int devId){
    std::lock_guard<std::mutex> lock(g_devMutex);
    auto it = g_devThread.find(devId);
    if(it == g_devThread.find(devId)){
        return nullptr;
    }
    return it->second;
}

static UrmaPingEidCtx* FindEidCtx(UrmaPingDevThread* devThread, const char* eid){
    std::lock_guard<std::mutex> lock(devThread->eidMutex);
    for(auto& eidCtx : devThread->eidCtxs){
        if(strcmp(eidCtx.eid, eid) == 0){
            return &eidCtx;
        }
    }
    return nullptr;
}

static int DoPing(UrmaPingDevThread* g_devThread, UrmaPingEidCtx* eidCtx, UrmaPingTask* task){
    HccnRpingTargetInfo target;
    memset(&target, 0, sizeof(target));
    target.srcPort = 0;
    target.sl = 0;
    target.tc = 0;
    target.port = 13886;
    target.srcEid = new char[IP_LEN];
    target.dstEid = new char[IP_LEN];
    target.addrType = 1;
    char payload[12] = "hellotarget";
    target.payloadLen = 12;
    strcpy(target.payload , payload);
    strncpy(target.srcEid , task->clientEid , IP_LEN - 1);
    strncpy(target.dstEid , task->serverEid , IP_LEN - 1);

    HccnRpingResultInfo* hccnResult = new HccnRpingResultInfo;

    printf("DoPing: start ping %s -> %s\n", task->clientEid , task->serverEid);

    HccnResult ret = HccnRpingAddTarget(eidCtx->rpingCtx, 1, &target);
    if(ret != HCCN_SUCCESS){
        delete[] target.srcEid;
        delete[] target.dstEid;
        delete hccnResult;
        printf("DoPing: add target failed\n");
        return -1;
    }

    uint32_t interval = 1;
    uint32_t timeout = 100;
    ret = HccnRpingBatchPingStart(eidCtx->rpingCtx ,PKT_NUM ,interval, timeout);
    if(ret != HCCN_SUCCESS){
        HccnRpingRemoveTarget(eidCtx->rpingCtx, 1, &target);
        delete[] target.srcEid;
        delete[] target.dstEid;
        delete hccnResult;
        printf("DoPing: ping process failed\n");
        return -1;
    }
    HccnResult hccnRet = HCCN_E_AGAIN;
    while(hccnRet == HCCN_E_AGAIN){
        sleep(1);
        hccnRet = HccnRpingGetResult(eidCtx->rpingCtx, 1, &target, hccnResult);
    }
    if(hccnRet != HCCN_SUCCESS){
        HccnRpingBatchPingStop(eidCtx->rpingCtx);
        HccnRpingRemoveTarget(eidCtx->rpingCtx, 1, &target);
        delete[] target.srcEid;
        delete[] target.dstEid;
        delete hccnResult;
        printf("DoPing: get result failed\n");
        return -1;
    }

    task->result->txPkt = hccnResult->txPkt;
    task->result->rxPkt = hccnResult->rxPkt;
    task->result->minRTT = hccnResult->minRTT;
    task->result->maxRTT = hccnResult->maxRTT;
    task->result->avgRTT = hccnResult->avgRTT;
    task->result->state = hccnResult->state;

    printf("DoPing: txPkt[%u] rxPkt[%u] minRTT[%u] maxRTT[%u] avgRTT[%u] state[%u]\n",
            task->result->txPkt, task->result->rxPkt, task->result->minRTT,
            task->result->maxRTT, task->result->avgRTT, task->result->state);

    ret = HccnRpingBatchPingStop(eidCtx->rpingCtx);
    if(ret != HCCN_SUCCESS){
        HccnRpingRemoveTarget(eidCtx->rpingCtx);
        delete[] target.srcEid;
        delete[] target.dstEid;
        delete hccnResult;
        printf("DoPing: stop ping failed\n");
        return -1;
    }

    ret = HccnRpingRemoveTarget(eidCtx->rpingCtx, 1, &target);
    if(ret != HCCN_SUCCESS){
        delete[] target.srcEid;
        delete[] target.dstEid;
        delete hccnResult;
        printf("DoPing: remove target failed\n");
        return -1;
    }

    delete[] target.srcEid;
    delete[] target.dstEid;
    delete hccnResult;
    return 0;
}

static void WorkerThreadFunc(UrmaPingDevThread* devThread){
    aclrtSetDevice(devThread->devId);
    {
        std::lock_guard<std::mutex> lock(devThread->eidMutex);
        for(auto& eidCtx : devThread->eidCtxs){
            HccnRpingInitAttr* initAttr = new HccnRpingInitAttr();
            initAttr->mode = HCCN_RPING_MODE_UB_RC;
            initAttr->port = 13886;
            initAttr->npuNum = 128;
            size_t needSize = (size_t)PKT_NUM * 2048;
            initAttr->bufferSize = ((needSize + 4095) / 4096 + 1) * 4096;
            initAttr->eid = new char[IP_LEN];
            strcpy(initAttr->eid, eidCtx.eid);
            HccnRpingCtx rpingCtx = nullptr;
            HccnResult ret = HccnRpingInit(devThread->devId, initAttr, &rpingCtx);
            delete[] initAttr->eid;
            delete initAttr;
            if(ret != HCCN_SUCCESS){
                printf("device[%d,%s] init failed, ret=%d\n", devThread->devId, eidCtx.eid, ret);
                continue;
            }
            eidCtx.rpingCtx = rpingCtx;
            eidCtx.isInited = true;
            printf("device[%d,%s] init success, listening...\n", devThread->devId, eidCtx.eid);
        }
    }

    while(devThread->isStop.load() == false){
        UrmaPingTask* task = nullptr;
        {
            std::lock_guard<std::mutex> lock(devThread->queueMutex);
            if(!devThread->taskQueue.empty()){
                task = devThread->taskQueue.front();
                devThread->taskQueue.pop();
            }
        }

        if(task != nullptr){
            UrmaPingEidCtx* eidCtx = FindEidCtx(devThread, task->clientEid);
            if(eidCtx == nullptr || !eidCtx->isInited){
                printf("WorkerThread: eidCtx not found or not inited for devId=%d, eid=%s\n", task->clientDevId, task->clientEid);
                task->ret.store(-1);
                task->done.store(true);
                task->cv.notify_one();
                continue;
            }
            int doPingRet = DoPing(devThread, eidCtx, task);
            task->ret.store(doPingRet);
            task->done(true);
            task->cv.notify_one();
        }else{
            std::unique_lock<std::mutex> lock(devThread->queueMutex);
            devThread->queueCv.wait_for(lock, std::chrono::seconds(1));
        }
    }

    {
        std::lock_guard<std::mutex> lock(devThread->eidMutex);
        for(auto& eidCtx : devThread->eidCtxs){
            if(eidCtx.isInited){
                HccnResult ret = HccnRpingDeinit(eidCtx.rpingCtx);
                if(ret != HCCN_SUCCESS){
                    printf("device[%d,%s] deinit failed\n", devThread->devId, eidCtx.eid);
                }else{
                    printf("device[%d,%s] deinit success\n", devThread->devId, eidCtx.eid);
                }
                eidCtx.isInited = false;
            }
        }
    }
}

UrmaPingInit(UrmaPingEntity* entities, int count){
    std::lock_guard<std::mutex> lock(g_devMutex);

    std::map<int, std::vector<UrmaPingEntity>> devToEids;
    for(int i = 0 ; i < count ; i++){
        devToEids[entities[i].devId].push_back(entities[i]);
    }

    for(auto& entry : devToEids){
        int devId = entry.first;
        if(g_devThread.find(devId) != g_devThread.end()){
            printf("device[%d] already inited\n", devId);
            continue;
        }
        UrmaPingDevThread* devThread = new UrmaPingDevThread();
        devThread->devId = devId;
        devThread->isStop.store(false);

        for(auto& entity : entry.second){
            UrmaPingEidCtx eidCtx;
            memset(&eidCtx, 0, sizeof(eidCtx));
            strncpy(eidCtx.eid, entity.eid, IP_LEN - 1);
            devThread->eidCtxs.push_back(eidCtx);
        }

        devThread->workerThread = new std::thread(WorkerThreadFunc, devThread);
        g_devThread[devId] = devThread;
        printf("UrmaPingInit: added devThread for device[%d] with %zu eids\n", devId , entry.second.size());
    }

    printf("UrmaPingInit: total %zu devThreads\n", g_devThread.size());
    return 0;
}

int UrmaPingOne(int clientDevId, const char* clientEid, const char* serverEid, UrmaPingResult* result){
    UrmaPingDevThread* devThread = FindDevThread(clientDevId);
    if(devThread == nullptr){
        printf("UrmaPingOne: devThread not found for devId=%d\n", clientDevId);
        return -1;
    }

    UrmaPingTask task;
    task.clientDevId = clientDevId;
    memset(task.clientEid, 0 , IP_LEN);
    memset(task.serverEid, 0 , IP_LEN);
    strncpy(task.clientEid, clientEid, IP_LEN - 1);
    strncpy(task.serverEid, serverEid, IP_LEN - 1);
    task.result = result;
    task.done.store(false);
    task.ret.store(0);

    {
        std::lock_guard<std::mutex> lock(devThread->queueMutex);
        devThread->taskQueue.push(&task);
        devThread->queueCv.notify_one();
    }

    {
        std::unique_lock<std::mutex> lock(task.cvMutex);
        while(task.done.load() == false){
            task.cv.wait(lock);
        }
    }

    return task.ret.load();
}

int UrmaPingDeinit(UrmaPingEntity* entities , int count){
    std::lock_guard<std::mutex> lock(g_devMutex);

    for(int i = 0 ; i < count ; i++){
        auto it = g_devThread.find(entities[i].devId);
        if(it == g_devThread.end()){
            printf("UrmaPingDeinit: devThread not found for device[%d]\n", entities[i].devId);
            continue;
        }

        UrmaPingDevThread* devThread = it->second;
        if(devThread->workerThread != nullptr){
            devThread->isStop.store(true);
            devThread->queueCv.notify_all();
            devThread->workerThread->join();
            delete devThread->workerThread;
            devThread->workerThread = nullptr;
        }
        delete devThread;
        g_devThread.erase(it);
        printf("UrmaPingDeinit: removed device[%d]\n", entities[i].devId);
    }

    printf("UrmaPingDeinitL remaining %zu devThreads\n", g_devThread.size());
    return 0;
}