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
 * Description: grouped_weight_quant_batch_matmul_tiling header file
 * Create: 2026-09-22
 * Note:
 * History: 2026-09-22 port from cam_async repository
 */
/*!
 * \file grouped_weight_quant_batch_matmul_tiling.h
 * \brief
 */
#ifndef GROUPED_WEIGHT_QUANT_BATCH_MATMUL_TILING_H
#define GROUPED_WEIGHT_QUANT_BATCH_MATMUL_TILING_H

#include <graph/utils/type_utils.h>
#include <sstream>
#include <map>
#include <unordered_set>

#include "../grouped_matmul_tiling.h"
#include "../../../op_kernel/arch35/grouped_matmul_tiling_data_apt.h"
#include "log/log.h"
#include "register/op_impl_registry.h"

namespace optiling {
constexpr uint32_t X_IDX = 0;
constexpr uint32_t WEIGHT_IDX = 1;
constexpr uint32_t BIAS_IDX = 2;
constexpr uint32_t ANTIQUANT_SCALE_IDX = 5;
constexpr uint32_t ANTIQUANT_OFFSET_IDX = 6;
constexpr uint64_t ATTR_SPLIT_ITEM_IDX = 0;
constexpr uint64_t ATTR_TRANS_W_IDX = 2;
constexpr uint64_t ATTR_TRANS_X_IDX = 3;
constexpr uint64_t ATTR_GROUPTYPE_IDX = 4;
constexpr uint32_t ATTR_GROUP_LIST_TYPE_IDX = 5;

constexpr uint32_t MAX_X_DIM = 6UL;
constexpr uint32_t MIN_X_DIM = 2UL;

constexpr uint32_t BASIC_BLOCK_BASE_M = 256;
constexpr uint32_t BASIC_BLOCK_BASE_M_WITH_BIAS = 240;
constexpr uint32_t BASIC_BLOCK_BASE_N = 256;
constexpr uint32_t BASIC_BLOCK_BASE_K = 64;
constexpr uint32_t BASIC_BLOCK_BASE_N_MIN = 128;
constexpr uint32_t STEP_K_4 = 4;
constexpr uint32_t STEP_K_3 = 3;
constexpr uint32_t DEPTH_8 = 8;
constexpr uint32_t BUFFER_NUM_2 = 2;

constexpr uint32_t SCALE_FACTOR_DEFAULT = 1;
constexpr uint32_t SCALE_FACTOR_MIN = 1;
constexpr uint32_t SCALE_FACTOR_B_BIT = 8;
constexpr uint32_t SCALE_FACTOR_M_BIT = 16;
constexpr uint32_t SCALE_FACTOR_N_BIT = 24;

constexpr int32_t B16_DATA_SIZE = 2;
constexpr int32_t B8_DATA_SIZE = 1;

constexpr uint32_t MX_GROUP_SIZE = 32;

struct TailBlockResplitParam {
    uint32_t mainBlockSize = 0;
    uint64_t mainBlockCount = 0;
    uint16_t firstTailBlockSize = 0;
    uint16_t secondTailBlockSize = 0;
    uint16_t firstTailBlockCount = 0;
    uint16_t secondTailBlockCount = 0;
};

enum class GroupType : int8_t {
    NO_SPLIT = -1,
    SPLIT_M = 0,
    SPLIT_N = 1,
    SPLIT_K = 2,
};

enum class QuantType : uint8_t {
    NONE = 0,
    PER_TENSOR = 1,
    PER_CHANNEL = 2,
    PER_GROUP = 3,
    MX = 4
};

enum class WeightFormat : uint8_t {
    ND = 0,
    FRACTAL_NZ = 1,
};

// corresponds to bit 0: platform major class
enum class SocVersionType : uint8_t {
    RESERVERD = 0,
    SUPPORT_L0C_TO_OUT = 1,
    SUPPORT_L1_TO_BT_BF16 = 2,
};

// corresponds to bit 1: platform minor class
enum class SocVersionSubType : uint8_t {
    RESERVERD = 0,
};

// corresponds to bits 2-3: antiquantization scenario
enum class QuantizationScenario : uint8_t {
    DEFAULT = 0,
};

// corresponds to bit 4: algorithm major class
enum class OptimizationAlgorithmCategory : uint8_t {
    VECTOR_ANTIQUANT = 0,
    MULTI_SCALE_DEQUANT = 1,
    FIXPIPE_ANTIQUANT = 2,
};

// corresponds to bit 5: algorithm minor class
enum class OptimizationAlgorithmSubCategory : uint8_t {
    VDEFAULT = 0,
    SPLIT_K = 1,
    N_FIRST_TAIL_RESPLIT = 2,
    N_FIRST_BASIC_BLOCK = 3,
};

// corresponds to bits 6-9: fixp template custom combination
enum class FixpipeConfiguration : uint16_t {
    A_NORMAL_LOAD = 0,
    A_SINGLE_M_SINGLE_K_FULL_LOAD = 1,
};

enum class CustomSplitKConfiguration : uint8_t {
    A_NORMAL_LOAD = 0,
    A_MK_FULL_LOAD = 1,
};

// corresponds to bit 14: transpose scenario, transA/transB
enum class TransposeSituation : uint8_t {
    A_NOT_TRANS_B_NOT_TRANS = 0,
    A_NOT_TRANS_B_TRANS = 1,
    A_TRANS_B_NOT_TRANS = 2,
    A_TRANS_B_TRANS = 3,
};

// corresponds to bit 17: whether the optional inputs exist, hasAntiquantOffset/hasBias
enum class OptionInputSituation : uint8_t {
    ANTIQUANT_OFFSET_NOT_EXIST_BIAS_NOT_EXIST = 0,
    ANTIQUANT_OFFSET_NOT_EXIST_BIAS_EXIST = 1,
    ANTIQUANT_OFFSET_EXIST_BIAS_NOT_EXIST = 2,
    ANTIQUANT_OFFSET_EXIST_BIAS_EXIST = 3,
    ANTIQUANT_OFFSET_NOT_EXIST_BIAS_FP32_EXIST = 4,
    ANTIQUANT_OFFSET_EXIST_BIAS_FP32_EXIST = 6,
};

enum class Mte2Configuration : uint8_t {
    MTE2_INNER_SIZE_512_BUF_NUM_2 = 0,
    MTE2_INNER_SIZE_512_BUF_NUM_4 = 1,
    MTE2_INNER_SIZE_1024_BUF_NUM_2 = 2,
    MTE2_INNER_SIZE_256_BUF_NUM_4 = 3,
    MTE2_INNER_SIZE_512_BUF_NUM_DEFAULT = 4,  // w8 and w4 reuse one set of settings in non-performance scenarios
    MTE2_INNER_SIZE_384_BUF_NUM_3 = 5,
    MTE2_INNER_SIZE_DYNAMIC_BUF_NUM_4 = 15,  // MXA8W4 NZ scenario, the K axis length is adjusted dynamically
};

class TilingKeyConfigure {
  public:
    // corresponds to bits 0-1: platform major class, platform minor class
    uint8_t socVersionType = 0;

