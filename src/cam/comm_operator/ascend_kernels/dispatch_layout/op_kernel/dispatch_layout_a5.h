/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: dispatch layout a5 kernel implementation
 * Create: 2026-05-25
 * Note:
 * History: 2026-05-25 create dispatch layout a5 kernel implementation (multi-mesh layered support)
 */

#ifndef DISPATCH_LAYOUT_A5_H
#define DISPATCH_LAYOUT_A5_H

#include <climits>
#include "kernel_operator.h"

#include "comm_args.h"
#include "data_copy.h"
#include "sync_collectives.h"
#include "moe_distribute_base.h"
#include "dispatch_layout_tiling.h"
using namespace Moe;
namespace MoeDispatchLayoutA5 {

constexpr uint32_t UB_32_ALIGN = 32U;
constexpr uint32_t A5_MAX_BATCH_SIZE = 4096U;
constexpr uint32_t TEMP_BATCH_SIZE = 32U;

template <AscendC::HardEvent event>
__aicore__ inline void SyncFunc()
{
    int32_t eventID = static_cast<int32_t>(GetTPipePtr()->FetchEventID(event));
    AscendC::SetFlag<event>(eventID);
    AscendC::WaitFlag<event>(eventID);
}

using namespace AscendC;
using namespace Moe;
template <typename T>
class DispatchLayoutA5 {
public:
    __aicore__ inline DispatchLayoutA5(){};

