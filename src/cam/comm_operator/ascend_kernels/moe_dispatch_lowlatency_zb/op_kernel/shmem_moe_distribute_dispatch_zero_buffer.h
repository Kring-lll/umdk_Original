/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: ShmemMoeDistributeDispatchZeroBuffer operator kernel function header file
 * Create: 2026-06-10
 * Note:
 * History: 2026-06-10 create ShmemMoeDistributeDispatchZeroBuffer operator kernel function header file
 */

/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: ShmemMoeDistributeDispatchZeroBuffer operator kernel function header file, for A3 ZeroBuffer
 * Author: Hu Tianlun
 * Create: 2026-01-03
 * Note:
 * History: 2026-01-03 create ShmemMoeDistributeDispatchZeroBuffer operator kernel function header file, for A3 ZeroBuffer
 */

#ifndef SHMEM_MOE_DISTRIBUTE_DISPATCH_ZEROBUFFER_H
#define SHMEM_MOE_DISTRIBUTE_DISPATCH_ZEROBUFFER_H

#include "shmem_api.h"
// #include "data_copy.h"
#include "kernel_operator.h"
#include "kernel_tiling/kernel_tiling.h"
// #include "shmem_moe_distribute_v2_base.h"
#include "shmem_moe_distribute_dispatch_zero_buffer_tiling.h"
#include <type_traits>

using namespace AscendC;
#define SHMEM_PUT_BY_DTYPE(dtype, ...)                                      \
    do {                                                                    \
        if constexpr (std::is_same_v<dtype, half>) {                        \
            shmem_put_half_mem_nbi(__VA_ARGS__);                            \
        } else if constexpr (std::is_same_v<dtype, bfloat16_t>) {           \
            shmem_put_bfloat16_mem_nbi(__VA_ARGS__);                        \
        } else if constexpr (std::is_same_v<dtype, float>) {                \
            shmem_put_float_mem_nbi(__VA_ARGS__);                           \
        }                                                                   \
    } while (0)

#define SHMEM_GET_BY_DTYPE(dtype, ...)                                      \
    do {                                                                    \
        if constexpr (std::is_same_v<dtype, half>) {                        \
            shmem_get_half_mem_nbi(__VA_ARGS__);                            \
        } else if constexpr (std::is_same_v<dtype, bfloat16_t>) {           \
            shmem_get_bfloat16_mem_nbi(__VA_ARGS__);                        \
        }                                                                   \
    } while (0)

namespace MoeDistributeDispatchZeroBufferImpl {
constexpr uint8_t BUFFER_NUM = 2;  // 多buf
constexpr uint8_t BUFFER_SINGLE = 1;
constexpr uint8_t COMM_NUM = 2;               // 通信域大小
constexpr uint8_t COMM_EP_IDX = 0;
constexpr uint8_t COMM_TP_IDX = 1;
// 先写死这个偏移，如果TP固定为2，可直接往起始数据偏移开始读写
constexpr uint64_t WIN_STATE_OFFSET = 500UL * 1024UL;
constexpr uint64_t STATE_WIN_OFFSET = 950UL * 1024UL;
constexpr int64_t UB_SINGLE_DMA_SIZE_MAX = 190 * 1024;
constexpr int64_t UB_SINGLE_PING_PONG_ADD_SIZE_MAX = UB_SINGLE_DMA_SIZE_MAX / 2;
constexpr int UB_ALIGN_SIZE = 32;
constexpr static int32_t UB_HEAD_OFFSET = 96;
constexpr static int32_t UB_MID_OFFSET = UB_HEAD_OFFSET + UB_SINGLE_PING_PONG_ADD_SIZE_MAX + UB_ALIGN_SIZE;
constexpr uint64_t ALIGNED_LEN_256 = 256UL;
constexpr uint64_t ALIGNED_LEN_64 = 64UL;
constexpr uint32_t UB_32B_ALIGN = 32;
constexpr uint32_t FLAG_CNT_ALIGN = UB_32B_ALIGN / sizeof(int32_t);  // 8
constexpr int32_t BITS_PER_BYTE = 8;
constexpr uint32_t MAX_UB_SIZE = 170U * 1024U;
// add meta info offset on meta_gva_gm
constexpr uint64_t RANK_META_INFO_OFFSET = 1024UL * 1024UL;
constexpr uint32_t UB_ALIGN = 32U;

template <AscendC::HardEvent event>
__aicore__ inline void SyncFunc()
{
    int32_t eventID = static_cast<int32_t>(GetTPipePtr()->FetchEventID(event));
    AscendC::SetFlag<event>(eventID);
    AscendC::WaitFlag<event>(eventID);
}

#define TemplateMC2TypeClass                                                                               \
    typename XType, typename ExpandXOutType, bool StaticQuant, bool DynamicQuant, bool IsSmoothScaleExist, \
        bool IsNeedAllgather
#define TemplateMC2TypeFunc XType, ExpandXOutType, StaticQuant, DynamicQuant, IsSmoothScaleExist, IsNeedAllgather

// using namespace MoeDistributeV2Base;
template <TemplateMC2TypeClass>
class ShmemMoeDistributeDispatchZeroBuffer
{
public:
    __aicore__ inline ShmemMoeDistributeDispatchZeroBuffer(){};
    // All Output Tensor GM can be regarded as a shmem_ptr now
    __aicore__ inline void Init(GM_ADDR x, GM_ADDR expertIds, GM_ADDR scales, GM_ADDR xActiveMask, GM_ADDR elasticInfo,
                                GM_ADDR expandXOut, GM_ADDR dynamicScalesOut, GM_ADDR expandIdxOut,
                                GM_ADDR expertTokenNumsOut, GM_ADDR sendCountsOut, GM_ADDR tpSendCountsOut,
                                GM_ADDR workspaceGM, TPipe *pipe, const ShmemMoeDistributeDispatchZeroBufferTilingData *tilingData);
    __aicore__ inline void Process();

private:
    __aicore__ inline void SendToSharedExpert();
    __aicore__ inline void SendToMoeExpert();
    __aicore__ inline void SendCountNotify();
    __aicore__ inline void WaitNotify();
    __aicore__ inline void InputToDstOutput();
    __aicore__ inline void LocalLayout();
    __aicore__ inline void SetLayoutStatus();
    __aicore__ inline void CleanUp();
    __aicore__ inline void ReduceMaxInplace(const LocalTensor<float> &srcLocal, uint32_t count);
    __aicore__ inline void QuantProcess();
    __aicore__ inline void NotifyBufInit();
    __aicore__ inline void QuantInit(GM_ADDR scales);
    __aicore__ inline void SplitToCore(uint32_t curSendCnt, uint32_t curUseAivNum, uint32_t &startTokenId,
                                       uint32_t &endTokenId, uint32_t &sendTokenNum, bool isFront = true);
    __aicore__ inline void CalTokenSendExpertCnt(uint32_t dstExpertId, int32_t calCnt, int32_t &curExpertCnt);
    __aicore__ inline void ReorderRecvDataOutput(int32_t rankId, LocalTensor<int32_t> &transLt, bool isCumSum = false);
    
    __aicore__ inline GM_ADDR GetWindAddrByRankId(uint8_t ctxIdx, const int32_t rankId)
    {
        return (GM_ADDR)(metaInfo_gva_gm) + STATE_WIN_OFFSET + dataState_ * WIN_STATE_OFFSET + RANK_META_INFO_OFFSET;
    }

    // get Core level metaInfo address
    __aicore__ inline GM_ADDR GetWindStateAddrByRankId(uint8_t ctxIdx, const int32_t rankId)
    {
        return (GM_ADDR)(metaInfo_gva_gm) + STATE_WIN_OFFSET + dataState_ * WIN_STATE_OFFSET;
    }

