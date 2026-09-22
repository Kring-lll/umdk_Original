/**
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * SPDX-License-Identifier: MIT
 * Description: weight_quant_basic_block header file
 * Create: 2026-09-22
 * Note:
 * History: 2026-09-22 port from cam_async repository
 */
/*!
 * \file weight_quant_basic_block.h
 * \brief
 */
#ifndef GROUPED_MATMUL_WEIGHT_QUANT_BASIC_BLOCK_H
#define GROUPED_MATMUL_WEIGHT_QUANT_BASIC_BLOCK_H

#include "basic_block_config.h"
#if ASC_DEVKIT_MAJOR >= 9
#include "kernel_basic_intf.h"
#else
#include "kernel_operator.h"
#include "kernel_operator_intf.h"
#endif
#include "lib/matmul_intf.h"
#include "tool.h"
#include "weight_quant_basic_block_base.h"
#include "weight_quant_cube_compute.h"
#include "weight_quant_vec_compute.h"
#include "basic_api/weight_quant_basic_api_v1.h"

using AscendC::Conditional;
using AscendC::GetSubBlockIdx;
using AscendC::IsSameType;
using AscendC::LocalTensor;
using AscendC::TBuf;
using AscendC::TPipe;
using AscendC::TPosition;

namespace WeightQuantBatchMatmulV2::Arch35 {

#define GMM_WQ_BASIC_BLOCK_TEMPLATE_PARAM                                                                              \
    template <typename xType, typename wType, typename antiQuantScaleType, typename scaleType,                         \
        typename perTokenScaleType, typename biasType, typename yType, const WqmmConfig &wqmmConfig,                   \
        const VecAntiQuantConfig &vecConfig>

#define GMM_WQ_BASIC_BLOCK_CLASS                                                                                       \
    WeightQuantMatmulBasicBlock<xType, wType, antiQuantScaleType, scaleType, perTokenScaleType, biasType, yType,       \
        wqmmConfig, vecConfig>

GMM_WQ_BASIC_BLOCK_TEMPLATE_PARAM
class WeightQuantMatmulBasicBlock : public WeightQuantMatmulBasicBlockBaseClass {
  public:
    __aicore__ inline WeightQuantMatmulBasicBlock(){};
    __aicore__ inline void Init(bool hasBias, uint64_t antiQuantGroupSize, uint64_t aPrefetchSize,
        const TCubeTiling *__restrict matmulTiling, TPipe *tPipe);
    __aicore__ inline void UpdateGlobalAddr(__gm__ xType *x, __gm__ wType *weight,
        __gm__ antiQuantScaleType *antiquantScale, __gm__ xType *antiquantOffset, __gm__ scaleType *scale,
        __gm__ perTokenScaleType *perTokenScale, __gm__ biasType *bias, __gm__ yType *y, const bool hasBias,
        const bool weightL2Cacheable);
    __aicore__ inline void ComputeBasicBlock(
        const BasicBlockOffsetParam &offsetParam, const BasicBlockOffsetParam &lastOffsetParam);
    __aicore__ inline void PrefetchA(uint64_t aPrefetchSize, uint64_t xSizeLimit);
    __aicore__ inline void End(const BasicBlockOffsetParam &offsetParam);

  protected:
    __aicore__ inline void SetAivToAic();
    __aicore__ inline void WaitAivToAic();
    __aicore__ inline void SetAicToAiv();
    __aicore__ inline void WaitAicToAiv();
    __aicore__ inline void ComputeBasicBlockAivNdNkNzKn(const BasicBlockOffsetParam &offsetParam);
    __aicore__ inline void ComputeBasicBlockAivNdKnNzNk(const BasicBlockOffsetParam &offsetParam);
    __aicore__ inline void mxBiasSetParamAndGmtoUb(const BasicBlockOffsetParam &offsetParam,
        L1ConsumeConfig &l1ConsumeConfig, UbConsumeConfig &ubConsumeConfig, const uint64_t kMte2Offset,
        const uint64_t mte2RealK);
    __aicore__ inline void ComputeBasicBlockAic(const BasicBlockOffsetParam &offsetParam);

    BasicBlockLibVectorAntiQuantCompute<xType, wType, antiQuantScaleType, biasType, yType, wqmmConfig, vecConfig>
        vectorCompute_;