    __aicore__ inline void Init(GM_ADDR topkIdx, GM_ADDR numTokensPerRank, GM_ADDR numTokensPerExpert,
                                GM_ADDR isTokenInRank, GM_ADDR notifySendData, GM_ADDR sendTokenIdxSmall,
                                GM_ADDR workspace, TPipe *pipe, const DispatchLayoutTilingData *tilingData)
    {
        numTokens_ = tilingData->dispatchLayoutInfo.numTokens;
        numRanks_ = tilingData->dispatchLayoutInfo.numRanks;
        numExperts_ = tilingData->dispatchLayoutInfo.numExperts;
        numTopk_ = tilingData->dispatchLayoutInfo.numTopk;
        localRankSize_ = tilingData->dispatchLayoutInfo.localRankSize;
        meshNum_ = (numRanks_ + localRankSize_ - 1) / localRankSize_;
        tpipe_ = pipe;

        coreIdx_ = GetBlockIdx();
        uint32_t maxAivNum = GetBlockNum();
        aivNum_ = numTokens_ <= maxAivNum ? numTokens_ : maxAivNum;
        uint32_t temp = numTokens_ / aivNum_;
        uint32_t restNum = numTokens_ % aivNum_;
        int64_t topkIdxOffset;
        int64_t isTokenOffset;
        int64_t meshOffsetOffset;
        int64_t meshNumOffset;
        int64_t sendTokenIdxOffset;
        tempTokens_ = temp;

        if (coreIdx_ < aivNum_) {
            if (coreIdx_ < restNum) {
                tempTokens_++;
            }
            topkIdx32AlignIntLen_ = Ceil(tempTokens_ * numTopk_ * sizeof(int64_t), UB_32_ALIGN) * UB_32_ALIGN;
            numTokensPerRank32AlignIntLen_ = Ceil(numRanks_ * sizeof(T), UB_32_ALIGN) * UB_32_ALIGN;
            numTokensPerExpert32AlignIntLen_ = Ceil(numExperts_ * sizeof(T), UB_32_ALIGN) * UB_32_ALIGN;
            isTokenInRank32AlignIntLen_ = Ceil(tempTokens_ * numRanks_ * sizeof(T), UB_32_ALIGN) * UB_32_ALIGN;
            localTokenMeshUniqCount32AlignIntLen_ = Ceil(meshNum_ * sizeof(T), UB_32_ALIGN) * UB_32_ALIGN;
            localTokenMeshTotalCount32AlignIntLen_ =
                Ceil(tempTokens_ * meshNum_ * sizeof(T), UB_32_ALIGN) * UB_32_ALIGN;
            localTokenMeshOffset32AlignIntLen_ = localTokenMeshTotalCount32AlignIntLen_;
            localTokenMeshNum32AlignIntLen_ = Ceil(tempTokens_ * sizeof(T), UB_32_ALIGN) * UB_32_ALIGN;
            sendTokenIdx32AlignIntLen_ = Ceil(tempTokens_ * numExperts_ * sizeof(T), UB_32_ALIGN) * UB_32_ALIGN;
            expertRankTokenIdx32AlignIntLen_ =
                Ceil(numExperts_ * TEMP_BATCH_SIZE * sizeof(T), UB_32_ALIGN) * UB_32_ALIGN;

            if (coreIdx_ < restNum) {
                topkIdxOffset = coreIdx_ * tempTokens_ * numTopk_ * sizeof(int64_t);
                isTokenOffset = coreIdx_ * tempTokens_ * numRanks_ * sizeof(T);
                meshOffsetOffset = coreIdx_ * tempTokens_ * meshNum_ * sizeof(T);
                meshNumOffset = coreIdx_ * tempTokens_ * sizeof(T);
                sendTokenIdxOffset = coreIdx_ * tempTokens_ * numExperts_ * sizeof(T);
            } else {
                topkIdxOffset = (restNum + coreIdx_ * tempTokens_) * numTopk_ * sizeof(int64_t);
                isTokenOffset = (restNum + coreIdx_ * tempTokens_) * numRanks_ * sizeof(T);
                meshOffsetOffset = (restNum + coreIdx_ * tempTokens_) * meshNum_ * sizeof(T);
                meshNumOffset = (restNum + coreIdx_ * tempTokens_) * sizeof(T);
                sendTokenIdxOffset = (restNum + coreIdx_ * tempTokens_) * numExperts_ * sizeof(T);
            }

            topkIdxGM_.SetGlobalBuffer((__gm__ int64_t *)(topkIdx + topkIdxOffset));
            numTokensPerRankGM_.SetGlobalBuffer((__gm__ T *)numTokensPerRank);
            numTokensPerExpertSrcGM_.SetGlobalBuffer((__gm__ T *)numTokensPerExpert);
            numTokensPerExpertGM_.SetGlobalBuffer((__gm__ T *)notifySendData);
            isTokenInRankGM_.SetGlobalBuffer((__gm__ T *)(isTokenInRank + isTokenOffset));
            localTokenMeshUniqCountGM_.SetGlobalBuffer((__gm__ T *)(notifySendData) + numExperts_);
            localTokenMeshTotalCountGM_.SetGlobalBuffer((__gm__ T *)(notifySendData + meshOffsetOffset) +
                                                          numExperts_ + meshNum_);
            localTokenMeshNumGM_.SetGlobalBuffer((__gm__ T *)(notifySendData + meshNumOffset) + numExperts_ +
                                                   meshNum_ * (A5_MAX_BATCH_SIZE + 1));
            localTokenMeshOffsetGM_.SetGlobalBuffer((__gm__ T *)(notifySendData + meshOffsetOffset) + numExperts_ +
                                                      meshNum_ + A5_MAX_BATCH_SIZE * (meshNum_ + 1));
            sendTokenIdxGM_.SetGlobalBuffer((__gm__ T *)(notifySendData + sendTokenIdxOffset) + numExperts_ +
                                            meshNum_ + A5_MAX_BATCH_SIZE * (1 + 2 * meshNum_));
            expertRankTokenIdxGM_.SetGlobalBuffer((__gm__ T *)notifySendData + numExperts_ + meshNum_ +
                                                  A5_MAX_BATCH_SIZE * (1 + 2 * meshNum_ + numExperts_));
            tempExpertGM_.SetGlobalBuffer((__gm__ T *)notifySendData + numExperts_ + meshNum_ +
                                          A5_MAX_BATCH_SIZE * (1 + 2 * meshNum_));
            tempMeshGM_.SetGlobalBuffer((__gm__ T *)notifySendData + numExperts_ * (1 + aivNum_) + meshNum_ +
                                          A5_MAX_BATCH_SIZE * (1 + 2 * meshNum_));
            sendTokenIdxSmallGM_.SetGlobalBuffer((__gm__ T *)(sendTokenIdxSmall + topkIdxOffset / 2));
        }
    }

