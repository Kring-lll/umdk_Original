/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: ShmemMoeDistributeCombineZeroBuffer operator kernel function header file, for a3
 * Author: Hu Tianlun
 * Create: 2026-01-22
 * Note:
 * History: 2026-01-22 create ShmemMoeDistributeCombineZeroBuffer operator kernel function header file, for a3
 */
#ifndef SHMEM_MOE_DISTRIBUTE_COMBINE_ZEROBUFFER_H
#define SHMEM_MOE_DISTRIBUTE_COMBINE_ZEROBUFFER_H

#include "kernel_operator.h"
#include "kernel_tiling/kernel_tiling.h"
#include "shmem_moe_distribute_v2_base.h"
#include "shmem_moe_distribute_combine_zero_buffer_tiling.h"
#include "shmem_api.h"

using namespace AscendC;
#define SHMEM_PUT_BY_DTYPE(dtype, ...)                            \
    do {                                                          \
        if constexpr (std::is_same_v<dtype, half>) {              \
            shmem_put_half_mem_nbi(__VA_ARGS__);                  \
        } else if constexpr (std::is_same_v<dtype, bfloat16_t>) { \
            shmem_put_bfloat16_mem_nbi(__VA_ARGS__);              \
        } else if constexpr (std::is_same_v<dtype, float>) {      \
            shmem_put_float_mem_nbi(__VA_ARGS__);                 \
        }                                                         \
    } while (0)

#define SHMEM_GET_BY_DTYPE(dtype, ...)                            \
    do {                                                          \
        if constexpr (std::is_same_v<dtype, half>) {              \
            shmem_get_half_mem_nbi(__VA_ARGS__);                  \
        } else if constexpr (std::is_same_v<dtype, bfloat16_t>) { \
            shmem_get_bfloat16_mem_nbi(__VA_ARGS__);              \
        }                                                         \
    } while (0)

namespace MoeDistributeCombineV2Impl {
using namespace MoeDistributeV2Base;
constexpr uint8_t BUFFER_NUM = 2;                       // 多buf
constexpr uint32_t STATE_OFFSET = 32U;                  // 状态空间偏移地址
constexpr uint32_t STATE_SIZE = 1024UL * 1024UL;        // 1M
constexpr uint32_t COMBINE_STATE_OFFSET = 64U * 1024U;  // 本卡状态空间偏移地址，前面的地址给dispatch用
constexpr uint8_t EP_DOMAIN = 0;
constexpr uint8_t TP_DOMAIN = 1;
constexpr uint32_t FLOAT_PER_UB_ALIGN = 8U;
constexpr uint64_t WIN_STATE_OFFSET = 500UL * 1024UL;
constexpr uint64_t STATE_WIN_OFFSET =
    975UL * 1024UL;  // dispatch 用的是950， 错开 25 是为了预留48*512内存给 combine 用不同的 magic 位置
// constexpr uint64_t STATE_HCCL_OFFSET = 32UL;
constexpr uint64_t STATE_CHECK_OFFSET = 1000UL * 1024UL;
constexpr uint64_t TIMEOUT_DETECTION_THRESHOLD = 50000UL;
constexpr uint64_t CYCLES_PER_US = 50UL;
constexpr uint64_t TIMEOUT_DETECTION_TX_UNITS = 8UL;
constexpr uint32_t EXPAND_IDX_INFO = 3U;  // expand_idx是按3元组保存信息，分别为rank_id token_id topk_id
constexpr uint32_t ALIGNED_LEN = 256U;    // blockReduceMax中，最多支持连续256字节数据参与计算
constexpr float SCALE_PARAM = 127.0;      // 计算量化参数所需的缩放倍数
constexpr uint64_t ALIGNED_LEN_256 = 256UL;
constexpr uint64_t WIN_ADDR_ALIGN = 512UL;
constexpr uint32_t REDUCE_NUM = 8U;
constexpr uint32_t ELASTIC_INFO_OFFSET = 4U;
constexpr uint32_t RANK_LIST_NUM = 2;
constexpr uint8_t EP_WORLD_SIZE_IDX = 1U;
constexpr uint8_t SHARE_RANK_NUM_IDX = 2U;
constexpr uint8_t MOE_NUM_IDX = 3U;
constexpr uint32_t DIM_NUM = 2;
constexpr size_t MASK_CALC_NEED_WORKSPACE = 10UL * 1024UL;
constexpr uint32_t BLOCK_NUM = ALIGNED_LEN / UB_ALIGN;  // blockReduceMax中，最多支持连续256字节数据参与计算
constexpr uint64_t RANK_META_INFO_OFFSET = 1024UL * 1024UL;

#define TemplateMC2TypeClass \
    typename ExpandXType, typename XType, typename ExpandIdxType, bool IsNeedReduceScatter, bool IsInt8Quant
#define TemplateMC2TypeFunc ExpandXType, XType, ExpandIdxType, IsNeedReduceScatter, IsInt8Quant

template <TemplateMC2TypeClass>
class ShmemMoeDistributeCombineZeroBuffer
{
public:
    __aicore__ inline ShmemMoeDistributeCombineZeroBuffer(){};
    __aicore__ inline void Init(GM_ADDR expandX, GM_ADDR expertIds, GM_ADDR expandIdx, GM_ADDR epSendCount,
                                GM_ADDR tpSendCount, GM_ADDR expertScales, GM_ADDR xActiveMask, GM_ADDR sharedExpertX,
                                GM_ADDR elasticInfo, GM_ADDR oriX, GM_ADDR constExpertAlpha1, GM_ADDR constExpertAlpha2,
                                GM_ADDR constExpertV, GM_ADDR XOut, GM_ADDR workspaceGM, TPipe *pipe,
                                const ShmemMoeDistributeCombineZeroBufferTilingData *tilingData);
    __aicore__ inline void Process();

private:
    __aicore__ inline void InputToDstOutput();
    __aicore__ inline void SplitToCore(uint32_t curSendCnt, uint32_t curUseAivNum, uint32_t &startTokenId,
                                       uint32_t &endTokenId, uint32_t &sendTokenNum);
    __aicore__ inline void InitInputAndOutput(GM_ADDR expandX, GM_ADDR expertIds, GM_ADDR expandIdx,
                                              GM_ADDR epSendCount, GM_ADDR expertScales, GM_ADDR xActiveMask,
                                              GM_ADDR sharedExpertX, GM_ADDR elasticInfo, GM_ADDR oriX,
                                              GM_ADDR constExpertAlpha1, GM_ADDR constExpertAlpha2,
                                              GM_ADDR constExpertV, GM_ADDR XOut);
    __aicore__ inline void InitAttrs(const ShmemMoeDistributeCombineZeroBufferTilingData *tilingData);
    __aicore__ inline void InitTilingAttrs(const ShmemMoeDistributeCombineZeroBufferTilingData *tilingData);
    __aicore__ inline void InitElasticInfo(uint32_t &sharedExpertRankNum);
    __aicore__ inline void InitElasticInfoTensor();
    __aicore__ inline void InitInt8Quant();
    __aicore__ inline void AlltoAllBuffInitAndMaskCal();
    __aicore__ inline void ReduceScatterTrans();
    __aicore__ inline void TokenMaskCalCnt();
    __aicore__ inline void ExpertMaskCalCnt();
    __aicore__ inline void GenerateActiveMask(half val);
    __aicore__ inline void MaskSpecialExpert();
    __aicore__ inline void MaskAlign();
    __aicore__ inline void SetWaitTpStatusAndDisPatch();
    __aicore__ inline void CustomAdd(LocalTensor<XType> &dst, LocalTensor<XType> &src0, LocalTensor<XType> &src1);
    __aicore__ inline void ExpertAlltoAllDispatchInnerCopyAdd(uint32_t toRankId, uint32_t tokenId, uint32_t topkId,
                                                              uint32_t tkIndex);
    __aicore__ inline void ExpertAlltoAllDispatchCopyAdd();
    __aicore__ inline void Int8QuantProcess();
    __aicore__ inline void Int8DequantProcess(LocalTensor<XType> &src);
    __aicore__ inline void ProcessConstantExpert(uint32_t tokenIndex, uint32_t const_expert_idx, float scaleVal);
    __aicore__ inline void ProcessCopyExpert(uint32_t tokenIndex, float scaleVal);
    __aicore__ inline void ProcessMoeExpert(uint32_t tokenIndexOffset, uint32_t topkId, float scaleVal);
    __aicore__ inline void LocalWindowCopy();
    __aicore__ inline void BuffInit();
    __aicore__ inline void SplitCoreCal();
    __aicore__ inline void WaitDispatch(uint32_t tokenIndex);
    __aicore__ GM_ADDR GetWinAddrByRankId(const int32_t rankId, const uint8_t domain)
    {
        return (GM_ADDR)(metaInfo_gva_gm) + STATE_WIN_OFFSET + winStatusOffset_ + RANK_META_INFO_OFFSET +
               winDataSizeOffset_;
    }