    TPipe *tpipe_{nullptr};
    GlobalTensor<XType> xGMTensor_;
    GlobalTensor<int32_t> expertIdsGMTensor_;
    GlobalTensor<int32_t> allExpertTokenNumsGMTensor_;
    GlobalTensor<int32_t> expandIdxGMTensor_;
    GlobalTensor<int32_t> LocalNotifyDataTensor_;
    GlobalTensor<float> LocalNotifyStatusTensor_;
    GlobalTensor<int32_t> magicValTensor_;
    GlobalTensor<float> layoutStatusTensor_;
    GlobalTensor<float> recvCntStatusGt;
    GlobalTensor<ExpandXOutType> dstWinGMTensor;
    GlobalTensor<float> dstScaleGMTensor;
    GlobalTensor<int32_t> sendCountsGlobal;

    LocalTensor<ExpandXOutType> xTmpTensor_;
    LocalTensor<XType> xInTensor_;
    LocalTensor<ExpandXOutType> xOutTensor_;
    LocalTensor<float> xOutFp32Tensor_;
    LocalTensor<int32_t> expertIdsTensor_;
    LocalTensor<float> rowMaxTensor_;
    LocalTensor<float> layoutFlag_;
    LocalTensor<float> statusTensor_;
    LocalTensor<float> notifyStatusFp32Tensor_;
    LocalTensor<float> layoutStatusFp32Tensor_;
    LocalTensor<float> smoothScalesTensor_;
    LocalTensor<int32_t> dstExpIdTensor_;
    LocalTensor<int32_t> subExpIdTensor_;
    LocalTensor<float> workLocalTensor_;
    LocalTensor<int32_t> recvDataTensor_;
    LocalTensor<int32_t> putOffsetTensor_;  // 全局recv_count前缀和
    LocalTensor<int32_t> expandIdsTensor_;

    TBuf<> expertIdsBuf_;
    TBuf<> putOffsetBuf;
    TBuf<> expandIdsBuf_;
    TBuf<> statusBuf_;
    TBuf<> notifyBuf_;
    TBuf<> gatherNotiStatusBuf_;
    TBuf<> notiStatusBuf_;
    TBuf<> rowMaxBuf_;
    TBuf<> receiveDataCastFloatBuf_;
    TBuf<> smoothScalesBuf_;
    TBuf<> dstExpBuf_;
    TBuf<> subExpBuf_;
    TBuf<> layoutWaitStatusBuf_;
    TBuf<> notifyWaitStatusBuf_;
    TBuf<> validExpertIndexBuf_;
    TBuf<> validBsIndexTBuf_;
    TBuf<> gatherMaskTBuf_;
    TBuf<> SendCountBuf_;
    TBuf<> recvDataBuf_;
    TBuf<> sendCountBuf_;
    TBuf<QuePosition::VECCALC> CleartBuf;
    TQueBind<QuePosition::VECIN, QuePosition::VECOUT, 1> xQueue_;  // 非量化使用，量化场景接收也可使用
    TQue<QuePosition::VECIN, 1> xInQueue_;                         // 量化使用，量化前的输入
    TQue<QuePosition::VECOUT, 1> xOutQueue_;                       // 量化使用，量化后的输出

    GM_ADDR expandXOutGM_;                                          // This is also a shmem ptr now
    GM_ADDR dynamicScalesOutGM_;
    GM_ADDR sendCountsOutGM_;
    GM_ADDR sendTpCountOutGM_;
    GM_ADDR putOffsetWorkspaceGM_;
    GM_ADDR statusDataSpaceGm_;
    GM_ADDR layoutStatusGm_;
    GM_ADDR localNotifyDataSpaceGm_;
    GM_ADDR localNotifyStatusGm_;
    // metaInfo heap addrs
    GM_ADDR metaInfo_gva_gm;

    // tiling侧已确保数据上限，相乘不会越界，因此统一采用uint32_t进行处理
    uint32_t axisBS_{0};
    uint32_t axisMaxBS_{0};
    uint32_t axisH_{0};
    uint32_t axisK_{0};
    uint32_t aivNum_{0};
    uint32_t sharedUsedAivNum_{0};
    uint32_t moeUsedAivNum_{0};
    uint32_t epWorldSize_{0};
    uint32_t epWorldSizeOriginal_{0};
    uint32_t tpWorldSize_{0};
    int32_t epRankId_{0};
    int32_t epRankIdOriginal_{0};
    uint32_t tpGatherRankId_{0};  // gather 对端ID
    uint32_t tpRankId_{0};        // 本卡 ID
    uint32_t aivId_{0};           // aiv id
    uint32_t coreNum_{0};
    uint32_t sharedExpertNum_{0};
    uint32_t sharedExpertRankNum_{0};     // 共享专家卡数
    uint32_t rankNumPerSharedExpert_{0};  // 部署单个共享专家所用的卡数
    uint32_t moeExpertNum_{0};
    uint32_t globalBS_{0};
    uint32_t moeExpertRankNum_{0};  // moe专家卡数，等于epWorldSize_ - sharedExpertRankNum_
    uint32_t moeExpertNumPerRank_{0};
    uint32_t totalExpertNum_{0};
    // uint32_t dealRankPerCore_{0};
    uint32_t hOutSize_{0};
    uint32_t hAlignWinSize_{0};
    uint32_t hAlignWinCnt_{0};
    uint32_t hOutAlignUbSize_{0};
    uint32_t hOutUBAlignSize_{0};
    uint32_t hOutSizeAlign_{0};
    uint32_t clearAlign_{0};
    uint32_t startExpertId_;
    uint32_t endExpertId_;
    uint32_t sendExpertNum_;
    uint32_t totalCnt_;
    uint32_t lastCore_{0};
    uint32_t dataState_{0};
    uint32_t axisBsAlignSize_{0};
    uint32_t totalUsedUB_{0};
    uint64_t activeMaskBsCnt_{0};
    // uint64_t winDataSizeOffset_{0};
    uint64_t expertPerSizeOnWin_{0};
    uint64_t recvWinBlockNum_;  // 接收Win区块数
    uint64_t sendToMoeExpTokenCnt_{0};
    bool isTokenMaskFlag_ = false;
    bool isExpertMaskFlag_ = false;
    bool hasElasticInfoFlag_ = false;
    bool isScalingDownFlag_ = false;
    bool isShareExpertRankFlag_ = false;
    float exp_flag;
    // float sumTarget_;
    uint64_t totalWinSize_{0};
    uint32_t gatherCount_{0};
    uint32_t expertTokenNumsType_{1};
    uint32_t preCnt_{0};
    // uint32_t stateOffset_{0};
    uint32_t recStatusNumPerCore_{0};
    int32_t expertIdsCnt_{0};
    // int32_t tokenQuantAlign_{0};
    int32_t zeroComputeExpertNum_{0};
    uint32_t rscvStatusNum_{0};
    uint32_t remainderRankNum_{0};
    uint32_t startStatusIndex_{0};
    uint32_t sendToSharedExpTokenCnt_{0};
    uint32_t maxSize_{0};
    uint32_t bufferNum_{0};
    uint32_t recvDataAlignLen_{0};
    uint32_t putOffsetAlignSize{0};
    int32_t magicVal_{0};