    __aicore__ inline void Process()
    {
        if (coreIdx_ < aivNum_) {
            MultiCoreCompute();
        } else {
            SyncAll<true>();
        }
    }

private:
    __aicore__ inline void MultiCoreCompute()
    {
        tpipe_->Reset();
        tpipe_->InitBuffer(topkIdxBuf_, topkIdx32AlignIntLen_);
        tpipe_->InitBuffer(sendTokenIdxSmallBuf_, topkIdx32AlignIntLen_);
        tpipe_->InitBuffer(numTokensPerRankBuf_, numTokensPerRank32AlignIntLen_);
        tpipe_->InitBuffer(numTokensPerExpertBuf_, numTokensPerExpert32AlignIntLen_);
        tpipe_->InitBuffer(prefixCountPerExpertBuf_, numTokensPerExpert32AlignIntLen_);
        tpipe_->InitBuffer(isTokenInRankBuf_, isTokenInRank32AlignIntLen_);
        tpipe_->InitBuffer(localTokenMeshUniqCountBuf_, localTokenMeshUniqCount32AlignIntLen_);
        tpipe_->InitBuffer(localTokenMeshTotalCountBuf_, localTokenMeshTotalCount32AlignIntLen_);
        tpipe_->InitBuffer(localTokenMeshNumBuf_, localTokenMeshNum32AlignIntLen_);
        tpipe_->InitBuffer(seenRankBuf_, numRanks_ * sizeof(T));
        tpipe_->InitBuffer(seenMeshBuf_, meshNum_ * sizeof(T));
        tpipe_->InitBuffer(localTokenMeshOffsetBuf_, localTokenMeshOffset32AlignIntLen_);
        tpipe_->InitBuffer(sendTokenIdxBuf_, sendTokenIdx32AlignIntLen_);
        tpipe_->InitBuffer(tempMeshBuf_, meshNum_ * sizeof(T));
        tpipe_->InitBuffer(tempExpertBuf_, numExperts_ * sizeof(T));
        tpipe_->InitBuffer(expertRankTokenIdxBuf_, expertRankTokenIdx32AlignIntLen_);
        tpipe_->InitBuffer(countExpertBuf_, numExperts_ * sizeof(T));
        tpipe_->InitBuffer(intermediateExpertBuf_, numExperts_ * sizeof(T));
        tpipe_->InitBuffer(intermediateMeshBuf_, meshNum_ * sizeof(T));

        LocalTensor<int64_t> topkIdxTensor = topkIdxBuf_.AllocTensor<int64_t>();
        LocalTensor<T> sendTokenIdxSmallTensor = sendTokenIdxSmallBuf_.AllocTensor<T>();
        const DataCopyExtParams dataCopyParams{1U, topkIdx32AlignIntLen_, 0U, 0U, 0U};
        const DataCopyPadExtParams<int64_t> padParams{false, 0U, 0U, 0U};
        DataCopyPad(topkIdxTensor, topkIdxGM_, dataCopyParams, padParams);
        LocalTensor<T> numTokensPerRankTensor = numTokensPerRankBuf_.AllocTensor<T>();
        LocalTensor<T> numTokensPerExpertTensor = numTokensPerExpertBuf_.AllocTensor<T>();
        LocalTensor<T> prefixCountPerExpertTensor = prefixCountPerExpertBuf_.AllocTensor<T>();
        LocalTensor<T> isTokenInRankTensor = isTokenInRankBuf_.AllocTensor<T>();
        LocalTensor<T> localTokenMeshUniqCountTensor = localTokenMeshUniqCountBuf_.AllocTensor<T>();
        LocalTensor<T> localTokenMeshTotalCountTensor = localTokenMeshTotalCountBuf_.AllocTensor<T>();
        LocalTensor<T> localTokenMeshNumTensor = localTokenMeshNumBuf_.AllocTensor<T>();
        LocalTensor<T> sendTokenIdxTensor = sendTokenIdxBuf_.AllocTensor<T>();
        LocalTensor<T> localTokenMeshOffsetTensor = localTokenMeshOffsetBuf_.AllocTensor<T>();
        LocalTensor<T> tempExpertTensor = tempExpertBuf_.AllocTensor<T>();
        LocalTensor<T> tempMeshTensor = tempMeshBuf_.AllocTensor<T>();
        LocalTensor<T> intermediateExpertTensor = intermediateExpertBuf_.AllocTensor<T>();
        LocalTensor<T> intermediateMeshTensor = intermediateMeshBuf_.AllocTensor<T>();
        LocalTensor<T> expertRankTokenIdxTensor = expertRankTokenIdxBuf_.AllocTensor<T>();
        LocalTensor<T> seenRankTensor = seenRankBuf_.AllocTensor<T>();
        LocalTensor<T> seenMeshTensor = seenMeshBuf_.AllocTensor<T>();
        LocalTensor<T> countExpertTensor = countExpertBuf_.AllocTensor<T>();
        Duplicate<T>(countExpertTensor, 0, numExperts_);
        Duplicate<T>(numTokensPerRankTensor, 0, numRanks_);
        Duplicate<T>(numTokensPerExpertTensor, 0, numExperts_);
        Duplicate<T>(prefixCountPerExpertTensor, 0, numExperts_);
        Duplicate<T>(isTokenInRankTensor, 0, tempTokens_ * numRanks_);
        Duplicate<T>(localTokenMeshOffsetTensor, 0, localTokenMeshOffset32AlignIntLen_ / sizeof(T));
        Duplicate<T>(sendTokenIdxTensor, 0, sendTokenIdx32AlignIntLen_ / sizeof(T));
        Duplicate<T>(tempMeshTensor, 0, meshNum_);
        Duplicate<T>(tempExpertTensor, 0, numExperts_);
        Duplicate<T>(intermediateMeshTensor, 0, meshNum_);
        Duplicate<T>(intermediateExpertTensor, 0, numExperts_);
        Duplicate<T>(localTokenMeshUniqCountTensor, 0, meshNum_);
        Duplicate<T>(localTokenMeshTotalCountTensor, 0, tempTokens_ * meshNum_);
        Duplicate<T>(localTokenMeshNumTensor, 0, tempTokens_);
        SyncFunc<AscendC::HardEvent::MTE2_S>();
        SyncFunc<AscendC::HardEvent::V_S>();
        int experts_per_rank = numExperts_ / numRanks_;
        for (int i = 0; i < tempTokens_; ++i) {
            SyncFunc<AscendC::HardEvent::S_V>();
            Duplicate<T>(seenRankTensor, 0, numRanks_);
            Duplicate<T>(seenMeshTensor, 0, meshNum_);
            SyncFunc<AscendC::HardEvent::V_S>();
            for (int j = 0; j < numTopk_; ++j) {
                int64_t expert_idx = topkIdxTensor.GetValue(i * numTopk_ + j);
                uint32_t per_expert_num = numTokensPerExpertTensor.GetValue(expert_idx) + 1;
                numTokensPerExpertTensor.SetValue(expert_idx, per_expert_num);
                int rank_id = expert_idx / experts_per_rank;
                int mesh_id = rank_id / localRankSize_;
                if (!seenMeshTensor.GetValue(mesh_id)) {
                    uint32_t uniqCount = localTokenMeshUniqCountTensor.GetValue(mesh_id) + 1;
                    localTokenMeshUniqCountTensor.SetValue(mesh_id, uniqCount);
                    localTokenMeshOffsetTensor.SetValue(i * meshNum_ + mesh_id, 1);
                    seenMeshTensor.SetValue(mesh_id, 1);
                    uint32_t sendMeshNum = localTokenMeshNumTensor.GetValue(i);
                    localTokenMeshNumTensor.SetValue(i, sendMeshNum + 1);
                }
                uint32_t totalCount = localTokenMeshTotalCountTensor.GetValue(i * meshNum_ + mesh_id) + 1;
                localTokenMeshTotalCountTensor.SetValue(i * meshNum_ + mesh_id, totalCount);
                sendTokenIdxTensor.SetValue(i * numExperts_ + expert_idx, 1);
                if (!seenRankTensor.GetValue(rank_id)) {
                    uint32_t per_rank_num = numTokensPerRankTensor.GetValue(rank_id) + 1;
                    isTokenInRankTensor.SetValue(i * numRanks_ + rank_id, 1);
                    seenRankTensor.SetValue(rank_id, 1);
                    numTokensPerRankTensor.SetValue(rank_id, per_rank_num);
                }
            }
        }
        SyncFunc<AscendC::HardEvent::S_MTE3>();
        uint32_t sendSize = tempTokens_ * numRanks_ * sizeof(T);
        const DataCopyExtParams isTokenInRankDataCopyParams{1U, sendSize, 0U, 0U, 0U};
        DataCopyPad(isTokenInRankGM_, isTokenInRankTensor, isTokenInRankDataCopyParams);
        sendSize = tempTokens_ * sizeof(T);
        const DataCopyExtParams localTokenMeshNumParams{1U, sendSize, 0U, 0U, 0U};
        DataCopyPad(localTokenMeshNumGM_, localTokenMeshNumTensor, localTokenMeshNumParams);
        sendSize = tempTokens_ * meshNum_ * sizeof(T);
        const DataCopyExtParams localTokenMeshTotalCountParams{1U, sendSize, 0U, 0U, 0U};
        DataCopyPad(localTokenMeshTotalCountGM_, localTokenMeshTotalCountTensor, localTokenMeshTotalCountParams);
        AscendC::SetAtomicAdd<T>();
        const DataCopyExtParams tempExpertDataCopyParams{1U, numTokensPerExpert32AlignIntLen_, 0U, 0U, 0U};
        for (int i = coreIdx_ + 1; i < aivNum_; ++i) {
            DataCopyPad(tempExpertGM_[i * numExperts_], numTokensPerExpertTensor, tempExpertDataCopyParams);
        }
        sendSize = meshNum_ * sizeof(T);
        const DataCopyExtParams tempMeshDataCopyParams{1U, sendSize, 0U, 0U, 0U};
        for (int i = coreIdx_ + 1; i < aivNum_; ++i) {
            DataCopyPad(tempMeshGM_[i * meshNum_], localTokenMeshUniqCountTensor, tempMeshDataCopyParams);
        }
        const DataCopyExtParams localTokenMeshUniqCountParams{1U, sendSize, 0U, 0U, 0U};
        DataCopyPad(localTokenMeshUniqCountGM_, localTokenMeshUniqCountTensor, localTokenMeshUniqCountParams);
        const DataCopyExtParams numTokensPerRankDataCopyParams{1U, numTokensPerRank32AlignIntLen_, 0U, 0U, 0U};
        DataCopyPad(numTokensPerRankGM_, numTokensPerRankTensor, numTokensPerRankDataCopyParams);
        const DataCopyExtParams numTokensPerExpertDataCopyParams{1U, numTokensPerExpert32AlignIntLen_, 0U, 0U, 0U};
        DataCopyPad(numTokensPerExpertGM_, numTokensPerExpertTensor, numTokensPerExpertDataCopyParams);
        DataCopyPad(numTokensPerExpertSrcGM_, numTokensPerExpertTensor, numTokensPerExpertDataCopyParams);
        AscendC::SetAtomicNone();
        PipeBarrier<PIPE_MTE3>();
        SyncAll<true>();
        SyncFunc<AscendC::HardEvent::MTE3_MTE2>();
        const DataCopyPadExtParams<T> tempPadParams{false, 0U, 0U, 0U};
        DataCopyPad(localTokenMeshUniqCountTensor, tempMeshGM_[coreIdx_ * meshNum_], tempMeshDataCopyParams,
                    tempPadParams);
        DataCopyPad(numTokensPerExpertTensor, tempExpertGM_[coreIdx_ * numExperts_], tempExpertDataCopyParams,
                    tempPadParams);
        SyncFunc<AscendC::HardEvent::MTE2_V>();
        SyncFunc<AscendC::HardEvent::MTE2_S>();
        for (int j = 0; j < meshNum_; ++j) {
            int value = localTokenMeshUniqCountTensor.GetValue(j);
            tempMeshTensor.SetValue(j, value);
        }
        DataCopy(prefixCountPerExpertTensor, numTokensPerExpertTensor, numExperts_);
        PipeBarrier<PIPE_V>();
        for (int i = 0; i < tempTokens_; ++i) {
            for (int j = 0; j < meshNum_; ++j) {
                int value1 = localTokenMeshOffsetTensor.GetValue(i * meshNum_ + j);
                int value2 = tempMeshTensor.GetValue(j);
                localTokenMeshOffsetTensor.SetValue(i * meshNum_ + j, value1 * value2 + value1);
                tempMeshTensor.SetValue(j, value2 + value1);
            }
            DataCopy(intermediateExpertTensor, sendTokenIdxTensor[i * numExperts_], numExperts_);
            PipeBarrier<PIPE_V>();
            AscendC::Mul(sendTokenIdxTensor[i * numExperts_], prefixCountPerExpertTensor, intermediateExpertTensor,
                         numExperts_);
            PipeBarrier<PIPE_V>();
            DataCopy(tempExpertTensor, sendTokenIdxTensor[i * numExperts_], numExperts_);
            PipeBarrier<PIPE_V>();
            AscendC::Add(sendTokenIdxTensor[i * numExperts_], tempExpertTensor, intermediateExpertTensor, numExperts_);
            PipeBarrier<PIPE_V>();
            DataCopy(tempExpertTensor, prefixCountPerExpertTensor, numExperts_);
            PipeBarrier<PIPE_V>();
            AscendC::Add(prefixCountPerExpertTensor, intermediateExpertTensor, tempExpertTensor, numExperts_);
            PipeBarrier<PIPE_V>();
        }
        SyncFunc<AscendC::HardEvent::S_MTE3>();
        SyncFunc<AscendC::HardEvent::V_MTE3>();
        sendSize = tempTokens_ * numExperts_ * sizeof(T);
        const DataCopyExtParams sendTokenIdxDataCopyParams{1U, sendSize, 0U, 0U, 0U};
        const DataCopyExtParams sendTokenIdxSmallDataCopyParams{
            1U, static_cast<uint32_t>(tempTokens_ * numTopk_ * sizeof(T)), 0U, 0U, 0U};
        DataCopyPad(sendTokenIdxGM_, sendTokenIdxTensor, sendTokenIdxDataCopyParams);
        sendSize = tempTokens_ * meshNum_ * sizeof(T);
        const DataCopyExtParams localTokenMeshOffsetParams{1U, sendSize, 0U, 0U, 0U};
        DataCopyPad(localTokenMeshOffsetGM_, localTokenMeshOffsetTensor, localTokenMeshOffsetParams);
        Duplicate<T>(countExpertTensor, 0, numExperts_);
        SyncFunc<AscendC::HardEvent::V_S>();
        for (int i = 0; i < tempTokens_; ++i) {
            for (int j = 0; j < numTopk_; ++j) {
                int64_t expert_idx = topkIdxTensor.GetValue(i * numTopk_ + j);
                int rank_id = expert_idx / experts_per_rank;
                int mesh_id = rank_id / localRankSize_;
                int32_t offset = localTokenMeshOffsetTensor.GetValue(i * meshNum_ + mesh_id);
                int32_t count = countExpertTensor.GetValue(expert_idx);
                expertRankTokenIdxTensor.SetValue(expert_idx * TEMP_BATCH_SIZE + count % TEMP_BATCH_SIZE, offset);
                count++;
                countExpertTensor.SetValue(expert_idx, count);
                sendTokenIdxSmallTensor(i * numTopk_ + j) = sendTokenIdxTensor(i * numExperts_ + expert_idx) - 1;
                if (count % TEMP_BATCH_SIZE == 0) {
                    int32_t preCount = numTokensPerExpertTensor.GetValue(expert_idx);
                    SyncFunc<AscendC::HardEvent::S_MTE3>();
                    const DataCopyExtParams expertRankTokendataCopyParams{1U, TEMP_BATCH_SIZE * sizeof(T), 0U, 0U, 0U};
                    DataCopyPad(expertRankTokenIdxGM_[expert_idx * A5_MAX_BATCH_SIZE + preCount + count - TEMP_BATCH_SIZE],
                                expertRankTokenIdxTensor[expert_idx * TEMP_BATCH_SIZE], expertRankTokendataCopyParams);
                    SyncFunc<AscendC::HardEvent::MTE3_S>();
                }
            }
        }
        SyncFunc<AscendC::HardEvent::S_MTE3>();
        DataCopyPad(sendTokenIdxSmallGM_, sendTokenIdxSmallTensor, sendTokenIdxSmallDataCopyParams);
        for (int i = 0; i < numExperts_; ++i) {
            int32_t count = countExpertTensor.GetValue(i);
            int32_t rest = count % TEMP_BATCH_SIZE;
            if (rest > 0) {
                int32_t preCount = numTokensPerExpertTensor.GetValue(i);
                SyncFunc<AscendC::HardEvent::S_MTE3>();
                const DataCopyExtParams expertRankTokendataCopyParams{1U, uint32_t(rest * sizeof(T)), 0U, 0U, 0U};
                DataCopyPad(expertRankTokenIdxGM_[i * A5_MAX_BATCH_SIZE + preCount + count - rest],
                            expertRankTokenIdxTensor[i * TEMP_BATCH_SIZE], expertRankTokendataCopyParams);
            }
        }
    }