    __aicore__ GM_ADDR GetWinStateAddrByRankId(const int32_t rankId, const uint8_t domain)
    {
        return (GM_ADDR)(metaInfo_gva_gm) + STATE_WIN_OFFSET + winStatusOffset_;
    }

    __aicore__ inline uint32_t MIN(uint32_t x, uint32_t y)
    {
        return (x < y) ? x : y;
    }

    TPipe *tpipe_{nullptr};
    GlobalTensor<ExpandXType> expandXGT_;
    GlobalTensor<bool> xActiveMaskGM_;
    GlobalTensor<int32_t> expertIdsGM_;
    GlobalTensor<int32_t> expandIdxGM_;
    GlobalTensor<ExpandIdxType> epSendCountGM_;
    GlobalTensor<ExpandIdxType> tpSendCountGM_;
    GlobalTensor<ExpandIdxType> elasticInfoGM_;
    GlobalTensor<float> expertScalesGT_;
    GlobalTensor<XType> srcWinGMTensor;
    GlobalTensor<XType> sharedExpertXGM_;
    GlobalTensor<XType> XOutGT_;
    GlobalTensor<XType> rankWindow_;  // 用于存对端window的变量
    GlobalTensor<XType> tpRankWindow_;
    GlobalTensor<XType> rowTmpGlobal_;
    GlobalTensor<ExpandXType> oriXGM_;
    GlobalTensor<ExpandXType> constExpertAlpha1GM_;
    GlobalTensor<ExpandXType> constExpertAlpha2GM_;
    GlobalTensor<ExpandXType> constExpertVGM_;
    GlobalTensor<uint32_t> selfDataStatusGMTensor_;

    GM_ADDR epWindowGM_;
    GM_ADDR tpWindowGM_;
    GM_ADDR stateGM_;
    GM_ADDR maskCalcWorkspaceGM_;
    GM_ADDR statusDataSpaceGm_;
    // shmem addrs
    GM_ADDR metaInfo_gva_gm;
    GM_ADDR expandXGM_;

    LocalTensor<XType> winTpSendCountTensor_;
    LocalTensor<ExpandXType> gmTpSendCountTensor_;
    LocalTensor<XType> outTensor_;
    LocalTensor<float> tokenReuceTensor_;
    LocalTensor<float> weightedSumTensor_;
    LocalTensor<float> sumTokenTensor_;
    LocalTensor<float> winTpSendCountFloatTensor_;
    LocalTensor<float> gmTpSendCountFloatTensor_;
    LocalTensor<int32_t> elasticInfoTensor_;
    LocalTensor<bool> maskStrideTensor_;
    LocalTensor<bool> maskGenerateTensor_;
    LocalTensor<uint32_t> dataStateLocalTensor_;
    LocalTensor<float> stateResetTensor_;
    LocalTensor<int32_t> globalSendCountLocal_;


    // tiling侧已确保数据上限， 相乘不会越界，因此统一采用uin32_t进行处理
    uint32_t axisBS_{0};
    uint32_t axisH_{0};
    uint32_t axisK_{0};
    uint32_t aivNum_{0};
    uint32_t epWorldSize_{0};
    uint32_t epWorldSizeOriginal_{0};
    uint32_t tpWorldSize_{0};
    uint32_t epRankId_{0};
    uint32_t epRankIdOriginal_{0};
    uint32_t tpRankId_{0};
    uint32_t aivId_{0};  // aiv id
    uint32_t sharedExpertNum_{0};
    uint32_t moeExpertPerRankNum_{0};  // 每张卡部署的moe专家数
    uint32_t moeSendNum_{0};           // moeExpertPerRankNum_ * epWorldSize_
    uint32_t zeroExpertNum_{0};
    uint32_t copyExpertNum_{0};
    uint32_t constExpertNum_{0};
    uint32_t moeExpertNum_{0};
    uint32_t globalBS_{0};
    uint32_t tpStateOffsetOnWin_{0};
    uint32_t bsKNum_{0};
    // uint32_t startTokenId{0};
    // uint32_t endTokenId{0};
    uint32_t sendCntNum_{0};
    uint32_t ubSize_{0};
    uint32_t dataState_{0};
    uint32_t stateOffset_{0};
    uint64_t activeMaskBsCnt_{0};
    uint64_t winDataSizeOffset_{0};
    uint64_t winStatusOffset_{0};
    uint64_t totalWinSize_{0};
    uint32_t selfSendCnt_{0};
    uint32_t tpRemoteSendCnt_{0};
    uint32_t activeMaskAlignSize_{0};
    uint32_t hExpandXTypeSize_{0};
    uint32_t hAlign32Size_{0};
    uint32_t hFloatAlign32Size_{0};
    uint32_t hFloatAlign256Size_{0};
    uint32_t hExpandXAlign32Size_{0};
    uint32_t hAlignWinSize_{0};
    uint32_t hAlignWinCnt_{0};
    uint32_t tokenScaleCnt_{0};
    uint32_t scaleNumAlignSize_{0};
    uint32_t flagRcvCount_{0};
    uint32_t axisBsAlignSize_{0};

    TQueBind<QuePosition::VECIN, QuePosition::VECOUT, 1> moeQueue_;
    TQue<QuePosition::VECIN, 1> moeSumQueue_;
    TQueBind<QuePosition::VECIN, QuePosition::VECOUT, 1> gmTpSendCountQueue_;
    TQue<QuePosition::VECIN, 1> gmTpSendCountInQueue_;
    TQue<QuePosition::VECIN, 1> winTpSendCountInQueue_;
    TQue<QuePosition::VECOUT, 1> xOutQueue_;

    TBuf<> sendCountBuf_;
    TBuf<> readStateBuf_;
    TBuf<> expertScalesBuf_;
    TBuf<> expertIdsBuf_;
    TBuf<> rowTmpFloatBuf_;
    TBuf<> sumFloatBuf_;
    TBuf<> tokenSumBuf_;
    TBuf<> weightedSumBuf_;
    TBuf<> xOutBuf_;

    TBuf<> mulBuf_;
    TBuf<> indexCountsBuf_;
    TBuf<> winTpSendCountFloatBuf_;
    TBuf<> gmTpSendCountFloatBuf_;
    TBuf<> tokenBuf_;
    TBuf<> xActMaskTBuf_;
    TBuf<> xActMaskCastTBuf_;
    TBuf<> tokenTargetTBuf_;
    TBuf<> validBsIndexTBuf_;
    TBuf<> xActMaskSumTBuf_;
    TBuf<> stateBuf_;
    TBuf<> stateResetBuf_;
    TBuf<> expertMaskBuf_;
    TBuf<> elasticInfoBuf_;
    bool isInputTokenMaskFlag_ = false;
    bool isInputExpertMaskFlag_ = false;
    bool hasSharedExpertX_ = false;
    bool hasElasticInfoFlag_ = false;
    bool isScalingDownFlag_ = false;
    bool isShareExpertRankFlag_ = false;
    bool enableSpecialExpert_ = false;

    // int8量化
    TBuf<> xAbsBuf_;
    TBuf<> xMaxBuf_;
    TBuf<> xScaleMulBuf_;