    DataCopyExtParams expandXCopyParams_;
    DataCopyExtParams scaleCopyParams_;
    DataCopyExtParams xCopyParams_;
};

template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeDispatchZeroBuffer<TemplateMC2TypeFunc>::Init(
    GM_ADDR x, GM_ADDR expertIds, GM_ADDR scales, GM_ADDR xActiveMask, GM_ADDR elasticInfo, GM_ADDR expandXOut,
    GM_ADDR dynamicScalesOut, GM_ADDR expandIdxOut, GM_ADDR expertTokenNumsOut, GM_ADDR sendCountsOut,
    GM_ADDR tpSendCountsOut, GM_ADDR workspaceGM, TPipe *pipe, const ShmemMoeDistributeDispatchZeroBufferTilingData *tilingData)
{
    tpipe_ = pipe;
    aivId_ = GetBlockIdx();
    coreNum_ = GetBlockNum();
    epRankId_ = tilingData->moeDistributeDispatchZeroBufferInfo.epRankId;
    // epRankIdOriginal_ = tilingData->moeDistributeDispatchZeroBufferInfo.epRankId;
    PRINTF("[----------- CHECK DISPATCH ----------- ] of Rank %d\n", epRankId_);

    // 检查hcclwinsize是否越界
    totalWinSize_ = static_cast<uint64_t>(tilingData->moeDistributeDispatchZeroBufferInfo.totalWinSize);

    axisBS_ = tilingData->moeDistributeDispatchZeroBufferInfo.bs;
    axisH_ = tilingData->moeDistributeDispatchZeroBufferInfo.h;
    epWorldSizeOriginal_ = tilingData->moeDistributeDispatchZeroBufferInfo.epWorldSize;
    epWorldSize_ = tilingData->moeDistributeDispatchZeroBufferInfo.epWorldSize;
    sharedExpertRankNum_ = tilingData->moeDistributeDispatchZeroBufferInfo.sharedExpertRankNum;
    moeExpertNum_ = tilingData->moeDistributeDispatchZeroBufferInfo.moeExpertNum;
    globalBS_ = tilingData->moeDistributeDispatchZeroBufferInfo.globalBs;
    tpRankId_ = tilingData->moeDistributeDispatchZeroBufferInfo.tpRankId;
    tpGatherRankId_ = ((tpRankId_ == 0) ? 1 : 0);
    axisK_ = tilingData->moeDistributeDispatchZeroBufferInfo.k;
    aivNum_ = tilingData->moeDistributeDispatchZeroBufferInfo.aivNum;
    tpWorldSize_ = tilingData->moeDistributeDispatchZeroBufferInfo.tpWorldSize;

    // get shmem ptrs
    metaInfo_gva_gm = (GM_ADDR)tilingData->moeDistributeDispatchZeroBufferInfo.shmemptr;
    statusDataSpaceGm_ = (GM_ADDR)(metaInfo_gva_gm);
    // metaInfo 先放每个 AI Core 的 magicVal
    GlobalTensor<int32_t> allMagicValTensor_;
    allMagicValTensor_.SetGlobalBuffer((__gm__ int32_t *)(statusDataSpaceGm_));
    magicValTensor_.SetGlobalBuffer((__gm__ int32_t *)(statusDataSpaceGm_) + aivId_ * FLAG_CNT_ALIGN);

    // 这里放 LocalLayout 的 Flag
    layoutStatusGm_ = (GM_ADDR)(statusDataSpaceGm_ + coreNum_ * UB_32B_ALIGN);
    layoutStatusTensor_.SetGlobalBuffer((__gm__ float *)(layoutStatusGm_));
    // 这里放 SendCount 本卡的 Flag --> 也对应 rankSize 个
    localNotifyStatusGm_ = (GM_ADDR)(layoutStatusGm_ + coreNum_ * UB_32B_ALIGN);
    LocalNotifyStatusTensor_.SetGlobalBuffer((__gm__ float *)(localNotifyStatusGm_) + aivId_ * FLAG_CNT_ALIGN);
    // 这里放 SendCount
    localNotifyDataSpaceGm_ = (GM_ADDR)(localNotifyStatusGm_ + coreNum_ * ALIGNED_LEN_64);
    LocalNotifyDataTensor_.SetGlobalBuffer((__gm__ int32_t *)(localNotifyDataSpaceGm_));

    // 每次调用magic++,用来区分不同轮次
    TBuf<> tBuf;
    tpipe_->InitBuffer(tBuf, UB_32B_ALIGN);
    LocalTensor<int32_t> tempLocal = tBuf.Get<int32_t>();
    tempLocal(0) = 1;

    // 使用atomic方式实现+1
    AscendC::SetAtomicAdd<int32_t>();
    AscendC::SetFlag<HardEvent::S_MTE3>(EVENT_ID0);
    AscendC::WaitFlag<HardEvent::S_MTE3>(EVENT_ID0);  // 等待SetValue完成
    DataCopy(magicValTensor_, tempLocal, FLAG_CNT_ALIGN);
    AscendC::SetAtomicNone();
    AscendC::SetFlag<HardEvent::MTE3_S>(EVENT_ID0);
    AscendC::WaitFlag<HardEvent::MTE3_S>(EVENT_ID0);  // 等待SetValue完成
    magicVal_ = magicValTensor_.GetValue(0);
    PipeBarrier<PIPE_ALL>();

    exp_flag = (float)magicVal_;

    tpipe_->InitBuffer(notifyBuf_, FLAG_CNT_ALIGN);
    layoutFlag_ = notifyBuf_.Get<float>();
    layoutFlag_.SetValue(0, exp_flag);
    SyncFunc<AscendC::HardEvent::S_MTE3>();
    DataCopy(LocalNotifyStatusTensor_, layoutFlag_, FLAG_CNT_ALIGN);

    if (epRankId_ < sharedExpertRankNum_) {
        isShareExpertRankFlag_ = true;
    }

    axisMaxBS_ = globalBS_ / epWorldSizeOriginal_;
    sharedExpertNum_ = tilingData->moeDistributeDispatchZeroBufferInfo.sharedExpertNum;
    if (sharedExpertNum_ > 0) {
        rankNumPerSharedExpert_ = sharedExpertRankNum_ / sharedExpertNum_;
    }
    moeExpertRankNum_ = epWorldSize_ - sharedExpertRankNum_;
    moeExpertNumPerRank_ = moeExpertNum_ / moeExpertRankNum_;

    xGMTensor_.SetGlobalBuffer((__gm__ XType *)x);
    expertIdsGMTensor_.SetGlobalBuffer((__gm__ int32_t *)expertIds);
    expandIdxGMTensor_.SetGlobalBuffer((__gm__ int32_t *)(expandIdxOut));
    expandXOutGM_ = expandXOut;
    dynamicScalesOutGM_ = dynamicScalesOut;
    sendCountsOutGM_ = sendCountsOut;  // 无GlobalTensor
    sendCountsGlobal.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(sendCountsOutGM_));
    sendTpCountOutGM_ = tpSendCountsOut;
    putOffsetWorkspaceGM_ = workspaceGM;
    recvCntStatusGt.SetGlobalBuffer((__gm__ float *)(putOffsetWorkspaceGM_));
    int32_t recvCntFlagOffset = moeExpertNum_ * epWorldSize_ * sizeof(float) * 8;   // * 8 for GM UB_Alignment with 32B
    int32_t allExpertTokenNumsOffset = recvCntFlagOffset + (moeExpertNum_ * epWorldSize_) * sizeof(int32_t);
    allExpertTokenNumsGMTensor_.SetGlobalBuffer((__gm__ int32_t *)(putOffsetWorkspaceGM_ + allExpertTokenNumsOffset));
    