    using aType = typename Conditional<IsMxA8W4<xType, wqmmConfig.antiQuantType>(),
        MatmulTypeWithScale<TPosition::TSCM, TPosition::TSCM, CubeFormat::NZ, xType, wqmmConfig.aTrans>,
        MatmulL1GmType<TPosition::TSCM, CubeFormat::NZ, xType, wqmmConfig.aTrans>>::type;
    using bType = typename Conditional<IsMxA8W4<xType, wqmmConfig.antiQuantType>(),
        MatmulTypeWithScale<TPosition::TSCM, TPosition::TSCM, CubeFormat::NZ, xType, wqmmConfig.bTrans>,
        MatmulL1GmType<TPosition::TSCM, CubeFormat::NZ, xType, wqmmConfig.bTrans>>::type;
    using cType = MatmulType<TPosition::GM, CubeFormat::ND, yType>;
    using biasMatmulType = MatmulType<TPosition::TSCM, CubeFormat::ND, biasType>;

    using MMImpl = typename Conditional<IsMxA8W4<xType, wqmmConfig.antiQuantType>(),
        WqbmmBasicApiV1<xType, biasType, yType, wqmmConfig.aTrans, wqmmConfig.bTrans>,
        MatmulImpl<aType, bType, cType, biasMatmulType, CFG_MDL>>::type;
    WeightQuantBatchMatmulV2CubeCompute<xType, biasType, antiQuantScaleType, perTokenScaleType, yType, wqmmConfig,
        MMImpl>
        cubeCompute_;

    uint64_t cvLoopIdx_ = 0;