    // corresponds to bits 2-3: antiquantization scenario
    uint8_t quantizationScenario = 0;

    // corresponds to bits 4-5: algorithm major class, algorithm minor class
    uint8_t algorithm = 0;

    // corresponds to bit 14: transpose scenario, transA/transB
    uint8_t transposeSituation = 0;

    // corresponds to bit 15: dequantization type perchannel/pertensor/perGroup
    uint8_t antiquantType = 0;

    // corresponds to bit 16: quantization type perchannel/perTensor/None
    uint8_t quantType = 0;

    // corresponds to bit 17: whether the optional inputs exist, hasAntiquantOffset/hasBias
    uint8_t optionInputSituation = 0;

    // corresponds to bit 18: weight data type weightNd/weightNz
    uint8_t weightFormat = 0;

    // corresponds to bits 6-9: template custom combination
    uint16_t templateCustom = 0;

    // corresponds to bits 10-13: reserved bits for api constant folding
    uint16_t apiConstexpr = 0;

  public:
    void PrintTilingKeyLog() const
    {
        std::stringstream ss;
        ss << "socVersionType: " << static_cast<uint32_t>(this->socVersionType)
           << " quantizationScenario: " << static_cast<uint32_t>(this->quantizationScenario)
           << " algorithm: " << static_cast<uint32_t>(this->algorithm)
           << " transposeSituation: " << static_cast<uint32_t>(this->transposeSituation)
           << " antiquantType: " << static_cast<uint32_t>(this->antiquantType)
           << " quantType: " << static_cast<uint32_t>(this->quantType)
           << " optionInputSituation: " << static_cast<uint32_t>(this->optionInputSituation)
           << " weightFormat: " << static_cast<uint32_t>(this->weightFormat)
           << " templateCustom: " << static_cast<uint32_t>(this->templateCustom)
           << " apiConstexpr: " << static_cast<uint32_t>(this->apiConstexpr);
        OP_LOGI("GMMWeightQuantBatchMatmul", "tilingKeyConfigure: %s", ss.str().c_str());
        return;
    }

    uint64_t GenTilingKey() const;
};

class GroupedWeightQuantBatchMatmulTiling {
  public:
    bool SetTiling(gert::TilingContext *context);