    hOutSize_ = axisH_ * sizeof(ExpandXOutType);
    hOutSizeAlign_ = Ceil(hOutSize_, UB_ALIGN) * UB_ALIGN;  // scale起始放置偏移
    uint32_t hScaleSizeAlign = hOutSizeAlign_ + UB_ALIGN;   // 填充三元组起始偏移
    // 实际搬运大小，搬运token_align32B + 32B(float)
    uint32_t hScaleIdxSize = hScaleSizeAlign;
    if (sharedExpertRankNum_ != 0U) {
        sharedUsedAivNum_ = (aivNum_ * sharedExpertNum_) / (axisK_ + sharedExpertNum_);
        if (sharedUsedAivNum_ == 0) {
            sharedUsedAivNum_ = 1;
        }
    }
    expertIdsCnt_ = axisBS_ * axisK_;
    recvWinBlockNum_ = epWorldSize_ * moeExpertNumPerRank_;
    moeUsedAivNum_ = aivNum_ - sharedUsedAivNum_;
    PipeBarrier<PIPE_ALL>();

    if (isShareExpertRankFlag_) {
        rscvStatusNum_ = epWorldSize_;
    } else {
        rscvStatusNum_ = recvWinBlockNum_;
    }
    recStatusNumPerCore_ = rscvStatusNum_ / aivNum_;  // 每个aiv需要处理的专家数
    remainderRankNum_ = rscvStatusNum_ % aivNum_;
    startStatusIndex_ = recStatusNumPerCore_ * aivId_;  // + sharedExpertRankNum_, 每个aiv发送的
    if (aivId_ < remainderRankNum_) {                   // 前remainderRankNum个aiv需要多发1个卡的数据
        recStatusNumPerCore_ += 1;
        startStatusIndex_ += aivId_;
    } else {
        startStatusIndex_ += remainderRankNum_;
    }
    totalExpertNum_ = sharedExpertRankNum_ + moeExpertNum_;
    uint32_t statusBufCntAlign = Ceil(Ceil(totalExpertNum_, aivNum_), 8) * 8;  // 8 = UB_ALIGN / sizeof(int32_t)
    uint32_t statusBufSize = statusBufCntAlign * UB_ALIGN;
    tpipe_->InitBuffer(statusBuf_, statusBufSize);
    totalUsedUB_ += statusBufSize;

    statusTensor_ = statusBuf_.Get<float>();
    statusTensor_.SetValue(0, exp_flag);
    hOutAlignUbSize_ = Ceil(hScaleIdxSize, UB_ALIGN) * UB_ALIGN; 
    uint32_t hFp32Size = axisH_ * sizeof(float);
    uint32_t expertIdsSize = expertIdsCnt_ * sizeof(int32_t);
    uint32_t xActivateMaskSize = axisBS_ * (Ceil(axisK_ * sizeof(bool), UB_ALIGN) * UB_ALIGN) * sizeof(half);
    uint32_t bsAlign256 = Ceil(axisBS_ * sizeof(half), ALIGNED_LEN_256) * ALIGNED_LEN_256 / sizeof(half);
    uint32_t bsKAlign256 = Ceil(expertIdsCnt_ * sizeof(half), ALIGNED_LEN_256) * ALIGNED_LEN_256 / sizeof(half);
    uint32_t expertIdsBufSize = expertIdsSize > bsAlign256 ? expertIdsSize : bsAlign256;
    expertIdsSize = Ceil(expertIdsSize, UB_ALIGN) * UB_ALIGN;
    maxSize_ = hFp32Size > expertIdsSize ? hFp32Size : expertIdsSize;
    maxSize_ = maxSize_ > xActivateMaskSize ? maxSize_ : xActivateMaskSize;
    maxSize_ = maxSize_ > bsKAlign256 ? maxSize_ : bsKAlign256;
    tpipe_->InitBuffer(expertIdsBuf_, expertIdsBufSize);  // BS * K * 4 = 32K
    totalUsedUB_ += expertIdsSize;
    expertIdsTensor_ = expertIdsBuf_.Get<int32_t>();

    uint32_t SendCountBufSize = moeExpertNum_ * sizeof(int32_t);
    tpipe_->InitBuffer(SendCountBuf_, SendCountBufSize);

    tpipe_->InitBuffer(gatherMaskTBuf_, maxSize_);
    totalUsedUB_ += maxSize_;
    workLocalTensor_ = gatherMaskTBuf_.Get<float>();

    if constexpr (DynamicQuant || StaticQuant) {
        QuantInit(scales);
        dstExpBuf_ = receiveDataCastFloatBuf_;  // 内存复用
        subExpBuf_ = smoothScalesBuf_;          // 内存复用
    } else {
        tpipe_->InitBuffer(dstExpBuf_, maxSize_);  // BS * K * 4 = 32K
        totalUsedUB_ += maxSize_;
        tpipe_->InitBuffer(subExpBuf_, maxSize_);  // BS * K * 4 = 32K
        totalUsedUB_ += maxSize_;
        uint32_t tmpTotalUB = totalUsedUB_ + hOutAlignUbSize_ * BUFFER_NUM;
        bufferNum_ = tmpTotalUB > MAX_UB_SIZE ? BUFFER_SINGLE : BUFFER_NUM;
        tpipe_->InitBuffer(xQueue_, bufferNum_, hOutAlignUbSize_);  // 7k*2 + 32 + 12
    }

    dstExpIdTensor_ = dstExpBuf_.Get<int32_t>();
    subExpIdTensor_ = subExpBuf_.Get<int32_t>();

    uint32_t axisHCommu = hScaleIdxSize / sizeof(ExpandXOutType);  // 有效搬运长度
    xCopyParams_ = {1U, static_cast<uint32_t>(axisH_ * sizeof(XType)), 0U, 0U, 0U};
    expandXCopyParams_ = {1U, static_cast<uint32_t>(hOutSizeAlign_), 0U, 0U, 0U};
    scaleCopyParams_ = {1U, sizeof(float), 0U, 0U, 0U};  
}

template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeDispatchZeroBuffer<TemplateMC2TypeFunc>::QuantInit(GM_ADDR scales)
{
    uint32_t hAlignSize = Ceil(axisH_ * sizeof(XType), UB_ALIGN) * UB_ALIGN;
    tpipe_->InitBuffer(receiveDataCastFloatBuf_, maxSize_);  // max{28K, BS * K * 4B}
    totalUsedUB_ += maxSize_;
    tpipe_->InitBuffer(smoothScalesBuf_, maxSize_);
    totalUsedUB_ += maxSize_;
    smoothScalesTensor_ = smoothScalesBuf_.Get<float>();
    if constexpr (DynamicQuant) {
        tpipe_->InitBuffer(rowMaxBuf_, UB_ALIGN);  // 32B
    }
    uint32_t tmpTotalUB = totalUsedUB_ + BUFFER_NUM * hAlignSize + hOutAlignUbSize_ * BUFFER_NUM;
    bufferNum_ = tmpTotalUB > MAX_UB_SIZE ? BUFFER_SINGLE : BUFFER_NUM;
    tpipe_->InitBuffer(xInQueue_, bufferNum_, hAlignSize);         // 14K * 2
    tpipe_->InitBuffer(xOutQueue_, bufferNum_, hOutAlignUbSize_);  // 7K * 2 + 32 + 6
    // PRINTF("[----------- CHECK QuantInit ---------]\n");
}

template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeDispatchZeroBuffer<TemplateMC2TypeFunc>::SplitToCore(uint32_t curSendCnt,
                                                                                 uint32_t curUseAivNum,
                                                                                 uint32_t &startTokenId,
                                                                                 uint32_t &endTokenId,
                                                                                 uint32_t &sendTokenNum, bool isFront)
{
    sendTokenNum = curSendCnt / curUseAivNum;                // 每个aiv需要发送的token数(也可以是其他数)
    uint32_t remainderTokenNum = curSendCnt % curUseAivNum;  // 余数
    uint32_t newAivId;
    if (isFront) {
        newAivId = aivId_;
    } else {
        newAivId = aivId_ - moeUsedAivNum_;                 // 由于是后面的核作为发送的共享专家，因此需要换算
    }
    startTokenId = sendTokenNum * newAivId;                 // 每个aiv发送时的起始rankid
    if (newAivId < remainderTokenNum) {                     // 前remainderRankNum个aiv需要多发1个卡的数据
        sendTokenNum += 1;
        startTokenId += newAivId;
    } else {
        startTokenId += remainderTokenNum;
    }
    endTokenId = startTokenId + sendTokenNum;
}