    LocalTensor<xType> weightL1_;
    LocalTensor<biasType> biasL1_;
    uint64_t weightL1DbOffset_;
    uint64_t biasL1DbOffset_;
    bool hasBias_;
};

GMM_WQ_BASIC_BLOCK_TEMPLATE_PARAM
__aicore__ inline void GMM_WQ_BASIC_BLOCK_CLASS::Init(bool hasBias, uint64_t antiQuantGroupSize, uint64_t aPrefetchSize,
    const TCubeTiling *__restrict matmulTiling, TPipe *tPipe)
{
    hasBias_ = hasBias;
    biasL1DbOffset_ = 0;
    weightL1_ = LocalTensor<xType>(TPosition::TSCM, 0, L1_SIZE_BYTE / sizeof(xType));

    if constexpr (IsMxA8W4<xType, wqmmConfig.antiQuantType>()) {
        static constexpr uint64_t MXA8W4_WEIGHT_SIZE =
            256 * 256;  // the standard shape of a single weight block is 256*256
        static constexpr uint64_t MX_BIAS_L1_SIZE = BIAS_L1_SIZE * GetKBUnit<biasType>() * sizeof(biasType);
        weightL1DbOffset_ = L1_SIZE * GetKBUnit<xType>() - MXA8W4_WEIGHT_SIZE;
        uint64_t l1RemainSize = L1_SIZE_BYTE - MXA8W4_WEIGHT_SIZE * DOUBLE_BUFFER_NUM;
        uint64_t l1StartSize = MXA8W4_WEIGHT_SIZE;
        biasL1_ = LocalTensor<biasType>(TPosition::TSCM, l1StartSize, l1RemainSize / sizeof(biasType));
        if (hasBias_) {
            biasL1DbOffset_ = (l1RemainSize - MX_BIAS_L1_SIZE) / sizeof(biasType);
            l1RemainSize -= DOUBLE_BUFFER_NUM * MX_BIAS_L1_SIZE;
            l1StartSize += MX_BIAS_L1_SIZE;
        }

        if ASCEND_IS_AIC {
            cubeCompute_.MxA8W4Init(aPrefetchSize, l1RemainSize, l1StartSize, biasL1DbOffset_, matmulTiling, biasL1_);
        } else {
            vectorCompute_.Init(tPipe, hasBias_);
        }
    } else {
        uint64_t weightL1Space =
            matmulTiling->baseN * matmulTiling->stepKb * matmulTiling->baseK;  // size of a single weight block
        weightL1DbOffset_ = L1_SIZE * GetKBUnit<xType>() - weightL1Space;

        if ASCEND_IS_AIC {
            cubeCompute_.Init(L1_SIZE_BYTE, weightL1Space, aPrefetchSize, matmulTiling, tPipe, biasL1DbOffset_);
        } else {
            vectorCompute_.Init(tPipe, hasBias_);
        }
    }
}

GMM_WQ_BASIC_BLOCK_TEMPLATE_PARAM
__aicore__ inline void GMM_WQ_BASIC_BLOCK_CLASS::UpdateGlobalAddr(__gm__ xType *x, __gm__ wType *weight,
    __gm__ antiQuantScaleType *antiquantScale, __gm__ xType *antiquantOffset, __gm__ scaleType *scale,
    __gm__ perTokenScaleType *perTokenScale, __gm__ biasType *bias, __gm__ yType *y, const bool hasBias,
    const bool weightL2Cacheable)
{
    if ASCEND_IS_AIC {
        cubeCompute_.UpdateGlobalAddr(x, y, bias, antiquantScale, scale, perTokenScale, hasBias);
    } else {
        vectorCompute_.UpdateGlobalAddr(
            weight, antiquantScale, antiquantOffset, nullptr, nullptr, bias, weightL2Cacheable);
    }
}

/*
 * This function determines the actual transfer volume of the vec cores according to the transpose attribute, the shape
 * size on L1 and vecconfig ND TransB = True: the mte2 transfer volume of the two vec cores is (curVecCoreMte2RealN,
 * curVecCoreMte2RealK) NZ TransB = False: the mte2 transfer volume of the two vec cores is
 * (CeilDiv(curVecCoreMte2RealN,16), CeilDiv(curVecCoreMte2RealK,16), 16 ,16)
 */
GMM_WQ_BASIC_BLOCK_TEMPLATE_PARAM
__aicore__ inline void GMM_WQ_BASIC_BLOCK_CLASS::ComputeBasicBlockAivNdNkNzKn(const BasicBlockOffsetParam &offsetParam)
{
    UbConsumeConfig ubConsumeConfig;
    L1ConsumeConfig l1ConsumeConfig;
    ubConsumeConfig.nWeightLowBitUbOffset = 0;
    l1ConsumeConfig.l1RealExternalLen = offsetParam.nL1Size;

    // initial value is half of nl1
    uint64_t curVecCoreMte2RealN = offsetParam.nL1Size >> 1;
    if constexpr (wqmmConfig.weightFormat == CubeFormat::NZ) {
        curVecCoreMte2RealN = offsetParam.nL1Size > BLOCK_CUBE
                                  ? CeilAlign(curVecCoreMte2RealN, static_cast<uint64_t>(BLOCK_CUBE))
                                  : offsetParam.nL1Size;
    }

    // the actual value must be determined per vec core
    ubConsumeConfig.l1RequireVfComputeRealN =
        GetSubBlockIdx() == 0 ? curVecCoreMte2RealN : offsetParam.nL1Size - curVecCoreMte2RealN;
    l1ConsumeConfig.l1SplitTwoVecExternalOffset = GetSubBlockIdx() * curVecCoreMte2RealN;

    for (uint64_t kMte2Offset = 0; kMte2Offset < offsetParam.kSize; kMte2Offset += vecConfig.ubMte2InnerSize) {
        uint64_t mte2RealK = (kMte2Offset + vecConfig.ubMte2InnerSize) > offsetParam.kSize
                                 ? offsetParam.kSize - kMte2Offset
                                 : vecConfig.ubMte2InnerSize;  // the actual amount the vec needs to transfer along K in
                                                               // total (accounting for the tail block)
        vectorCompute_.WaitVToMTE2();
        vectorCompute_.CopyGmToUb(ubConsumeConfig.l1RequireVfComputeRealN, mte2RealK,
            offsetParam.nOffset + l1ConsumeConfig.l1SplitTwoVecExternalOffset, kMte2Offset, offsetParam);

        // with the current scheme the computed amount along N is never smaller than the loaded amount, so there is no N
        // loop
        for (ubConsumeConfig.kWeightLowBitUbOffset = 0; ubConsumeConfig.kWeightLowBitUbOffset < mte2RealK;
            ubConsumeConfig.kWeightLowBitUbOffset += offsetParam.kbL1Size, cvLoopIdx_++) {
            ubConsumeConfig.l1RequireVfComputeRealK =
                (ubConsumeConfig.kWeightLowBitUbOffset + offsetParam.kbL1Size) >= mte2RealK
                    ? mte2RealK - ubConsumeConfig.kWeightLowBitUbOffset
                    : offsetParam.kbL1Size;
            if (cvLoopIdx_ > 1) {
                WaitAicToAiv();
            }
            vectorCompute_.WeightAntiQuantCompute(
                ubConsumeConfig, weightL1_[(cvLoopIdx_ & 1) * weightL1DbOffset_], l1ConsumeConfig, nullptr);
            SetAivToAic();
        }
        vectorCompute_.SetVToMTE2();
    }
}

/*
 * This function updates the various parameter settings of the Mx Bias and completes the transfer of bias from GM to UB
 * isBiasSingleVector = True means only one Vector core is needed for the computation
 * calcMxBias = True means the Bias needs to be computed and transferred
 */
GMM_WQ_BASIC_BLOCK_TEMPLATE_PARAM
__aicore__ inline void GMM_WQ_BASIC_BLOCK_CLASS::mxBiasSetParamAndGmtoUb(const BasicBlockOffsetParam &offsetParam,
    L1ConsumeConfig &l1ConsumeConfig, UbConsumeConfig &ubConsumeConfig, const uint64_t kMte2Offset,
    const uint64_t mte2RealK)
{
    uint64_t ubMte2MxBiasNSize = 0;
    uint64_t ubMte2MxBiasNOffset = 0;
    if (hasBias_ && kMte2Offset == 0) {
        ubConsumeConfig.isBiasSingleVector = (mte2RealK == l1ConsumeConfig.l1RealExternalLen);
        if (ubConsumeConfig.isBiasSingleVector) {
            ubMte2MxBiasNSize = offsetParam.nL1Size;
        } else {
            uint64_t mxBiasVec0Nsize =
                offsetParam.nL1Size < MX_BIAS_SINGLE_VECTOR_SIZE ? offsetParam.nL1Size : MX_BIAS_SINGLE_VECTOR_SIZE;
            ubMte2MxBiasNSize = (GetSubBlockIdx() == 0) ? mxBiasVec0Nsize : (offsetParam.nL1Size - mxBiasVec0Nsize);
        }
        ubMte2MxBiasNOffset =
            offsetParam.nOffset + GetSubBlockIdx() * ((ubMte2MxBiasNSize != 0) ? MX_BIAS_SINGLE_VECTOR_SIZE : 0);
    }
    ubConsumeConfig.calcMxBias = (hasBias_ && (kMte2Offset == 0) && (ubMte2MxBiasNSize != 0));
    if (ubConsumeConfig.calcMxBias) {
        ubConsumeConfig.ubMxBiasNsize = ubMte2MxBiasNSize;
        l1ConsumeConfig.l1MxBiasSplitNOffset = GetSubBlockIdx() * MX_BIAS_SINGLE_VECTOR_SIZE;
        vectorCompute_.CopyMxBiasGmToUb(ubMte2MxBiasNSize, ubMte2MxBiasNOffset);
    }
}

/*
 * This function determines the actual transfer volume of the vec cores according to the transpose attribute, the shape
 * size on L1 and vecconfig ND TransB = False: the mte2 transfer volume of the two vec cores is (curVecCoreMte2RealK,
 * curVecCoreMte2RealN) NZ TransB = True: the mte2 transfer volume of the two vec cores is (CeilDiv(curVecCoreMte2RealK,
 * C0), CeilDiv(curVecCoreMte2RealN, 16), 16, C0)
 */
GMM_WQ_BASIC_BLOCK_TEMPLATE_PARAM
__aicore__ inline void GMM_WQ_BASIC_BLOCK_CLASS::ComputeBasicBlockAivNdKnNzNk(const BasicBlockOffsetParam &offsetParam)
{
    // c:v is 1:2, the data required by cube is provided by the two v cores
    uint64_t kMte2BaseSize = offsetParam.kbL1Size >> 1;

    UbConsumeConfig ubConsumeConfig;
    L1ConsumeConfig l1ConsumeConfig;
    ubConsumeConfig.l1RequireVfComputeRealN = offsetParam.nL1Size;
    ubConsumeConfig.kWeightLowBitUbOffset = 0;
    ubConsumeConfig.nWeightLowBitUbOffset = 0;
    l1ConsumeConfig.l1SplitTwoVecExternalOffset = GetSubBlockIdx() * kMte2BaseSize;

    for (uint64_t kMte2Offset = 0; kMte2Offset < offsetParam.kSize; kMte2Offset += offsetParam.kbL1Size, cvLoopIdx_++) {
        l1ConsumeConfig.l1RealExternalLen = (kMte2Offset + offsetParam.kbL1Size) > offsetParam.kSize
                                                ? offsetParam.kSize - kMte2Offset
                                                : offsetParam.kbL1Size;

        /*
         * Scenario 1: the current core is v0; the k actually transferred by mte2 is the minimum of the standard mte2
         * transfer value (kMte2BaseSize) and the real k value on l1 Scenario 2: the current core is v1 and the real k
         * value on l1 is greater than the value transferred by core v1, so the k actually transferred by mte2 is their
         * difference Scenario 3: the current core is v1 and the real k value on l1 is smaller than the value
         * transferred by core v1, so mte2 needs no transfer and the value can just be set to 0
         */
        uint64_t mte2RealK = GetSubBlockIdx() == 0 ? min(kMte2BaseSize, l1ConsumeConfig.l1RealExternalLen)
                             : l1ConsumeConfig.l1RealExternalLen > kMte2BaseSize
                                 ? l1ConsumeConfig.l1RealExternalLen - kMte2BaseSize
                                 : 0;

        vectorCompute_.WaitVToMTE2();
        if constexpr (IsMxA8W4<xType, wqmmConfig.antiQuantType>()) {
            mxBiasSetParamAndGmtoUb(offsetParam, l1ConsumeConfig, ubConsumeConfig, kMte2Offset, mte2RealK);
        }
        vectorCompute_.CopyGmToUb(offsetParam.nL1Size, mte2RealK, offsetParam.nOffset,
            kMte2Offset + GetSubBlockIdx() * kMte2BaseSize, offsetParam);

        if (cvLoopIdx_ > 1) {
            WaitAicToAiv();
        }
        ubConsumeConfig.l1RequireVfComputeRealK = mte2RealK;
        if constexpr (wqmmConfig.weightFormat == CubeFormat::NZ) {
            vectorCompute_.WeightAntiQuantComputeNzNk(ubConsumeConfig, weightL1_[(cvLoopIdx_ & 1) * weightL1DbOffset_],
                l1ConsumeConfig, biasL1_[(cvLoopIdx_ & 1) * biasL1DbOffset_]);
        } else {
            vectorCompute_.WeightAntiQuantCompute(
                ubConsumeConfig, weightL1_[(cvLoopIdx_ & 1) * weightL1DbOffset_], l1ConsumeConfig, nullptr);
        }

        SetAivToAic();
        vectorCompute_.SetVToMTE2();
    }
}

GMM_WQ_BASIC_BLOCK_TEMPLATE_PARAM
__aicore__ inline void GMM_WQ_BASIC_BLOCK_CLASS::ComputeBasicBlockAic(const BasicBlockOffsetParam &offsetParam)
{
    // with the current scheme the computed amount along N is never smaller than the loaded amount, so there is no N
    // loop
    for (uint64_t kbL1Offset = 0; kbL1Offset < offsetParam.kSize; kbL1Offset += offsetParam.kbL1Size, cvLoopIdx_++) {
        uint64_t kbL1RealSize = (kbL1Offset + offsetParam.kbL1Size) >= offsetParam.kSize
                                    ? offsetParam.kSize - kbL1Offset
                                    : offsetParam.kbL1Size;
        if constexpr (IsMxA8W4<xType, wqmmConfig.antiQuantType>()) {
            cubeCompute_.WaitScaleMTE1ToMTE2(kbL1Offset);
            cubeCompute_.CopyMxScaleGmToL1(offsetParam, kbL1Offset, cvLoopIdx_);
        }
        cubeCompute_.WaitMTE1ToMTE2(cvLoopIdx_);
        cubeCompute_.CopyAAndBiasGmToL1(offsetParam, kbL1Offset, kbL1RealSize, offsetParam.nL1Size, cvLoopIdx_);
        WaitAivToAic();
        cubeCompute_.LaunchMatmul(weightL1_[(cvLoopIdx_ & 1) * weightL1DbOffset_], kbL1Offset, kbL1RealSize,
            offsetParam, cvLoopIdx_);  // mte1 mmad fixp pipeline
        cubeCompute_.SetMTE1ToMTE2(cvLoopIdx_);
        if constexpr (IsMxA8W4<xType, wqmmConfig.antiQuantType>()) {
            cubeCompute_.SetScaleMTE1ToMTE2(kbL1Offset, offsetParam);
        }
        SetAicToAiv();
    }
    cubeCompute_.GetTensorC(offsetParam);
    cubeCompute_.ClearAFullLoadFlag();  // clear the set sync flag left by previous loops during the full load of A
}

GMM_WQ_BASIC_BLOCK_TEMPLATE_PARAM
__aicore__ inline void GMM_WQ_BASIC_BLOCK_CLASS::ComputeBasicBlock(
    const BasicBlockOffsetParam &offsetParam, const BasicBlockOffsetParam &lastOffsetParam)
{
    if ASCEND_IS_AIV {
        if constexpr ((wqmmConfig.weightFormat != CubeFormat::NZ && !wqmmConfig.bTrans) ||
                      (wqmmConfig.weightFormat == CubeFormat::NZ && wqmmConfig.bTrans)) {
            ComputeBasicBlockAivNdKnNzNk(offsetParam);
        } else {
            ComputeBasicBlockAivNdNkNzKn(offsetParam);
        }
    } else {
        ComputeBasicBlockAic(offsetParam);
    }
}

GMM_WQ_BASIC_BLOCK_TEMPLATE_PARAM
__aicore__ inline void GMM_WQ_BASIC_BLOCK_CLASS::PrefetchA(uint64_t aPrefetchSize, uint64_t xSizeLimit)
{
    cubeCompute_.PrefetchA(aPrefetchSize, xSizeLimit);
}

GMM_WQ_BASIC_BLOCK_TEMPLATE_PARAM
__aicore__ inline void GMM_WQ_BASIC_BLOCK_CLASS::End(const BasicBlockOffsetParam &offsetParam)
{
    if ASCEND_IS_AIC {
        cubeCompute_.EndSync(cvLoopIdx_);
    } else {
        if (cvLoopIdx_ > 0) {
            WaitAicToAiv();
        }
        if (cvLoopIdx_ > 1) {
            WaitAicToAiv();
        }
        vectorCompute_.End();
    }
}

GMM_WQ_BASIC_BLOCK_TEMPLATE_PARAM
__aicore__ inline void GMM_WQ_BASIC_BLOCK_CLASS::SetAivToAic()
{
#ifndef __CCE_KT_TEST__
    CrossCoreSetFlag<SYNC_MODE4, PIPE_MTE3>(SYNC_AIC_AIV_FLAG);
#endif
}

GMM_WQ_BASIC_BLOCK_TEMPLATE_PARAM
__aicore__ inline void GMM_WQ_BASIC_BLOCK_CLASS::WaitAivToAic()
{
#ifndef __CCE_KT_TEST__
#ifndef __DAV_310R6__
    CrossCoreWaitFlag<SYNC_MODE4, PIPE_MTE1>(SYNC_AIC_AIV_FLAG + FLAG_ID_MAX);
#endif
    CrossCoreWaitFlag<SYNC_MODE4, PIPE_MTE1>(SYNC_AIC_AIV_FLAG);
#endif
}

GMM_WQ_BASIC_BLOCK_TEMPLATE_PARAM
__aicore__ inline void GMM_WQ_BASIC_BLOCK_CLASS::SetAicToAiv()
{
#ifndef __CCE_KT_TEST__
#if !defined(__DAV_310R6__)
    CrossCoreSetFlag<SYNC_MODE4, PIPE_MTE1>(SYNC_AIV_AIC_FLAG + FLAG_ID_MAX);
#endif
    CrossCoreSetFlag<SYNC_MODE4, PIPE_MTE1>(SYNC_AIV_AIC_FLAG);
#endif
}

GMM_WQ_BASIC_BLOCK_TEMPLATE_PARAM
__aicore__ inline void GMM_WQ_BASIC_BLOCK_CLASS::WaitAicToAiv()
{
#ifndef __CCE_KT_TEST__
    CrossCoreWaitFlag<SYNC_MODE4, PIPE_MTE3>(SYNC_AIV_AIC_FLAG);
#endif
}
}  // namespace WeightQuantBatchMatmulV2::Arch35

#endif  // GROUPED_MATMUL_WEIGHT_QUANT_BASIC_BLOCK_H