    LocalTensor<int8_t> castLocalTensor_;
    LocalTensor<half> fp16CastTensor_;
    LocalTensor<float> absFloatTensor_;
    LocalTensor<float> reduceMaxFloatTensor_;
    LocalTensor<XType> scaleDivTensor_;
    LocalTensor<float> scaleDivFloatTensor_;
    LocalTensor<float> scaleDupLocalTensor_;
    LocalTensor<XType> sendLocalTensor_;
    LocalTensor<half> tokenTargetTensor_;
    LocalTensor<int32_t> validBsIndexTensor_;
    LocalTensor<bool> expertMaskTensor_;
    LocalTensor<float> expertScalesLocal_;
    LocalTensor<float> rowTmpFloatLocal_;
    LocalTensor<float> mulBufLocal_;
    LocalTensor<float> sumFloatBufLocal_;

    uint32_t mask_{0};
    uint32_t repeatNum_{0};
    uint32_t scaleNum_{0};
    float scaleValFloat_;
};

template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeCombineZeroBuffer<TemplateMC2TypeFunc>::TokenMaskCalCnt()
{
    // 一维mask, 计算得到有效bs数量
    LocalTensor<bool> xActiveMaskTensor = xActMaskTBuf_.Get<bool>();
    LocalTensor<half> tempTensor = xActMaskCastTBuf_.Get<half>();
    LocalTensor<half> sumOutTensor = xActMaskSumTBuf_.Get<half>();
    DataCopyExtParams xActiveMaskParams{1U, static_cast<uint32_t>(axisBS_ * sizeof(bool)), 0U, 0U, 0U};
    DataCopyPadExtParams<bool> xActiveMaskCopyPadParams{false, 0U, 0U, 0U};
    DataCopyPad(xActiveMaskTensor, xActiveMaskGM_, xActiveMaskParams, xActiveMaskCopyPadParams);
    SyncFunc<AscendC::HardEvent::MTE2_V>();
    LocalTensor<int8_t> xActiveMaskInt8Tensor = xActiveMaskTensor.ReinterpretCast<int8_t>();
    Cast(tempTensor, xActiveMaskInt8Tensor, RoundMode::CAST_NONE, axisBS_);
    PipeBarrier<PIPE_V>();
    SumParams params{1, axisBsAlignSize_, axisBS_};
    Sum(sumOutTensor, tempTensor, params);
    SyncFunc<AscendC::HardEvent::V_S>();
    activeMaskBsCnt_ = static_cast<int32_t>(sumOutTensor.GetValue(0));
}

template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeCombineZeroBuffer<TemplateMC2TypeFunc>::ExpertMaskCalCnt()
{
    // 二维mask, 挑选有效token
    uint64_t rsvdCnt = 0;
    uint32_t mask = axisBS_;
    LocalTensor<bool> maskStrideTensor = tokenBuf_.Get<bool>();
    LocalTensor<half> tempTensor = rowTmpFloatBuf_.Get<half>();
    LocalTensor<half> maskTempTensor = sumFloatBuf_.Get<half>();
    LocalTensor<uint8_t> maskTensor = tokenBuf_.Get<uint8_t>();
    LocalTensor<int32_t> bsIndexTensor = mulBuf_.Get<int32_t>();
    LocalTensor<uint32_t> maskTensorInt32 = tokenBuf_.Get<uint32_t>();
    DataCopyExtParams xActiveMaskParams{static_cast<uint16_t>(axisBS_), static_cast<uint32_t>(axisK_ * sizeof(bool)),
                                        0U, 0U, 0U};
    DataCopyPadExtParams<bool> xActiveMaskCopyPadParams{true, 0U, static_cast<uint8_t>(UB_ALIGN - axisK_), 0U};
    SumParams axisBsSumParams{
        1, static_cast<uint32_t>(Ceil(axisBS_ * sizeof(half), UB_ALIGN) * UB_ALIGN / sizeof(half)), axisBS_};
    uint32_t calCnt = Ceil(axisBS_ * sizeof(half), ALIGNED_LEN) * ALIGNED_LEN / sizeof(half);

    Duplicate<half>(maskTempTensor, (half)0, calCnt);
    DataCopyPad(maskStrideTensor, xActiveMaskGM_, xActiveMaskParams, xActiveMaskCopyPadParams);
    SyncFunc<AscendC::HardEvent::MTE2_V>();
    LocalTensor<int8_t> maskStrideInt8Tensor = maskStrideTensor.ReinterpretCast<int8_t>();
    Cast(tempTensor, maskStrideInt8Tensor, RoundMode::CAST_NONE, activeMaskAlignSize_);
    PipeBarrier<PIPE_V>();
    uint32_t innerAlign = Ceil(axisK_ * sizeof(half), UB_ALIGN) * UB_ALIGN / sizeof(half) * BUFFER_NUM;
    SumParams axisKSumParams{axisBS_, innerAlign, axisK_};
    Sum(tokenTargetTensor_, tempTensor, axisKSumParams);
    PipeBarrier<PIPE_V>();
    Mins(maskTempTensor, tokenTargetTensor_, static_cast<half>(1), axisBS_);
    PipeBarrier<PIPE_V>();
    CompareScalar(maskTensor, maskTempTensor, static_cast<half>(1), AscendC::CMPMODE::EQ, calCnt);
    CreateVecIndex(bsIndexTensor, 0, axisBS_);
    PipeBarrier<PIPE_V>();
    GatherMask(validBsIndexTensor_, bsIndexTensor, maskTensorInt32, true, mask, {1, 1, 0, 0}, activeMaskBsCnt_);
}

template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeCombineZeroBuffer<TemplateMC2TypeFunc>::InitInputAndOutput(
    GM_ADDR expandX, GM_ADDR expertIds, GM_ADDR expandIdx, GM_ADDR epSendCount, GM_ADDR expertScales,
    GM_ADDR xActiveMask, GM_ADDR sharedExpertX, GM_ADDR elasticInfo, GM_ADDR oriX, GM_ADDR constExpertAlpha1,
    GM_ADDR constExpertAlpha2, GM_ADDR constExpertV, GM_ADDR XOut)
{
    expandXGM_ = expandX;
    expandXGT_.SetGlobalBuffer((__gm__ ExpandXType *)expandX);
    expertIdsGM_.SetGlobalBuffer((__gm__ ExpandIdxType *)expertIds);
    expandIdxGM_.SetGlobalBuffer((__gm__ int32_t *)expandIdx);
    epSendCountGM_.SetGlobalBuffer((__gm__ int32_t *)epSendCount);
    expertScalesGT_.SetGlobalBuffer((__gm__ float *)expertScales);
    xActiveMaskGM_.SetGlobalBuffer((__gm__ bool *)xActiveMask);
    sharedExpertXGM_.SetGlobalBuffer((__gm__ XType *)sharedExpertX);
    elasticInfoGM_.SetGlobalBuffer((__gm__ int32_t *)elasticInfo);
    oriXGM_.SetGlobalBuffer((__gm__ ExpandXType *)oriX);
    constExpertAlpha1GM_.SetGlobalBuffer((__gm__ ExpandXType *)constExpertAlpha1);
    constExpertAlpha2GM_.SetGlobalBuffer((__gm__ ExpandXType *)constExpertAlpha2);
    constExpertVGM_.SetGlobalBuffer((__gm__ ExpandXType *)constExpertV);

    XOutGT_.SetGlobalBuffer((__gm__ XType *)XOut);
}

template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeCombineZeroBuffer<TemplateMC2TypeFunc>::InitElasticInfo(uint32_t &sharedExpertRankNum)
{
    DataCacheCleanAndInvalid<int32_t, CacheLine::SINGLE_CACHE_LINE, DcciDst::CACHELINE_OUT>(elasticInfoGM_);
    isScalingDownFlag_ = elasticInfoGM_.GetValue(0);
    if (isScalingDownFlag_) {
        epWorldSize_ = elasticInfoGM_.GetValue(EP_WORLD_SIZE_IDX);
        sharedExpertRankNum = elasticInfoGM_.GetValue(SHARE_RANK_NUM_IDX);
        uint32_t moeExpertNum = elasticInfoGM_.GetValue(MOE_NUM_IDX);
        epRankId_ = elasticInfoGM_.GetValue(ELASTIC_INFO_OFFSET + epRankId_);
        moeExpertPerRankNum_ = moeExpertNum / (epWorldSize_ - sharedExpertRankNum);
    }
}

template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeCombineZeroBuffer<TemplateMC2TypeFunc>::InitTilingAttrs(
    const ShmemMoeDistributeCombineZeroBufferTilingData *tilingData)
{
    axisBS_ = tilingData->moeDistributeCombineV2Info.bs;
    axisH_ = tilingData->moeDistributeCombineV2Info.h;
    axisK_ = tilingData->moeDistributeCombineV2Info.k;
    aivNum_ = tilingData->moeDistributeCombineV2Info.aivNum;
    ubSize_ = tilingData->moeDistributeCombineV2Info.totalUbSize;
    globalBS_ = tilingData->moeDistributeCombineV2Info.globalBs;
    hasElasticInfoFlag_ = tilingData->moeDistributeCombineV2Info.hasElasticInfo;
    epWorldSizeOriginal_ = tilingData->moeDistributeCombineV2Info.epWorldSize;
    epRankId_ = tilingData->moeDistributeCombineV2Info.epRankId;
    epRankIdOriginal_ = tilingData->moeDistributeCombineV2Info.epRankId;
    epWorldSize_ = tilingData->moeDistributeCombineV2Info.epWorldSize;
    moeExpertPerRankNum_ = tilingData->moeDistributeCombineV2Info.moeExpertPerRankNum;
    tpWorldSize_ = tilingData->moeDistributeCombineV2Info.tpWorldSize;
    tpRankId_ = tilingData->moeDistributeCombineV2Info.tpRankId;
    totalWinSize_ = tilingData->moeDistributeCombineV2Info.totalWinSize;
    isInputTokenMaskFlag_ = tilingData->moeDistributeCombineV2Info.isTokenMask;
    isInputExpertMaskFlag_ = tilingData->moeDistributeCombineV2Info.isExpertMask;
    hasSharedExpertX_ = tilingData->moeDistributeCombineV2Info.hasSharedExpertX;
    zeroExpertNum_ = tilingData->moeDistributeCombineV2Info.zeroExpertNum;
    copyExpertNum_ = tilingData->moeDistributeCombineV2Info.copyExpertNum;
    constExpertNum_ = tilingData->moeDistributeCombineV2Info.constExpertNum;
    moeExpertNum_ = tilingData->moeDistributeCombineV2Info.moeExpertNum;
    enableSpecialExpert_ = (constExpertNum_ + zeroExpertNum_ + copyExpertNum_ > 0U);
}

template <TemplateMC2TypeClass>
__aicore__ inline void
ShmemMoeDistributeCombineZeroBuffer<TemplateMC2TypeFunc>::InitAttrs(const ShmemMoeDistributeCombineZeroBufferTilingData *tilingData)
{
    InitTilingAttrs(tilingData);
    uint32_t sharedExpertRankNum = tilingData->moeDistributeCombineV2Info.sharedExpertRankNum;

    metaInfo_gva_gm = (GM_ADDR)tilingData->moeDistributeCombineV2Info.shmemptr;
    statusDataSpaceGm_ = (GM_ADDR)(metaInfo_gva_gm);
    selfDataStatusGMTensor_.SetGlobalBuffer((__gm__ uint32_t *)(statusDataSpaceGm_ + aivId_ * WIN_ADDR_ALIGN));

    TBuf<> dataStateBuf;
    tpipe_->InitBuffer(dataStateBuf, UB_ALIGN);
    dataState_ = ShmemInitWinState(selfDataStatusGMTensor_, epRankIdOriginal_, moeExpertNum_, epWorldSizeOriginal_,
                                   globalBS_, dataStateBuf);

    if (hasElasticInfoFlag_) {
        InitElasticInfo(sharedExpertRankNum);
    }
    sharedExpertNum_ = tilingData->moeDistributeCombineV2Info.sharedExpertNum;
    moeSendNum_ = epWorldSize_ * moeExpertPerRankNum_;
    if (epRankId_ < sharedExpertRankNum) {
        isShareExpertRankFlag_ = true;
    }

    stateOffset_ = STATE_OFFSET;
    uint32_t hFloatSize = axisH_ * static_cast<uint32_t>(sizeof(float));
    hAlign32Size_ = Ceil(axisH_, UB_ALIGN) * UB_ALIGN;
    hFloatAlign32Size_ = Ceil(hFloatSize, UB_ALIGN) * UB_ALIGN;
    hFloatAlign256Size_ = Ceil(hFloatSize, ALIGNED_LEN) * ALIGNED_LEN;
    hExpandXTypeSize_ = axisH_ * sizeof(ExpandXType);
    hExpandXAlign32Size_ = Ceil(hExpandXTypeSize_, UB_ALIGN) * UB_ALIGN;
    hAlignWinSize_ = Ceil(hExpandXTypeSize_, WIN_ADDR_ALIGN) * WIN_ADDR_ALIGN;
    hAlignWinCnt_ = hAlignWinSize_ / sizeof(ExpandXType);
    bsKNum_ = axisBS_ * axisK_;
}

template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeCombineZeroBuffer<TemplateMC2TypeFunc>::InitInt8Quant()
{
    scaleValFloat_ = static_cast<float>(1.0f / SCALE_PARAM);
    uint32_t scaleGranu = static_cast<uint32_t>(UB_ALIGN / sizeof(float));  // 计算每个block得到的reducemax结果数量
    scaleNum_ = (hExpandXAlign32Size_ / sizeof(ExpandXType)) / scaleGranu;  // 得到有效scale的个数
    repeatNum_ = static_cast<uint32_t>(hFloatAlign256Size_ /
                                       ALIGNED_LEN);  // BlockReduceMax 与 Brcb的重复迭代次数，每次256b参与计算
    mask_ = static_cast<uint32_t>(ALIGNED_LEN / sizeof(float));
    tokenScaleCnt_ = hAlign32Size_ / sizeof(ExpandXType) + scaleNum_;  // int8_align + scale有效个数
}

template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeCombineZeroBuffer<TemplateMC2TypeFunc>::Init(
    GM_ADDR expandX, GM_ADDR expertIds, GM_ADDR expandIdx, GM_ADDR epSendCount, GM_ADDR tpSendCount,
    GM_ADDR expertScales, GM_ADDR xActiveMask, GM_ADDR sharedExpertX, GM_ADDR elasticInfo, GM_ADDR oriX,
    GM_ADDR constExpertAlpha1, GM_ADDR constExpertAlpha2, GM_ADDR constExpertV, GM_ADDR XOut, GM_ADDR workspaceGM,
    TPipe *pipe, const ShmemMoeDistributeCombineZeroBufferTilingData *tilingData)
{
    tpipe_ = pipe;
    aivId_ = GetBlockIdx();

    PRINTF("[----------- CHECK COMBINE ----------- ] of Rank %d\n", epRankId_);

    maskCalcWorkspaceGM_ = workspaceGM + aivId_ * MASK_CALC_NEED_WORKSPACE;

    InitInputAndOutput(expandX, expertIds, expandIdx, epSendCount, expertScales, xActiveMask, sharedExpertX,
                       elasticInfo, oriX, constExpertAlpha1, constExpertAlpha2, constExpertV, XOut);
    InitAttrs(tilingData);

    // 检查hcclwinsize是否越界
    // CheckWindowSize(totalWinSize_, epWinContext_->winSize, tpipe_, XOut);

    if constexpr (IsInt8Quant) {
        InitInt8Quant();
    }

    PipeBarrier<PIPE_ALL>();

    // 当前win区划分为前后两半区，连续两次dispatch，切换半区
    winDataSizeOffset_ =
        static_cast<uint64_t>(dataState_) * (tilingData->moeDistributeCombineV2Info.totalWinSize / 2UL);
    winStatusOffset_ = COMBINE_STATE_OFFSET + dataState_ * WIN_STATE_OFFSET;  // 前面的预留给dispatch使用
    epWindowGM_ = GetWinAddrByRankId(epRankIdOriginal_, EP_DOMAIN);
#if defined(ASCENDC_OOM) && ASCENDC_OOM == 1
    for (int tempepRankId = 0; tempepRankId < epWorldSize_; tempepRankId++) {
        OOMCheckAddrRange<XType>((__gm__ XType *)(GetWinAddrByRankId(tempepRankId, EP_DOMAIN)), totalWinSize_);
        OOMCheckAddrRange<float>((__gm__ float *)(GetWinStateAddrByRankId(tempepRankId, EP_DOMAIN)), STATE_SIZE);
    }
#endif
    if (isShareExpertRankFlag_) {
        DataCacheCleanAndInvalid<ExpandIdxType, CacheLine::SINGLE_CACHE_LINE, DcciDst::CACHELINE_OUT>(
            epSendCountGM_[epWorldSize_ - 1]);
        selfSendCnt_ = epSendCountGM_(epWorldSize_ - 1);
    } else {
        DataCacheCleanAndInvalid<ExpandIdxType, CacheLine::SINGLE_CACHE_LINE, DcciDst::CACHELINE_OUT>(
            epSendCountGM_[moeSendNum_ - 1]);
        selfSendCnt_ = epSendCountGM_(moeSendNum_ - 1);
    }
    // SplitCoreCal();
    if constexpr (IsNeedReduceScatter) {
        // auto contextGM1 = AscendC::GetHcclContext<1>();
        // tpWinContext_ = (__gm__ HcclOpResParam *)contextGM1;
        tpSendCountGM_.SetGlobalBuffer((__gm__ int32_t *)tpSendCount);
        tpWorldSize_ = tilingData->moeDistributeCombineV2Info.tpWorldSize;
        tpRankId_ = tilingData->moeDistributeCombineV2Info.tpRankId;
        tpWindowGM_ = GetWinAddrByRankId(tpRankId_, TP_DOMAIN);
#if defined(ASCENDC_OOM) && ASCENDC_OOM == 1
        for (int temptpRankId = 0; temptpRankId < tpWorldSize_; temptpRankId++) {
            OOMCheckAddrRange<XType>((__gm__ XType *)(GetWinAddrByRankId(temptpRankId, TP_DOMAIN)), totalWinSize_);
            OOMCheckAddrRange<int32_t>((__gm__ int32_t *)(GetWinStateAddrByRankId(temptpRankId, TP_DOMAIN)),
                                       STATE_SIZE);
        }
#endif
        tpStateOffsetOnWin_ = tpRankId_ * WIN_ADDR_ALIGN;
        tpRankWindow_.SetGlobalBuffer((__gm__ XType *)tpWindowGM_);
        tpRemoteSendCnt_ = tpSendCountGM_(1 - tpRankId_);
    }
    tpipe_->InitBuffer(moeQueue_, BUFFER_NUM, hExpandXAlign32Size_);
    flagRcvCount_ = axisK_ + sharedExpertNum_;
}

template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeCombineZeroBuffer<TemplateMC2TypeFunc>::BuffInit()
{
    tpipe_->Reset();
    tpipe_->InitBuffer(readStateBuf_, UB_ALIGN);  // 32
    if constexpr (IsNeedReduceScatter) {
        tpipe_->InitBuffer(gmTpSendCountInQueue_, BUFFER_NUM, hExpandXAlign32Size_);  // 28K 存储输入拷过来的token
        tpipe_->InitBuffer(xOutQueue_, BUFFER_NUM, hExpandXAlign32Size_);             // 14K 存储输出token
        tpipe_->InitBuffer(winTpSendCountInQueue_, BUFFER_NUM, hExpandXAlign32Size_);  // 14K * 2 存储对端win区token
        if constexpr (AscendC::IsSameType<XType, bfloat16_t>::value) {
            tpipe_->InitBuffer(winTpSendCountFloatBuf_, hFloatAlign32Size_);  // 28K 参与量化及customAdd中token的v核运算
            tpipe_->InitBuffer(gmTpSendCountFloatBuf_, hFloatAlign32Size_);  // 28K 参与量化及customAdd中token的v核运算
            winTpSendCountFloatTensor_ = winTpSendCountFloatBuf_.Get<float>();
            gmTpSendCountFloatTensor_ = gmTpSendCountFloatBuf_.Get<float>();
        }
    } else {
        tpipe_->InitBuffer(gmTpSendCountQueue_, BUFFER_NUM, hExpandXAlign32Size_);  // 28K 存储搬入token
        if constexpr (IsInt8Quant) {
            uint32_t tokenScaleAlign32Size = Ceil(tokenScaleCnt_ * sizeof(ExpandXType), UB_ALIGN) * UB_ALIGN;
            tpipe_->InitBuffer(xOutQueue_, BUFFER_NUM, tokenScaleAlign32Size);  // 28K 输出token搬运
            tpipe_->InitBuffer(xAbsBuf_, hFloatAlign256Size_);  // 28K blockReduceMax计算及后续Cast计算，256对齐
            uint32_t hFloatAlign256Cnt = hFloatAlign256Size_ / sizeof(float);
            tpipe_->InitBuffer(xMaxBuf_, (hFloatAlign256Cnt / REDUCE_NUM) * sizeof(float));  // 3.5K 存储ReduceMax结果
            tpipe_->InitBuffer(xScaleMulBuf_, hFloatAlign256Size_);           // 28K 参与Brcb计算，256对齐
            tpipe_->InitBuffer(winTpSendCountFloatBuf_, hFloatAlign32Size_);  // 28K 参与Div等token v核运算

            winTpSendCountFloatTensor_ = winTpSendCountFloatBuf_.Get<float>();
            absFloatTensor_ = xAbsBuf_.Get<float>();
            reduceMaxFloatTensor_ = xMaxBuf_.Get<float>();
            scaleDupLocalTensor_ = xScaleMulBuf_.Get<float>();
            fp16CastTensor_ = xAbsBuf_.Get<half>();
            Duplicate(absFloatTensor_, float(0), hFloatAlign256Cnt);  // 统一写0
        }
    }
    // tpipe_->InitBuffer(indexCountsBuf_, sendCntNum_ * EXPAND_IDX_INFO * sizeof(int32_t));
}

// template <TemplateMC2TypeClass>
// __aicore__ inline void ShmemMoeDistributeCombineZeroBuffer<TemplateMC2TypeFunc>::SplitCoreCal()
// {
//     // 对需要发送的token数平均分核，得到每个核上处理的卡的数量
//     sendCntNum_ = selfSendCnt_ / aivNum_;
//     uint32_t remainderRankNum = selfSendCnt_ % aivNum_;

//     startTokenId = sendCntNum_ * aivId_;

//     if (aivId_ < remainderRankNum) {
//         sendCntNum_++;
//         startTokenId += aivId_;
//     } else {
//         startTokenId += remainderRankNum;
//     }
//     endTokenId = startTokenId + sendCntNum_;
// }

template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeCombineZeroBuffer<TemplateMC2TypeFunc>::SplitToCore(uint32_t curSendCnt,
                                                                                 uint32_t curUseAivNum,
                                                                                 uint32_t &startTokenId,
                                                                                 uint32_t &endTokenId,
                                                                                 uint32_t &sendTokenNum)
{
    sendTokenNum = curSendCnt / curUseAivNum;                // 每个aiv需要发送的token数(也可以是其他数)
    uint32_t remainderTokenNum = curSendCnt % curUseAivNum;  // 余数
    // PRINTF("[------------ CHECK remainTokenNum %d ------------] of core %d\n", remainderTokenNum, aivId_);
    // uint32_t newAivId = aivId_;
    startTokenId = sendTokenNum * aivId_;  // 每个aiv发送时的起始rankid
    if (aivId_ < remainderTokenNum) {      // 前remainderRankNum个aiv需要多发1个卡的数据
        sendTokenNum += 1;
        startTokenId += aivId_;
        // PRINTF("[------------ CHECK startTokenId %d ------------] of core %d\n", startTokenId, aivId_);
    } else {
        startTokenId += remainderTokenNum;
    }
    endTokenId = startTokenId + sendTokenNum;
}


template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeCombineZeroBuffer<TemplateMC2TypeFunc>::Int8QuantProcess()
{
    SyncFunc<AscendC::HardEvent::MTE2_V>();
    castLocalTensor_ = sendLocalTensor_.template ReinterpretCast<int8_t>();  // 长度为int8H_Align + scaleNum
    scaleDivTensor_ = castLocalTensor_[hAlign32Size_].template ReinterpretCast<XType>();  // 偏移前面的int8

    Cast(winTpSendCountFloatTensor_, gmTpSendCountTensor_, RoundMode::CAST_NONE, axisH_);
    PipeBarrier<PIPE_V>();
    Abs(absFloatTensor_, winTpSendCountFloatTensor_, axisH_);  // absFloatTensor_ align到256并写0，支持ReduceMax与Brcb
    PipeBarrier<PIPE_V>();
    BlockReduceMax(reduceMaxFloatTensor_, absFloatTensor_, repeatNum_, mask_, 1, 1, BLOCK_NUM);  // 32->1 256->8
    PipeBarrier<PIPE_V>();
    Muls(reduceMaxFloatTensor_, reduceMaxFloatTensor_, scaleValFloat_, scaleNum_);  // 有效个数
    PipeBarrier<PIPE_V>();
    Cast(scaleDivTensor_, reduceMaxFloatTensor_, RoundMode::CAST_RINT, scaleNum_);  // 有效个数
    PipeBarrier<PIPE_V>();
    Brcb(scaleDupLocalTensor_, reduceMaxFloatTensor_, repeatNum_, {1, BLOCK_NUM});  // 一次256
    PipeBarrier<PIPE_V>();
    Div(winTpSendCountFloatTensor_, winTpSendCountFloatTensor_, scaleDupLocalTensor_, axisH_);  // 有效个数
    PipeBarrier<PIPE_V>();
    Cast(fp16CastTensor_, winTpSendCountFloatTensor_, RoundMode::CAST_RINT, axisH_);
    PipeBarrier<PIPE_V>();
    Cast(castLocalTensor_, fp16CastTensor_, RoundMode::CAST_RINT, axisH_);
    SyncFunc<AscendC::HardEvent::V_MTE3>();
}

template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeCombineZeroBuffer<TemplateMC2TypeFunc>::CustomAdd(LocalTensor<XType> &dst,
                                                                                   LocalTensor<XType> &src0,
                                                                                   LocalTensor<XType> &src1)
{
    if constexpr (AscendC::IsSameType<XType, bfloat16_t>::value) {
        Cast(winTpSendCountFloatTensor_, src0, RoundMode::CAST_NONE, axisH_);
        Cast(gmTpSendCountFloatTensor_, src1, RoundMode::CAST_NONE, axisH_);
        PipeBarrier<PIPE_V>();
        Add(winTpSendCountFloatTensor_, winTpSendCountFloatTensor_, gmTpSendCountFloatTensor_, axisH_);
        PipeBarrier<PIPE_V>();
        Cast(dst, winTpSendCountFloatTensor_, RoundMode::CAST_RINT, axisH_);
    } else {
        Add(dst, src0, src1, axisH_);
    }
}

template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeCombineZeroBuffer<TemplateMC2TypeFunc>::WaitDispatch(uint32_t tokenIndex)
{
    uint32_t copyCount = flagRcvCount_ * FLOAT_PER_UB_ALIGN;
    uint32_t targetCount = copyCount;
    if (isInputExpertMaskFlag_ || ((zeroExpertNum_ + copyExpertNum_ + constExpertNum_) > 0U)) {
        int32_t tokenTarget = static_cast<int32_t>(tokenTargetTensor_.GetValue(tokenIndex)) + sharedExpertNum_;
        targetCount = tokenTarget * FLOAT_PER_UB_ALIGN;
    }
    // 计算地址偏移，this is the local epRank GM
    GM_ADDR stateGM = GetWinStateAddrByRankId(epRankIdOriginal_, EP_DOMAIN) + tokenIndex * flagRcvCount_ * stateOffset_;
    GlobalTensor<float> stateGMTensor;
    stateGMTensor.SetGlobalBuffer((__gm__ float *)stateGM);
    float localState = 0;
    float target = (float)1.0 * targetCount;
    float minTarget = target - (float)0.5;
    float maxTarget = target + (float)0.5;
    SumParams sumParams{1, copyCount, copyCount};
    if (isScalingDownFlag_) {
        InitElasticInfoTensor();
    }
    uint32_t toRankId;
    LocalTensor<float> stateTensor = stateBuf_.Get<float>();
    while ((localState < minTarget) || (localState > maxTarget)) {
        SyncFunc<AscendC::HardEvent::S_MTE2>();
        DataCopy<float>(stateTensor, stateGMTensor, copyCount);
        SyncFunc<AscendC::HardEvent::MTE2_V>();
        Sum(stateTensor, stateTensor, sumParams);
        SyncFunc<AscendC::HardEvent::V_S>();
        localState = stateTensor(0);
    }
    SyncFunc<AscendC::HardEvent::S_MTE3>();
    DataCopy<float>(stateGMTensor, stateResetTensor_, copyCount);  // 等完清理
}

template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeCombineZeroBuffer<TemplateMC2TypeFunc>::Int8DequantProcess(LocalTensor<XType> &src)
{
    SyncFunc<AscendC::HardEvent::MTE2_V>();
    castLocalTensor_ = src.template ReinterpretCast<int8_t>();
    scaleDivTensor_ = src[hAlign32Size_ / 2];

    SyncFunc<AscendC::HardEvent::S_V>();
    Cast(scaleDivFloatTensor_, scaleDivTensor_, RoundMode::CAST_NONE, scaleNum_);
    Cast(fp16CastTensor_, castLocalTensor_, RoundMode::CAST_NONE, axisH_);
    PipeBarrier<PIPE_V>();
    Cast(absFloatTensor_, fp16CastTensor_, RoundMode::CAST_NONE, axisH_);
    Brcb(scaleDupLocalTensor_, scaleDivFloatTensor_, repeatNum_, {1, BLOCK_NUM});
    PipeBarrier<PIPE_V>();
    Mul(absFloatTensor_, absFloatTensor_, scaleDupLocalTensor_, axisH_);
    PipeBarrier<PIPE_V>();
    Cast(src, absFloatTensor_, RoundMode::CAST_RINT, axisH_);
    PipeBarrier<PIPE_V>();
}

// 处理常量专家
template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeCombineZeroBuffer<TemplateMC2TypeFunc>::ProcessConstantExpert(
    uint32_t tokenIndex, uint32_t const_expert_idx, float scaleVal)
{
    PipeBarrier<PIPE_ALL>();
    LocalTensor<float> constVFloatLocal = mulBuf_.Get<float>();
    LocalTensor<ExpandXType> const_v_ub = moeSumQueue_.AllocTensor<ExpandXType>();
    LocalTensor<ExpandXType> rowTmpLocal = tokenBuf_.Get<ExpandXType>();
    DataCopyPadExtParams<ExpandXType> copyPadExtParams{false, 0U, 0U, 0U};
    DataCopyExtParams expandXCopyParams{1U, static_cast<uint32_t>(hExpandXTypeSize_), 0U, 0U, 0U};
    // 直接从GM读取当前常量专家的alpha1和alpha2参数
    ExpandXType alpha1 = constExpertAlpha1GM_.GetValue(const_expert_idx);
    ExpandXType alpha2 = constExpertAlpha2GM_.GetValue(const_expert_idx);
    float alpha1Float;
    float alpha2Float;
    if constexpr (std::is_same_v<ExpandXType, bfloat16_t>) {
        alpha1Float = ToFloat(alpha1);
        alpha2Float = ToFloat(alpha2);
    } else {
        alpha1Float = static_cast<float>(alpha1);
        alpha2Float = static_cast<float>(alpha2);
    }

    // 使用moeSumQueue_分配缓冲区来存储常量专家向量v
    DataCopyPad(const_v_ub, constExpertVGM_[const_expert_idx * axisH_], expandXCopyParams, copyPadExtParams);
    moeSumQueue_.EnQue(const_v_ub);
    const_v_ub = moeSumQueue_.DeQue<ExpandXType>();

    // 读取输入token
    DataCopyPad(rowTmpLocal, oriXGM_[tokenIndex * axisH_], expandXCopyParams, copyPadExtParams);
    SyncFunc<AscendC::HardEvent::MTE2_V>();
    // 计算 alpha1 * x + alpha2 * v
    Cast(rowTmpFloatLocal_, rowTmpLocal, AscendC::RoundMode::CAST_NONE, axisH_);
    PipeBarrier<PIPE_V>();

    Cast(constVFloatLocal, const_v_ub, AscendC::RoundMode::CAST_NONE, axisH_);
    PipeBarrier<PIPE_V>();
    moeSumQueue_.FreeTensor<ExpandXType>(const_v_ub);

    AscendC::Muls(rowTmpFloatLocal_, rowTmpFloatLocal_, alpha1Float, axisH_);
    AscendC::Muls(constVFloatLocal, constVFloatLocal, alpha2Float, axisH_);
    PipeBarrier<PIPE_V>();
    AscendC::Add(rowTmpFloatLocal_, rowTmpFloatLocal_, constVFloatLocal, axisH_);
    PipeBarrier<PIPE_V>();

    // 乘以专家权重
    AscendC::Muls(mulBufLocal_, rowTmpFloatLocal_, scaleVal, axisH_);
    PipeBarrier<PIPE_V>();
    AscendC::Add(sumFloatBufLocal_, sumFloatBufLocal_, mulBufLocal_, axisH_);
    PipeBarrier<PIPE_V>();
}

// 处理拷贝专家
template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeCombineZeroBuffer<TemplateMC2TypeFunc>::ProcessCopyExpert(uint32_t tokenIndex,
                                                                                           float scaleVal)
{
    DataCopyPadExtParams<ExpandXType> copyPadExtParams{false, 0U, 0U, 0U};
    DataCopyExtParams expandXCopyParams{1U, static_cast<uint32_t>(hExpandXTypeSize_), 0U, 0U, 0U};
    LocalTensor<ExpandXType> tmpUb = moeSumQueue_.AllocTensor<ExpandXType>();
    DataCopyPad(tmpUb, oriXGM_[tokenIndex * axisH_], expandXCopyParams, copyPadExtParams);
    moeSumQueue_.EnQue(tmpUb);
    tmpUb = moeSumQueue_.DeQue<ExpandXType>();

    Cast(rowTmpFloatLocal_, tmpUb, AscendC::RoundMode::CAST_NONE, axisH_);
    PipeBarrier<PIPE_V>();
    moeSumQueue_.FreeTensor<ExpandXType>(tmpUb);
    Muls(mulBufLocal_, rowTmpFloatLocal_, scaleVal, axisH_);
    PipeBarrier<PIPE_V>();
    Add(sumFloatBufLocal_, sumFloatBufLocal_, mulBufLocal_, axisH_);
    PipeBarrier<PIPE_V>();
}

// 处理Moe专家
template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeCombineZeroBuffer<TemplateMC2TypeFunc>::ProcessMoeExpert(uint32_t tokenIndexOffset,
                                                                                          uint32_t topkId,
                                                                                          float scaleVal)
{
    uint32_t processLen = axisH_;
    const DataCopyExtParams xScaleCopyParams{1U, static_cast<uint32_t>(tokenScaleCnt_ * sizeof(ExpandXType)), 0U, 0U,
                                             0U};
    const DataCopyExtParams expandXCopyParams{1U, static_cast<uint32_t>(hExpandXTypeSize_), 0U, 0U, 0U};
    const DataCopyPadExtParams<ExpandXType> copyPadExtParams{false, 0U, 0U, 0U};

    GM_ADDR wAddr = (__gm__ uint8_t *)(epWindowGM_) + (tokenIndexOffset + topkId) * hAlignWinSize_;
    rowTmpGlobal_.SetGlobalBuffer((__gm__ ExpandXType *)wAddr);
    // 这里应该会拿到自己当时发出去的 token
    // DumpTensor(rowTmpGlobal_, 1181, 32);

    LocalTensor<ExpandXType> tmpUb = moeSumQueue_.AllocTensor<ExpandXType>();
    if constexpr (IsInt8Quant) {
        DataCopyPad(tmpUb, rowTmpGlobal_, xScaleCopyParams, copyPadExtParams);
    } else {
        DataCopyPad(tmpUb, rowTmpGlobal_, expandXCopyParams, copyPadExtParams);
    }
    moeSumQueue_.EnQue(tmpUb);
    tmpUb = moeSumQueue_.DeQue<ExpandXType>();
    if constexpr (IsInt8Quant) {
        Int8DequantProcess(tmpUb);
    }
    Cast(rowTmpFloatLocal_, tmpUb, AscendC::RoundMode::CAST_NONE, processLen);
    PipeBarrier<PIPE_V>();
    AscendC::Muls(mulBufLocal_, rowTmpFloatLocal_, scaleVal, processLen);
    PipeBarrier<PIPE_V>();
    AscendC::Add(sumFloatBufLocal_, sumFloatBufLocal_, mulBufLocal_, processLen);
    moeSumQueue_.FreeTensor<ExpandXType>(tmpUb);
}


template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeCombineZeroBuffer<TemplateMC2TypeFunc>::InputToDstOutput()
{
    if (axisBS_ == 0U) {
        return;
    }
    // uint32_t tokenPerBlock = 0U, startTokenIndex = 0U, endTokenIndex = 0U;
    // SplitCoreCal();
    uint32_t startTokenId, endTokenId, sendTokenNum;  // 每个aiv发送时的起始rankid
    SplitToCore(axisBS_, aivNum_, startTokenId, endTokenId, sendTokenNum);
    // PRINTF("[--------------- Check token Range [%d, %d] ------------] of Rank %d on Core %d\n", startTokenId, endTokenId, epRankId_, aivId_);
    if (sendTokenNum == 0U) {
        return;
    }

    tpipe_->Reset();
    uint32_t sendCountAlignLen_ = Ceil(epWorldSize_ * moeExpertNum_ * sizeof(int32_t), UB_ALIGN) * UB_ALIGN;
    tpipe_->InitBuffer(sendCountBuf_, sendCountAlignLen_);
    globalSendCountLocal_ = sendCountBuf_.Get<int32_t>();

    // uint32_t scaleAlignLen_ = Ceil(axisBS_ * axisK_ * sizeof(int32_t), UB_ALIGN) * UB_ALIGN;
    uint32_t scaleAlignLen_ = Ceil(sendTokenNum * axisK_ * sizeof(int32_t), UB_ALIGN) * UB_ALIGN;
    tpipe_->InitBuffer(expertScalesBuf_, scaleAlignLen_);
    tpipe_->InitBuffer(indexCountsBuf_, scaleAlignLen_);

    // get scale per token
    DataCopyExtParams bskParams{1U, static_cast<uint32_t>(sendTokenNum * axisK_ * sizeof(uint32_t)), 0U, 0U, 0U};
    const DataCopyPadExtParams<float> copyPadFloatParams{false, 0U, 0U, 0U};
    expertScalesLocal_ = expertScalesBuf_.Get<float>();
    DataCopyPad(expertScalesLocal_, expertScalesGT_[startTokenId * axisK_], bskParams, copyPadFloatParams);
    // PRINTF("[--------------- Check expertScalesLocal_ ------------] of Rank %d on Core %d\n", epRankId_, aivId_);
    // DumpTensor(expertScalesLocal_, 849, sendTokenNum * axisK_);

    // get per token remote read small offset
    LocalTensor<int32_t> expandIdxLocal_ = indexCountsBuf_.Get<int32_t>();
    // DataCopyExtParams sCntParams{1U, static_cast<uint32_t>(sendTokenNum * sizeof(uint32_t)), 0U, 0U, 0U};
    DataCopyPadExtParams<int32_t> copyPadint32Params{false, 0U, 0U, 0U};
    DataCopyPad(expandIdxLocal_, expandIdxGM_[startTokenId * axisK_], bskParams, copyPadint32Params);
    PRINTF("[--------------- Check expandIdxLocal_ ------------] of Rank %d on Core %d\n", epRankId_, aivId_);
    DumpTensor(expandIdxLocal_, 857, sendTokenNum * axisK_);

    // get per token remote read big offset
    DataCopyExtParams countParams{1U, static_cast<uint32_t>(epWorldSize_ * moeExpertNum_ * sizeof(int32_t)), 0U,
                                0U, 0U};
    DataCopyPad(globalSendCountLocal_, epSendCountGM_, countParams, copyPadint32Params);
    // PRINTF("[--------------- Check globalSendCountLocal_ ------------] of Rank %d on Core %d", epRankId_, aivId_);
    // DumpTensor(globalSendCountLocal_, 866, epWorldSize_ * moeExpertNum_);

    // PipeBarrier<PIPE_V>();
    // SyncFunc<AscendC::HardEvent::MTE2_S>();

    uint32_t expertIdsAlignLen_ = Ceil(axisK_ * sizeof(int32_t), UB_ALIGN) * UB_ALIGN;
    tpipe_->InitBuffer(expertIdsBuf_, expertIdsAlignLen_);
    DataCopyExtParams kParams{1U, static_cast<uint32_t>(axisK_ * sizeof(ExpandIdxType)), 0U, 0U, 0U};
    DataCopyPadExtParams<ExpandIdxType> copyPadParams{false, 0U, 0U, 0U};
    LocalTensor<int32_t> expertIdsTensor_ = expertIdsBuf_.Get<int32_t>();

    DataCopyExtParams xOutCopyParams{1U, static_cast<uint32_t>(hExpandXTypeSize_), 0U, 0U, 0U};
    DataCopyPadExtParams<ExpandXType> copyPadExtParams{false, 0U, 0U, 0U};

    tpipe_->InitBuffer(xOutBuf_, hExpandXAlign32Size_);
    tpipe_->InitBuffer(moeSumQueue_, BUFFER_NUM, hExpandXAlign32Size_); 
    tpipe_->InitBuffer(tokenSumBuf_, hFloatAlign32Size_);  
    tpipe_->InitBuffer(weightedSumBuf_, hFloatAlign256Size_);
    tpipe_->InitBuffer(sumFloatBuf_, hFloatAlign32Size_);
    tokenReuceTensor_ = tokenSumBuf_.Get<float>();
    weightedSumTensor_ = weightedSumBuf_.Get<float>();
    sumTokenTensor_ = sumFloatBuf_.Get<float>();

    for (uint32_t tokenIndex = startTokenId; tokenIndex < endTokenId; tokenIndex++) {
        uint32_t localTokenId = tokenIndex - startTokenId;
        DataCopyPad(expertIdsTensor_, expertIdsGM_[tokenIndex * axisK_], kParams, copyPadParams);
        // PRINTF("[--------------- Check expertIdsTensor_ ------------] of Rank %d on Core %d\n", epRankId_, aivId_);
        // DumpTensor(expertIdsTensor_, 878, axisK_);
        // SyncFunc<AscendC::HardEvent::MTE2_V>();
        PipeBarrier<PIPE_ALL>();

        Duplicate(sumTokenTensor_, static_cast<float>(0), axisH_);

        for (uint32_t topkId = 0U; topkId < axisK_; topkId++) {
            int32_t dstExpertId = expertIdsTensor_.GetValue(topkId);
            int32_t srcRankId = dstExpertId / moeExpertPerRankNum_;
            uint32_t offsetIdx = dstExpertId * epWorldSize_ + epRankId_;
            uint32_t col = offsetIdx % moeExpertNum_;
            int32_t remoteBase = (col == 0) ? 0 : globalSendCountLocal_(offsetIdx - 1);  // count 转 offset
            // int32_t remoteBase = globalSendCountLocal_(dstExpertId * epWorldSize_ + epRankId_);
            int32_t remoteOffset = expandIdxLocal_(localTokenId * axisK_ + topkId);
            float scale = expertScalesLocal_.GetValue(topkId);

            // PRINTF("[--------------- Check expertScalesLocal_ ------------] of Rank %d on Core %d\n", epRankId_, aivId_);
            // DumpTensor(expertScalesLocal_, 911, sendTokenNum * axisK_);
            // PRINTF("[--------------- Check scale %f ------------] of Rank %d on Core %d\n", scale, epRankId_, aivId_);

            auto srcPtr = reinterpret_cast<__gm__ uint8_t *>(shmem_ptr(expandXGM_, srcRankId));
            // auto srcPtr = reinterpret_cast<__gm__ uint8_t *>(shmem_ptr(expandXGM_, epRankId_));
            srcWinGMTensor.SetGlobalBuffer((__gm__ ExpandXType *)(srcPtr + hExpandXTypeSize_ * (remoteBase + remoteOffset)));

            // PRINTF("[--------------- Check remote Addr %p of srcRank %d ------------] of Rank %d on Core %d\n", 
            //             srcPtr, srcRankId, epRankId_, aivId_);
            PRINTF("[--------------- Check offsetIdx %d col %d remoteBase %d and remoteOffset %d------------] of Rank %d on Core %d\n", 
                        offsetIdx, col, remoteBase, remoteOffset, epRankId_, aivId_);
            PRINTF("[--------------- Check Count@offsetIdx %d and Offset@offsetIdx %d of srcRank %d ------------] of Rank %d on Core %d\n", 
                        globalSendCountLocal_(offsetIdx), remoteBase, srcRankId, epRankId_, aivId_);
            PRINTF("[--------------- Check on Range %d take SmallOffset %d - srcRank %d ------------] of Rank %d on Core %d\n", 
                        globalSendCountLocal_(offsetIdx) - remoteBase, remoteOffset, srcRankId, epRankId_, aivId_);

            LocalTensor<ExpandXType> tmpToken = moeSumQueue_.AllocTensor<ExpandXType>();
            // Read From Remote
            DataCopyPad(tmpToken, srcWinGMTensor, xOutCopyParams, copyPadExtParams);
            moeSumQueue_.EnQue(tmpToken);
            tmpToken = moeSumQueue_.DeQue<ExpandXType>();
            DumpTensor(tmpToken, 931, 8);
            Cast(tokenReuceTensor_, tmpToken, AscendC::RoundMode::CAST_NONE, axisH_);
            PipeBarrier<PIPE_V>();
            AscendC::Muls(weightedSumTensor_, tokenReuceTensor_, scale, axisH_);
            PipeBarrier<PIPE_V>();
            AscendC::Add(sumTokenTensor_, sumTokenTensor_, weightedSumTensor_, axisH_);
            moeSumQueue_.FreeTensor<ExpandXType>(tmpToken);
            PipeBarrier<PIPE_V>();
        }
        PipeBarrier<PIPE_V>();
        LocalTensor<ExpandXType> xOutLocal = xOutBuf_.Get<ExpandXType>();
        Cast(xOutLocal, sumTokenTensor_, AscendC::RoundMode::CAST_RINT, axisH_);
        SyncFunc<AscendC::HardEvent::V_MTE3>();
        // DumpTensor(xOutLocal, 944, 8);
        DataCopyPad(XOutGT_[tokenIndex * axisH_], xOutLocal, xOutCopyParams);
    }
}

template <TemplateMC2TypeClass>
__aicore__ inline void ShmemMoeDistributeCombineZeroBuffer<TemplateMC2TypeFunc>::Process()
{
    if ASCEND_IS_AIV {  // 全aiv处理
        // BuffInit();
        // SyncAll<true>();
        // shmem_barrier_all(); 
        InputToDstOutput();
        // SyncAll<true>();
        // shmem_barrier_all();  // 全卡同步，确保数据已经获取完
        // SetWaitTpStatusAndDisPatch();  // SetDispatchFlag in ExpertAlltoAllDispatchCopyAdd
        // AlltoAllBuffInitAndMaskCal();
        // LocalWindowCopy();  // WaitDispatchFlag per token
    }
}

}  // namespace MoeDistributeCombineZeroBufferImpl
#endif  // MOE_DISTRIBUTE_COMBINE_ZeroBuffer_H