template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeDispatchZeroBuffer<TemplateMC2TypeFunc>::CalTokenSendExpertCnt(uint32_t dstExpertId,
                                                                                           int32_t calCnt,
                                                                                           int32_t &curExpertCnt)
{
    Duplicate<int32_t>(dstExpIdTensor_, dstExpertId, calCnt);
    PipeBarrier<PIPE_V>();
    Sub(subExpIdTensor_, expertIdsTensor_, dstExpIdTensor_, calCnt);
    PipeBarrier<PIPE_V>();
    LocalTensor<float> tmpFp32 = subExpIdTensor_.ReinterpretCast<float>();
    LocalTensor<float> tmpoutFp32 = dstExpIdTensor_.ReinterpretCast<float>();
    Abs(tmpoutFp32, tmpFp32, calCnt);
    PipeBarrier<PIPE_V>();
    Mins(subExpIdTensor_, dstExpIdTensor_, 1, calCnt);
    PipeBarrier<PIPE_V>();
    ReduceSum<float>(tmpoutFp32, tmpFp32, workLocalTensor_, calCnt);
    SyncFunc<AscendC::HardEvent::V_S>();
    int32_t curOtherExpertCnt = dstExpIdTensor_(0);
    if (calCnt >= curOtherExpertCnt) {
        curExpertCnt = calCnt - curOtherExpertCnt;
    } else {
        curExpertCnt = 0;
    }
}


template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeDispatchZeroBuffer<TemplateMC2TypeFunc>::LocalLayout()
{
    LocalTensor<int32_t> sendCountLt = SendCountBuf_.Get<int32_t>();

    Duplicate<int32_t>(sendCountLt, 0, moeExpertNum_);
    PipeBarrier<PIPE_V>();

    // get expertIds first
    DataCopyExtParams expertIdsCntParams = {1U, static_cast<uint32_t>(expertIdsCnt_ * sizeof(uint32_t)), 0U, 0U, 0U};
    DataCopyPadExtParams<int32_t> expertIdsCntCopyPadParams{false, 0U, 0U, 0U};
    DataCopyPad(expertIdsTensor_, expertIdsGMTensor_, expertIdsCntParams, expertIdsCntCopyPadParams);
    SyncFunc<AscendC::HardEvent::MTE2_S>();

    // 分核
    sendToMoeExpTokenCnt_ = axisBS_ * axisK_;
    uint32_t startTokenId, endTokenId, sendTokenNum;            // 每个aiv发送时的起始rankid
    SplitToCore(sendToMoeExpTokenCnt_, moeUsedAivNum_, startTokenId, endTokenId, sendTokenNum);
    if (startTokenId >= sendToMoeExpTokenCnt_) {
        return;
    }

    for (int32_t i = startTokenId; i < endTokenId; ++i) {
        int32_t expertIdx = static_cast<int32_t>(expertIdsTensor_(i));
        int32_t curCnt = sendCountLt.GetValue(expertIdx) + 1;
        sendCountLt.SetValue(expertIdx, curCnt);
    }

    PipeBarrier<PIPE_V>();
    AscendC::SetAtomicAdd<int32_t>();
    uint32_t sendSize = moeExpertNum_ * sizeof(int32_t);
    const DataCopyExtParams sendCountDataCopyParams{1U, sendSize, 0U, 0U, 0U};

    SyncFunc<AscendC::HardEvent::V_MTE3>();
    DataCopyPad(LocalNotifyDataTensor_, sendCountLt, sendCountDataCopyParams);
    AscendC::SetAtomicNone();
    PipeBarrier<PIPE_MTE3>();
}

template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeDispatchZeroBuffer<TemplateMC2TypeFunc>::SetLayoutStatus(){
    // set local layout Complete Flag to all other Ranks on layoutStatusTensor_
    uint32_t rankNumPerBlock = 0U, startRankId = 0U, endRankId = 0U;
    SplitToCore(epWorldSize_, moeUsedAivNum_, startRankId, endRankId, rankNumPerBlock);
    if (rankNumPerBlock == 0U) {
        return;
    }

    for (uint32_t targetRankId = startRankId; targetRankId < endRankId; targetRankId++) {
        shmem_put_float_mem_nbi(layoutStatusTensor_[epRankId_], statusTensor_, 1, targetRankId);
    }
    SyncFunc<AscendC::HardEvent::MTE3_S>();
}


template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeDispatchZeroBuffer<TemplateMC2TypeFunc>::SendCountNotify()
{
    uint32_t rankNumPerBlock = 0U, startRankId = 0U, endRankId = 0U;
    SplitToCore(epWorldSize_, moeUsedAivNum_, startRankId, endRankId, rankNumPerBlock);
    if (rankNumPerBlock == 0U) {
        return;
    }

    tpipe_->InitBuffer(layoutWaitStatusBuf_, 32);           // only 32B
    AscendC::GlobalTensor<int32_t> gmRemoteDataGt;
    for (uint32_t targetRankId = startRankId; targetRankId < endRankId; targetRankId++) {
        float curVal = static_cast<float>(0);
        layoutStatusFp32Tensor_ = layoutWaitStatusBuf_.Get<float>();
        // only check current targetRank        
        DataCopyParams intriParams{1, static_cast<uint16_t>(1 * sizeof(uint32_t)), 0, 0};

        while (curVal != exp_flag) {
            DataCopy(layoutStatusFp32Tensor_, layoutStatusTensor_[targetRankId], intriParams);
            SyncFunc<AscendC::HardEvent::MTE2_S>();
            curVal = layoutStatusFp32Tensor_.GetValue(0);
        }

        shmem_get_int32_mem_nbi(allExpertTokenNumsGMTensor_[targetRankId * moeExpertNum_], LocalNotifyDataTensor_, moeExpertNum_, targetRankId);
        PipeBarrier<PIPE_ALL>();
        // Flag 也 get 一下
        shmem_get_float_mem_nbi(recvCntStatusGt[targetRankId], LocalNotifyStatusTensor_, 1, targetRankId);
    }
    SyncFunc<AscendC::HardEvent::MTE3_S>();
}