  protected:
    bool SetShapeList(const gert::TilingContext *context);
    bool CheckEmptyTensor(const gert::TilingContext *context);
    bool CheckTensorListSize(const gert::TilingContext *context);
    bool CheckTensorDtype(const gert::TilingContext *context, uint32_t attrIdx, size_t idx,
        const ge::DataType &tensorDtype, const std::string &tensorType) const;
    bool IsNzFormat(const gert::TilingContext *context, uint32_t attrIdx, size_t idx) const;
    bool CheckXAndWeightFormat(const gert::TilingContext *context, size_t idx) const;
    bool CheckNotNullPtr(const gert::TilingContext *context, uint32_t attrIdx, size_t idx) const;
    bool CheckNotNull(const gert::TilingContext *context, size_t idx) const;
    bool CheckTensorDimEqualTarget(const gert::TilingContext *context, uint32_t attrIdx, size_t idx, uint32_t targetDim,
        const std::string &tensorType) const;
    bool CheckTensorDimSingleXSingleWeightSingleY(const gert::TilingContext *context, size_t idx) const;
    bool CheckTensorDimMultiXMultiWeightMultiY(const gert::TilingContext *context, size_t idx) const;
    bool CheckTensorDim(const gert::TilingContext *context, size_t idx) const;
    bool CheckTensorShape(
        const gert::TilingContext *context, uint32_t attrIdx, size_t idx, const std::string &tensorType) const;
    bool CheckDimValue(const gert::TilingContext *context, size_t idx) const;
    bool CheckWeightInnerAxisEven(const gert::TilingContext *context, size_t idx) const;
    bool CheckEveryTensor(const gert::TilingContext *context) const;
    bool CheckGroupList(const gert::TilingContext *context) const;
    bool AnalyzeAttr(const gert::TilingContext *context);
    bool AnalyzeInput(const gert::TilingContext *context);
    bool CalcResplitTiling(const gert::TilingContext *context);
    bool SetBaseTiling();
    void SetMatMulTiling();
    void SetTilingKey(gert::TilingContext *context);
    bool SetCustomParam(gert::TilingContext *context);
    bool IsA16W4ND() const;
    bool IsMxA8W4() const;
    bool CheckPerTokenScale(const gert::TilingContext *context) const;
    bool CheckUnsupportDataFlow(const gert::TilingContext *context) const;
    bool CheckAntiQuantDtype(const gert::TilingContext *context) const;
    bool CheckBiasDtype(const gert::TilingContext *context) const;
    bool CheckGroupTypeAndSplitItem(const gert::TilingContext *context) const;
    bool CheckTransposeStatus(const gert::TilingContext *context) const;
    bool SetShapeListSplitMSingleXSingleWeightSingleY(const gert::TilingContext *context);
    bool SetShapeListMultiXMultiWeightMultiY(const gert::TilingContext *context);
    uint16_t GetTensorListSize(const gert::TilingContext *context, uint32_t attrIdx) const;
    void GetNumOfInputs(const gert::TilingContext *context);
    bool SetAntiquantGroupSize(const gert::TilingContext *context);
    bool CheckGroupSize(const gert::TilingContext *context) const;
    bool GetC0Size(const gert::TilingContext *context, ge::DataType dtype, uint64_t &c0Size) const;
    void CalcFullNumBlocksResplitTiling(uint64_t c0Size);
    void CalcNoFullNumBlocksResplitTiling(uint64_t c0Size);
    bool CheckResplitTilingResult(const gert::TilingContext *context) const;
    void PrintInputParam(const gert::TilingContext *context) const;
    void PrintTilingResult(const gert::TilingContext *context);
    bool EnableTailResplit() const;

  private:
    int32_t mList_[GroupedMatmul::MAX_TENSOR_CONT] = {0};
    int32_t kList_[GroupedMatmul::MAX_TENSOR_CONT] = {0};
    int32_t nList_[GroupedMatmul::MAX_TENSOR_CONT] = {0};

    bool transA_ = false;
    bool transB_ = false;
    bool isSingleX_ = true;
    bool isSingleWeight_ = true;
    bool isSingleY_ = true;
    bool hasBias_ = false;
    bool weightNzFlag_ = false;
    bool hasAntiquantOffset_ = false;

    uint64_t mSize_ = 0;
    uint64_t kSize_ = 0;
    uint64_t nSize_ = 0;
    uint64_t nSizeOri_ = 0;
    GroupType groupType_ = GroupType::SPLIT_M;
    int64_t splitItem_ = 0;
    uint32_t groupNum_ = 0;
    uint32_t groupListType_ = 0;
    uint32_t coreNum_ = 0;
    uint32_t groupSize_ = 0;
    uint8_t cubeNumBlocksN_ = 0;

    uint16_t numX_ = 0;
    uint16_t numWeight_ = 0;
    uint16_t numBias_ = 0;
    uint16_t numAntiquantScale_ = 0;
    uint16_t numAntiquantOffset_ = 0;

    ge::DataType xDType_ = ge::DT_UNDEFINED;
    ge::DataType weightDtype_ = ge::DT_UNDEFINED;
    ge::DataType biasDtype_ = ge::DT_UNDEFINED;
    ge::DataType antiquantScaleDtype_ = ge::DT_UNDEFINED;
    ge::DataType antiquantOffsetDtype_ = ge::DT_UNDEFINED;

    TailBlockResplitParam resplitParam_;
    TilingKeyConfigure tilingKeyConfig_;
    GroupedMatmulTilingData::GMMWeightQuantTilingData tilingData_;
};
}  // namespace optiling

#endif  // GROUPED_WEIGHT_QUANT_BATCH_MATMUL_TILING_H