    GlobalTensor<int64_t> topkIdxGM_;
    GlobalTensor<T> numTokensPerRankGM_;
    GlobalTensor<T> numTokensPerExpertGM_;
    GlobalTensor<T> numTokensPerExpertSrcGM_;
    GlobalTensor<T> isTokenInRankGM_;
    GlobalTensor<T> localTokenMeshOffsetGM_;
    GlobalTensor<T> localTokenMeshUniqCountGM_;
    GlobalTensor<T> localTokenMeshTotalCountGM_;
    GlobalTensor<T> localTokenMeshNumGM_;
    GlobalTensor<T> expertRankTokenIdxGM_;
    GlobalTensor<T> sendTokenIdxGM_;
    GlobalTensor<T> tempExpertGM_;
    GlobalTensor<T> tempMeshGM_;
    GlobalTensor<T> sendTokenIdxSmallGM_;

    TBuf<> topkIdxBuf_;
    TBuf<> sendTokenIdxSmallBuf_;
    TBuf<> numTokensPerRankBuf_;
    TBuf<> numTokensPerExpertBuf_;
    TBuf<> prefixCountPerExpertBuf_;
    TBuf<> isTokenInRankBuf_;
    TBuf<> localTokenMeshOffsetBuf_;
    TBuf<> localTokenMeshUniqCountBuf_;
    TBuf<> localTokenMeshTotalCountBuf_;
    TBuf<> localTokenMeshNumBuf_;
    TBuf<> seenRankBuf_;
    TBuf<> seenMeshBuf_;
    TBuf<> countExpertBuf_;
    TBuf<> countMeshBuf_;
    TBuf<> sendTokenIdxBuf_;
    TBuf<> tempExpertBuf_;
    TBuf<> tempMeshBuf_;
    TBuf<> expertRankTokenIdxBuf_;
    TBuf<> intermediateExpertBuf_;
    TBuf<> intermediateMeshBuf_;

    TPipe *tpipe_{nullptr};
    uint32_t numTokens_{0};
    uint32_t numRanks_{0};
    uint32_t numExperts_{0};
    uint32_t numTopk_{0};
    uint32_t localRankSize_{0};
    uint32_t meshNum_{0};
    uint32_t coreIdx_{0};
    uint32_t aivNum_{0};
    uint32_t tempTokens_{0};
    uint32_t rank_{0};

    uint32_t topkIdx32AlignIntLen_{0};
    uint32_t numTokensPerRank32AlignIntLen_{0};
    uint32_t numTokensPerExpert32AlignIntLen_{0};
    uint32_t isTokenInRank32AlignIntLen_{0};
    uint32_t localTokenMeshOffset32AlignIntLen_{0};
    uint32_t localTokenMeshUniqCount32AlignIntLen_{0};
    uint32_t localTokenMeshTotalCount32AlignIntLen_{0};
    uint32_t localTokenMeshNum32AlignIntLen_{0};
    uint32_t sendTokenIdx32AlignIntLen_{0};
    uint32_t expertRankTokenIdx32AlignIntLen_{0};
};
}  // namespace MoeDispatchLayoutA5

#endif  // DISPATCH_LAYOUT_A5_H