template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeDispatchZeroBuffer<TemplateMC2TypeFunc>::WaitNotify()
{
    uint32_t rankNumPerBlock = 0U, startRankId = 0U, endRankId = 0U;
    SplitToCore(epWorldSize_, moeUsedAivNum_, startRankId, endRankId, rankNumPerBlock);
    if (rankNumPerBlock == 0U) {
        return;
    }
    // WaitNotify Flag
    NotifyBufInit();

    LocalTensor<float> gatherNotifyTensor = gatherNotiStatusBuf_.Get<float>();
    LocalTensor<float> statusNotifyTensor = notiStatusBuf_.GetWithOffset<float>(UB_ALIGN / sizeof(float), UB_ALIGN);
    notifyStatusFp32Tensor_ = notifyWaitStatusBuf_.Get<float>();
    uint32_t mask = 1;  // gatherMask + sum 相关参数
    DataCopyParams intriParams{1, static_cast<uint16_t>(rankNumPerBlock * sizeof(uint32_t)), 0, 0};

    float sumOfFlag = static_cast<float>(-1);
    float compareTarget = static_cast<float>(exp_flag) * rankNumPerBlock;

    while (sumOfFlag != compareTarget) {
        DataCopy(notifyStatusFp32Tensor_, recvCntStatusGt[startRankId * rankNumPerBlock], intriParams);
        SyncFunc<AscendC::HardEvent::MTE2_V>();
        ReduceSum(statusNotifyTensor, notifyStatusFp32Tensor_, gatherNotifyTensor, mask, rankNumPerBlock, 1);
        SyncFunc<AscendC::HardEvent::V_S>();
        sumOfFlag = statusNotifyTensor.GetValue(0);
    }

    // Reload recvDataTensor
    recvDataAlignLen_ = Ceil(moeExpertNum_ * epWorldSize_ * sizeof(int32_t), UB_ALIGN_SIZE) * UB_ALIGN_SIZE;
    tpipe_->InitBuffer(recvDataBuf_, recvDataAlignLen_);

    recvDataTensor_ = recvDataBuf_.Get<int32_t>();
    DataCopyExtParams recvDataParams = {1U, static_cast<uint32_t>(recvDataAlignLen_), 0, 0, 0};
    DataCopyPadExtParams<int32_t> DataCopyPadExtParams{false, 0U, 0U, 0U};
    DataCopyPad(recvDataTensor_, allExpertTokenNumsGMTensor_, recvDataParams, DataCopyPadExtParams);
    PipeBarrier<PIPE_ALL>();

    tpipe_->InitBuffer(sendCountBuf_, Ceil(moeExpertNum_ * sizeof(int32_t), UB_ALIGN_SIZE) * UB_ALIGN_SIZE);
    LocalTensor<int32_t> recvTokenLt = sendCountBuf_.Get<int32_t>();

    for (uint32_t rank = startRankId; rank < endRankId; ++rank) {
        // 基于 recvDataTensor_ 每卡求前缀和
        ReorderRecvDataOutput(rank, recvTokenLt, true);  // localExpNum * ranks
        SyncFunc<AscendC::HardEvent::MTE2_MTE3>();
        DataCopyExtParams copyParams{1, static_cast<uint32_t>(moeExpertNum_ * sizeof(int32_t)), 0, 0, 0};
        DataCopyPad(sendCountsGlobal[rank * moeExpertNum_], recvTokenLt, copyParams);
    }
}

template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeDispatchZeroBuffer<TemplateMC2TypeFunc>::ReorderRecvDataOutput(
    int32_t rankId, LocalTensor<int32_t> &transLt, bool isCumSum)
{
    // SyncFunc<AscendC::HardEvent::MTE3_S>();
    uint32_t moeExpertPerRankNum = moeExpertNum_ / epWorldSize_;
    uint32_t startExpId = rankId * moeExpertPerRankNum;
    uint32_t endExpId = rankId * moeExpertPerRankNum + moeExpertPerRankNum;

    SyncFunc<AscendC::HardEvent::V_S>();
    SyncFunc<AscendC::HardEvent::MTE2_S>();
    // 对recv_data进行转置
    int32_t prefixSum = 0;  // 每卡求前缀和，调整为偏移，起始偏移从0开始
    for (uint32_t expId = startExpId; expId < endExpId; ++expId) {
        for (uint32_t srcRank = 0; srcRank < epWorldSize_; ++srcRank) {
            uint32_t index = (expId - startExpId) * epWorldSize_ + srcRank;
            uint32_t pairIdx = srcRank * moeExpertNum_ + expId;

            int32_t curRecvCount = recvDataTensor_(pairIdx);
            prefixSum += curRecvCount;                             // 先累加再更新前缀，这样就是Count不是Offset
            // 后面是用Offset的时候需要从0开始取
            transLt(index) = isCumSum ? prefixSum : curRecvCount;  // 根据是否需要前缀和进行填充
            // prefixSum += curRecvCount;
        }
    }
    PipeBarrier<PIPE_ALL>();
    SyncFunc<AscendC::HardEvent::S_MTE2>();
}

/*
共享专家卡：所有核用于给moe专家发送数据
moe专家卡：部分核用于给共享专家发送数据，部分核用于给moe专家发送数据
*/
// Mix of AlltoAllDispatch and LocalWindowCopy
template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeDispatchZeroBuffer<TemplateMC2TypeFunc>::InputToDstOutput()
{
    bool isSendShared = (aivId_ >= moeUsedAivNum_) && (sharedExpertRankNum_ != 0);
    if (isSendShared) {
        SendToSharedExpert();
        return;
    }
    DataCopyExtParams expertIdsCntParams = {1U, static_cast<uint32_t>(expertIdsCnt_ * sizeof(uint32_t)), 0U, 0U, 0U};
    DataCopyPadExtParams<int32_t> expertIdsCntCopyPadParams{false, 0U, 0U, 0U};
    DataCopyPad(expertIdsTensor_, expertIdsGMTensor_, expertIdsCntParams, expertIdsCntCopyPadParams);
    SyncFunc<AscendC::HardEvent::MTE2_S>();
    SendToMoeExpert();
}

template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeDispatchZeroBuffer<TemplateMC2TypeFunc>::SendToMoeExpert()
{
    uint32_t startTokenId, endTokenId, sendTokenNum;  // 每个aiv发送时的起始rankid
    SplitToCore(sendToMoeExpTokenCnt_, moeUsedAivNum_, startTokenId, endTokenId, sendTokenNum);
    if (startTokenId >= sendToMoeExpTokenCnt_) {
        return;
    }
    
    DataCopyPadExtParams<XType> copyPadExtParams{false, 0U, 0U, 0U};
    int32_t tokenIndex = 0;
    int32_t topKIndex = 0;
    uint32_t dstExpertId = 0;
    putOffsetAlignSize = Ceil(epWorldSize_ * moeExpertNum_ * sizeof(int32_t), UB_ALIGN) * UB_ALIGN;  // 4 * ranks * moeNum
    tpipe_->InitBuffer(putOffsetBuf, putOffsetAlignSize);
    putOffsetTensor_ = putOffsetBuf.Get<int32_t>();
    DataCopyExtParams putOffsetParams = {1U, static_cast<uint32_t>(epWorldSize_ * moeExpertNum_ * sizeof(uint32_t)), 0U,
                                         0U, 0U};
    DataCopyPadExtParams<int32_t> putOffsetCopyPadParams{false, 0U, 0U, 0U};
    // putOffsetGT ---> recvCntGt_ ---> 可以放在 workSpaceGM上
    DataCopyPad(putOffsetTensor_, sendCountsGlobal, putOffsetParams, putOffsetCopyPadParams);
    SyncFunc<AscendC::HardEvent::MTE2_S>();

    tpipe_->InitBuffer(expandIdsBuf_, sendTokenNum * sizeof(uint32_t));  // BS * K * 4 = 32K
    expandIdsTensor_ = expandIdsBuf_.Get<int32_t>();

    for (int32_t index = startTokenId; index < endTokenId; ++index) {
        int32_t tokenIndex = index / axisK_;
        int32_t topKIndex = index % axisK_;

        uint32_t dstExpertId = expertIdsTensor_(index);
        int32_t dstRankId = dstExpertId / moeExpertNumPerRank_ + sharedExpertRankNum_;
        uint32_t localExpertId = dstExpertId % moeExpertNumPerRank_;
        
        int32_t curExpertCnt = 0;
        
        if ((tokenIndex > 0) && (index > 0)) {
            // 用来统计已经有当前多少个 token 选择了这个 dstExpert 返回 curExpertCnt 是要放 token 的位置
            CalTokenSendExpertCnt(dstExpertId, index, curExpertCnt);  // curExpertCnt = sendTokenIdxSmall[index]
        }

        // LocalTensor that save smallIdx of Token tokenIndex on dstExpert
        expandIdsTensor_.SetValue(index - startTokenId, curExpertCnt);
        // 对端output的大偏移，不同专家及不同rank来源间的，本卡需要放置给该rank的token大偏移，定位到专家和来源rank
        uint32_t offsetIdx = dstExpertId * epWorldSize_ + epRankId_;
        uint32_t col = offsetIdx % moeExpertNum_;
        int32_t dstExpertOffset = (col == 0) ? 0 : putOffsetTensor_(offsetIdx - 1);   // Count 转 Offset
        // 再取小偏移
        auto dstPtr = reinterpret_cast<__gm__ uint8_t *>(shmem_ptr(expandXOutGM_, dstRankId));
        dstWinGMTensor.SetGlobalBuffer((__gm__ ExpandXOutType *)(dstPtr + hOutSizeAlign_ * (dstExpertOffset + curExpertCnt)));

        if constexpr (DynamicQuant || StaticQuant) {
            auto dstScalePtr = reinterpret_cast<__gm__ uint8_t *>(shmem_ptr(dynamicScalesOutGM_, dstRankId));
            dstScaleGMTensor.SetGlobalBuffer((__gm__ float *)(dstScalePtr + sizeof(float) * (dstExpertOffset + curExpertCnt)));
            xInTensor_ = xInQueue_.AllocTensor<XType>();   
            DataCopyPad(xInTensor_, xGMTensor_[tokenIndex * axisH_], xCopyParams_, copyPadExtParams);   
            xInQueue_.EnQue(xInTensor_);
            xInTensor_ = xInQueue_.DeQue<XType>();
            xOutTensor_ = xOutQueue_.AllocTensor<ExpandXOutType>();
            QuantProcess();
            xOutQueue_.EnQue(xOutTensor_);
            xOutTensor_ = xOutQueue_.DeQue<ExpandXOutType>();

            DataCopyPad(dstWinGMTensor, xOutTensor_, expandXCopyParams_);  // 拷贝token
            LocalTensor<float> xOutFp32Tensor = xOutTensor_.template ReinterpretCast<float>();
            DataCopyPad(dstScaleGMTensor, xOutFp32Tensor[hOutSizeAlign_ / sizeof(float)], scaleCopyParams_); // 拷贝Scale
            xOutQueue_.FreeTensor<ExpandXOutType>(xOutTensor_);
        } else {
            xTmpTensor_ = xQueue_.AllocTensor<ExpandXOutType>();
            DataCopyPad(xTmpTensor_, xGMTensor_[tokenIndex * axisH_], xCopyParams_, copyPadExtParams);
            xQueue_.EnQue(xTmpTensor_);
            xTmpTensor_ = xQueue_.DeQue<ExpandXOutType>();
            DataCopyPad(dstWinGMTensor, xTmpTensor_, expandXCopyParams_);
            xQueue_.FreeTensor<ExpandXOutType>(xTmpTensor_);
        }
    }

    DataCopyExtParams expandIdxParams = {1U, static_cast<uint32_t>(sendTokenNum * sizeof(uint32_t)), 0U, 0U, 0U};
    DataCopyPad(expandIdxGMTensor_[startTokenId], expandIdsTensor_, expandIdxParams);
}

template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeDispatchZeroBuffer<TemplateMC2TypeFunc>::SendToSharedExpert()
{
    uint32_t startTokenId, endTokenId, sendTokenNum;  // 每个aiv发送时的起始rankid
    SplitToCore(axisBS_, sharedUsedAivNum_, startTokenId, endTokenId, sendTokenNum, false);
    if (startTokenId >= axisBS_) {
        return;
    }

    DataCopyPadExtParams<XType> copyPadExtParams{false, 0U, 0U, 0U};
    for (uint32_t tokenIndex = startTokenId; tokenIndex < endTokenId; ++tokenIndex) {
        uint32_t temp = (epRankId_ * axisBS_) / sharedExpertRankNum_;
        // Target Shared Expert Rank --> 在共享专家内做均分
        uint32_t moeOnShareRank = Ceil((tokenIndex + 1 + temp) * sharedExpertRankNum_, axisBS_) - 1 - epRankId_;
        // 发给该共享专家已经有多少token数据 --> 还是根据负载均衡的计算方式推导
        uint32_t preCnt = (moeOnShareRank + epRankId_) * axisBS_ / sharedExpertRankNum_ - epRankId_\
                            * axisBS_ / sharedExpertRankNum_;

        auto dstPtr = reinterpret_cast<__gm__ uint8_t *>(shmem_ptr(expandXOutGM_, moeOnShareRank));
        dstWinGMTensor.SetGlobalBuffer(
            (__gm__ ExpandXOutType *)(dstPtr + hOutSizeAlign_ * epRankId_));

        PRINTF("[----------- Check dstShareRankId %d ------------] from Rank %d of Core %d\n", moeOnShareRank, epRankId_, aivId_);

        // if constexpr (DynamicQuant || StaticQuant) {
        //     auto dstScalePtr = reinterpret_cast<__gm__ uint8_t *>(shmem_ptr(dynamicScalesOutGM_, moeOnShareRank));
        //     dstScaleGMTensor.SetGlobalBuffer((__gm__ float *)(dstScalePtr));
        //     xInTensor_ = xInQueue_.AllocTensor<XType>();
        //     DataCopyPad(xInTensor_, xGMTensor_[tokenIndex * axisH_], xCopyParams_, copyPadExtParams);
        //     xInQueue_.EnQue(xInTensor_);
        //     xInTensor_ = xInQueue_.DeQue<XType>();
        //     xOutTensor_ = xOutQueue_.AllocTensor<ExpandXOutType>();
        //     QuantProcess();
        //     xOutQueue_.EnQue(xOutTensor_);
        //     xOutTensor_ = xOutQueue_.DeQue<ExpandXOutType>();

        //     if (isShareExpertRankFlag_) {
        //         xOutFp32Tensor_ = xOutTensor_.template ReinterpretCast<float>();
        //         DataCopyExtParams dataCopyParamsFloat = {1U, sizeof(float), 0U, 0U, 0U};
        //         DataCopyPad(dstScaleGMTensor[tokenIndex], xOutFp32Tensor_[axisH_ / sizeof(float)],
        //                     dataCopyParamsFloat);
        //         DataCopy(dstWinGMTensor[tokenIndex * axisH_], xOutTensor_, axisH_); // 约束对齐
        //     } else {
        //         DataCopy(dstWinGMTensor[(tokenIndex - preCnt) * axisH_], xOutTensor_, axisH_); // 约束对齐
        //     }
        //     xOutQueue_.FreeTensor(xOutTensor_);
        // } else {
        //     xTmpTensor_ = xQueue_.AllocTensor<ExpandXOutType>();
        //     DataCopyPad(xTmpTensor_, xGMTensor_[tokenIndex * axisH_], expandXCopyParams_, copyPadExtParams);
        //     xQueue_.EnQue(xTmpTensor_);
        //     xTmpTensor_ = xQueue_.DeQue<ExpandXOutType>();
        //     // 如果自己 --> 直接 output
        //     if (isShareExpertRankFlag_) {
        //         DataCopy(dstWinGMTensor[tokenIndex * axisH_], xTmpTensor_, axisH_);
        //     } else {
        //         // 否则发到对端对应位置上
        //         DataCopy(dstWinGMTensor[(tokenIndex - preCnt) * axisH_], xTmpTensor_, axisH_); // 约束对齐
        //     }
        //     xQueue_.FreeTensor<ExpandXOutType>(xTmpTensor_);
        // }
    }
}

template <TemplateMC2TypeClass>
__aicore__ inline void
ShmemMoeDistributeDispatchZeroBuffer<TemplateMC2TypeFunc>::ReduceMaxInplace(const LocalTensor<float> &srcLocal, uint32_t count)
{
    uint64_t repsFp32 = count >> 6;        // 6 is count / elemPerRefFp32
    uint64_t offsetsFp32 = repsFp32 << 6;  // 6 is repsFp32 * elemPerRefFp32
    uint64_t remsFp32 = count & 0x3f;      // 0x3f 63, count % elemPerRefFp32
    const uint64_t elemPerRefFp32 = 64UL;  // 256 bit / sizeof(float)
    if (likely(repsFp32 > 1)) {
        // 8 is rep stride
        Max(srcLocal, srcLocal[elemPerRefFp32], srcLocal, elemPerRefFp32, repsFp32 - 1, {1, 1, 1, 0, 8, 0});
        PipeBarrier<PIPE_V>();
    }
    if (unlikely(remsFp32 > 0) && unlikely(offsetsFp32 > 0)) {
        Max(srcLocal, srcLocal[offsetsFp32], srcLocal, remsFp32, 1, {1, 1, 1, 0, 8, 0});
        PipeBarrier<PIPE_V>();
    }
    uint32_t mask = (repsFp32 > 0) ? elemPerRefFp32 : count;
    // 8 is rep stride
    WholeReduceMax(srcLocal, srcLocal, mask, 1, 8, 1, 8);
}

template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeDispatchZeroBuffer<TemplateMC2TypeFunc>::QuantProcess()
{
    float dynamicScale = 0.0;
    LocalTensor<float> floatLocalTemp;
    floatLocalTemp = receiveDataCastFloatBuf_.Get<float>();

    Cast(floatLocalTemp, xInTensor_, RoundMode::CAST_NONE, axisH_);
    xInQueue_.FreeTensor<XType>(xInTensor_);
    PipeBarrier<PIPE_V>();

    if constexpr (DynamicQuant) {
        LocalTensor<float> floatLocalAbsTemp = smoothScalesBuf_.Get<float>();
        rowMaxTensor_ = rowMaxBuf_.Get<float>();

        Abs(floatLocalAbsTemp, floatLocalTemp, axisH_);
        PipeBarrier<PIPE_V>();
        ReduceMaxInplace(floatLocalAbsTemp, axisH_);

        SyncFunc<AscendC::HardEvent::V_S>();
        dynamicScale = float(127.0) / floatLocalAbsTemp.GetValue(0);
        SyncFunc<AscendC::HardEvent::S_V>();
        Muls(floatLocalTemp, floatLocalTemp, dynamicScale, axisH_);
        PipeBarrier<PIPE_V>();
    }
    LocalTensor<half> halfLocalTemp = floatLocalTemp.ReinterpretCast<half>();
    LocalTensor<int32_t> int32LocalTemp = floatLocalTemp.ReinterpretCast<int32_t>();

    Cast(int32LocalTemp, floatLocalTemp, RoundMode::CAST_RINT, axisH_);
    PipeBarrier<PIPE_V>();
    SetDeqScale((half)1.000000e+00f);
    PipeBarrier<PIPE_V>();
    Cast(halfLocalTemp, int32LocalTemp, RoundMode::CAST_ROUND, axisH_);
    PipeBarrier<PIPE_V>();
    Cast(xOutTensor_, halfLocalTemp, RoundMode::CAST_TRUNC, axisH_);

    floatLocalTemp = xOutTensor_.template ReinterpretCast<float>();
    floatLocalTemp.SetValue(hOutSizeAlign_ / sizeof(float), float(1.0) / dynamicScale);  // int8->float32
}

template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeDispatchZeroBuffer<TemplateMC2TypeFunc>::NotifyBufInit()
{
    uint32_t waitStatusBufSize = (((recStatusNumPerCore_ * UB_ALIGN) > 256) ? (recStatusNumPerCore_ * UB_ALIGN) : 256);
    uint64_t recStatusNumPerCoreSpace = Ceil(recStatusNumPerCore_ * sizeof(float), UB_ALIGN) * UB_ALIGN;
    uint64_t recvWinBlockNumSpace = recvWinBlockNum_ * sizeof(float);
    uint64_t gatherMaskOutSize =
        (recStatusNumPerCoreSpace > recvWinBlockNumSpace) ? recStatusNumPerCoreSpace : recvWinBlockNumSpace;
    tpipe_->InitBuffer(notifyWaitStatusBuf_, waitStatusBufSize);
    tpipe_->InitBuffer(gatherNotiStatusBuf_, gatherMaskOutSize);      // recStatusNumPerCore_32对齐后大小  * 32B
    tpipe_->InitBuffer(notiStatusBuf_, UB_ALIGN * 3);                 // 96B
}

template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeDispatchZeroBuffer<TemplateMC2TypeFunc>::CleanUp()
{    
    clearAlign_ = Ceil(moeExpertNum_ * sizeof(int32_t), UB_ALIGN) * UB_ALIGN;
    tpipe_->InitBuffer(CleartBuf, clearAlign_);

    // LocalNotifyDataTensor_ 的累加清零 ---> TODO：分核清零
    // uint32_t epNumPerBlock = 0U, startEpId = 0U, endEpId = 0U;
    // SplitToCore(moeExpertNum_, coreNum_, startEpId, endEpId, epNumPerBlock);
    // PRINTF("[----------- CHECK [startEpId, endEpId] and epNumPerBlock ----------- ] of Rank %d Core %d is [%d, %d], %d\n", 
    //         epRankId_, aivId_, startEpId, endEpId, epNumPerBlock);
    // LocalTensor<int32_t> cleanTempLt_ = CleartBuf.GetWithOffset<int32_t>(epNumPerBlock, 0);
    // Duplicate<int32_t>(cleanTempLt_, 0, epNumPerBlock);
    // PipeBarrier<PIPE_ALL>();
    // DataCopy(LocalNotifyDataTensor_[startEpId], cleanTempLt_, epNumPerBlock);
    // PipeBarrier<PIPE_ALL>();
    // SyncAll<true>();

    if (aivId_ == 0) {
        LocalTensor<int32_t> cleanTempLt_ = CleartBuf.GetWithOffset<int32_t>(moeExpertNum_, 0);
        Duplicate<int32_t>(cleanTempLt_, 0, moeExpertNum_);
        PipeBarrier<PIPE_ALL>();
        DataCopy(LocalNotifyDataTensor_, cleanTempLt_, moeExpertNum_);
        PipeBarrier<PIPE_ALL>();
    }

    SyncAll<true>();
}

template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeDispatchZeroBuffer<TemplateMC2TypeFunc>::Process()
{
    if ASCEND_IS_AIV {  // 全aiv处理
        LocalLayout();
        SyncAll<true>();
        SetLayoutStatus();  // set local Rank flag to all remote Ranks
        SendCountNotify();
        SyncAll<true>();
        WaitNotify();
        SyncAll<true>();
        InputToDstOutput();
        SyncAll<true>();
        CleanUp();
        // 85.6 us --> 56.6 us

        // if (aivId_ == 0) {
        //     uint32_t array[] = {epWorldSize_, moeExpertNum_};
        //     uint32_t count = moeExpertNum_ * epWorldSize_;
        //     AscendC::ShapeInfo shapeInfo(2, array);
        //     PRINTF("[--------------- Calling SetStatus -----------------] of Core %d\n", aivId_);
        //     // DumpTensor(sendCountsGlobal, 1311, moeExpertNum_ * epWorldSize_, shapeInfo);
        //     DumpTensor(LocalNotifyStatusTensor_, 1311, 16);
        // }
    }
}

}  // namespace MoeDistributeDispatchZeroBufferImpl
#endif  // MOE_DISTRIBUTE_DISPATCH_ZeroBuffer_H
