/**
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include "gather_selection_kv_cache_custom_tiling.h"

#include <algorithm>

namespace optiling {
namespace {

constexpr int32_t SEL_K_ROPE_IDX = 0;
constexpr int32_t SEL_KV_CACHE_IDX = 1;
constexpr int32_t SEL_KV_BLOCK_TABLE_IDX = 2;
constexpr int32_t SEL_KV_BLOCK_STATUS_IDX = 3;
constexpr int32_t SEL_TOPK_INDICES_IDX = 4;
constexpr int32_t FULL_K_ROPE_IDX = 5;
constexpr int32_t FULL_KV_CACHE_IDX = 6;
constexpr int32_t FULL_KV_BLOCK_TABLE_IDX = 7;
constexpr int32_t FULL_KV_ACTSEQ_IDX = 8;
constexpr int32_t FULL_Q_ACTSEQ_IDX = 9;

constexpr int64_t MAX_TOPK_NUM = 2048;
constexpr int64_t MIN_REUSE_VEC_TOPK = 33;
constexpr int64_t MAX_K_ROPE_DIM = 64;
constexpr int64_t MAX_KV_CACHE_DIM = 656;
constexpr int64_t TOPK_SORT_UNIT = 32;
constexpr int64_t INT32_BLOCK_NUM = 8;
constexpr int64_t SORT_SCRATCH_ARRAY_NUM = 8;

template <typename T>
T CeilAlign(T value, T align)
{
    return align == 0 ? value : (value + align - 1) / align * align;
}

bool IsCacheDtype(ge::DataType dtype)
{
    return dtype == ge::DT_FLOAT16 || dtype == ge::DT_BF16 || dtype == ge::DT_INT8;
}

} // namespace

ge::graphStatus GatherSelectionKvCacheCustomTiling::GetPlatformInfo()
{
    auto platformInfo = context_->GetPlatformInfo();
    OPS_ERR_IF(platformInfo == nullptr,
        OPS_LOG_E(context_->GetNodeName(), "get platformInfo nullptr."), return ge::GRAPH_FAILED);
    auto platform = platform_ascendc::PlatformAscendC(platformInfo);
    coreNum_ = platform.GetCoreNumAiv();
    OPS_ERR_IF(coreNum_ <= 0,
        OPS_LOG_E(context_->GetNodeName(), "AIV core num must be greater than 0."), return ge::GRAPH_FAILED);

    uint64_t ubSize = 0;
    platform.GetCoreMemSize(platform_ascendc::CoreMemType::UB, ubSize);
    ubSize_ = static_cast<int64_t>(ubSize);
    systemWorkspaceSize_ = static_cast<int64_t>(platform.GetLibApiWorkSpaceSize());
    OPS_ERR_IF(ubSize_ <= 0,
        OPS_LOG_E(context_->GetNodeName(), "UB size must be greater than 0."), return ge::GRAPH_FAILED);
    return ge::GRAPH_SUCCESS;
}

ge::graphStatus GatherSelectionKvCacheCustomTiling::GetAttrsAndShapes()
{
    auto attrs = context_->GetAttrs();
    OPS_ERR_IF(attrs == nullptr,
        OPS_LOG_E(context_->GetNodeName(), "get attrs nullptr."), return ge::GRAPH_FAILED);
    const int64_t* blockSize = attrs->GetAttrPointer<int64_t>(0);
    selTopKBlockSize_ = blockSize == nullptr ? 1 : *blockSize;
    OPS_ERR_IF(selTopKBlockSize_ != 1,
        OPS_LOG_E(context_->GetNodeName(),
            "custom kernel only supports selection_topk_block_size=1, but got %ld.", selTopKBlockSize_),
        return ge::GRAPH_FAILED);

    auto topkShapeIn = context_->GetInputShape(SEL_TOPK_INDICES_IDX);
    auto statusShapeIn = context_->GetInputShape(SEL_KV_BLOCK_STATUS_IDX);
    auto fullBlockTableIn = context_->GetInputShape(FULL_KV_BLOCK_TABLE_IDX);
    OPS_ERR_IF(topkShapeIn == nullptr || statusShapeIn == nullptr || fullBlockTableIn == nullptr,
        OPS_LOG_E(context_->GetNodeName(), "topk/status/full block table shape is nullptr."),
        return ge::GRAPH_FAILED);
    auto topkShape = topkShapeIn->GetStorageShape();
    auto statusShape = statusShapeIn->GetStorageShape();
    auto fullBlockTableShape = fullBlockTableIn->GetStorageShape();

    OPS_ERR_IF(fullBlockTableShape.GetDimNum() != 2,
        OPS_LOG_E(context_->GetNodeName(), "full_kv_block_table must be 2D."), return ge::GRAPH_FAILED);
    tokenNum_ = fullBlockTableShape.GetDim(0);
    tilingData_.set_fullMaxBlockNum(fullBlockTableShape.GetDim(1));

    // The first version intentionally limits S=1 and H=1. This makes tokens independent
    // and prevents a later seq from reading selected-cache data while another core rewrites it.
    if (topkShape.GetDimNum() == 3) { // TND: [B, 1, TOPK] when S=1
        OPS_ERR_IF(topkShape.GetDim(0) != tokenNum_ || topkShape.GetDim(1) != 1,
            OPS_LOG_E(context_->GetNodeName(),
                "TND custom kernel requires shape [B,1,TOPK] and B=%ld.", tokenNum_),
            return ge::GRAPH_FAILED);
        topk_ = topkShape.GetDim(2);
        OPS_ERR_IF(statusShape.GetDimNum() != 3 || statusShape.GetDim(0) != tokenNum_ ||
                       statusShape.GetDim(1) != 1 || statusShape.GetDim(2) != topk_ + 1,
            OPS_LOG_E(context_->GetNodeName(), "selection_kv_block_status TND shape mismatch."),
            return ge::GRAPH_FAILED);
    } else if (topkShape.GetDimNum() == 4) { // BSND: [B, 1, 1, TOPK]
        OPS_ERR_IF(topkShape.GetDim(0) != tokenNum_ || topkShape.GetDim(1) != 1 || topkShape.GetDim(2) != 1,
            OPS_LOG_E(context_->GetNodeName(),
                "BSND custom kernel requires shape [B,1,1,TOPK] and B=%ld.", tokenNum_),
            return ge::GRAPH_FAILED);
        topk_ = topkShape.GetDim(3);
        OPS_ERR_IF(statusShape.GetDimNum() != 4 || statusShape.GetDim(0) != tokenNum_ ||
                       statusShape.GetDim(1) != 1 || statusShape.GetDim(2) != 1 ||
                       statusShape.GetDim(3) != topk_ + 1,
            OPS_LOG_E(context_->GetNodeName(), "selection_kv_block_status BSND shape mismatch."),
            return ge::GRAPH_FAILED);
    } else {
        OPS_LOG_E(context_->GetNodeName(), "selection_topk_indices must be 3D or 4D.");
        return ge::GRAPH_FAILED;
    }
    OPS_ERR_IF(topk_ < MIN_REUSE_VEC_TOPK || topk_ > MAX_TOPK_NUM,
        OPS_LOG_E(context_->GetNodeName(), "custom ReuseVec requires 33 <= TOPK <= 2048, got %ld.", topk_),
        return ge::GRAPH_FAILED);

    auto selCacheIn = context_->GetInputShape(SEL_KV_CACHE_IDX);
    auto selRopeIn = context_->GetInputShape(SEL_K_ROPE_IDX);
    auto fullCacheIn = context_->GetInputShape(FULL_KV_CACHE_IDX);
    auto fullRopeIn = context_->GetInputShape(FULL_K_ROPE_IDX);
    auto selTableIn = context_->GetInputShape(SEL_KV_BLOCK_TABLE_IDX);
    OPS_ERR_IF(selCacheIn == nullptr || selRopeIn == nullptr || fullCacheIn == nullptr ||
                   fullRopeIn == nullptr || selTableIn == nullptr,
        OPS_LOG_E(context_->GetNodeName(), "cache/table shape is nullptr."), return ge::GRAPH_FAILED);

    auto selCacheShape = selCacheIn->GetStorageShape();
    auto selRopeShape = selRopeIn->GetStorageShape();
    auto fullCacheShape = fullCacheIn->GetStorageShape();
    auto fullRopeShape = fullRopeIn->GetStorageShape();
    auto selTableShape = selTableIn->GetStorageShape();
    OPS_ERR_IF(selCacheShape.GetDimNum() != 3 || fullCacheShape.GetDimNum() != 3 ||
                   selTableShape.GetDimNum() != 2,
        OPS_LOG_E(context_->GetNodeName(), "selection/full KV cache must be 3D and selection table must be 2D."),
        return ge::GRAPH_FAILED);

    tilingData_.set_selKvBlockSize(selCacheShape.GetDim(1));
    tilingData_.set_kvCacheDim(selCacheShape.GetDim(2));
    tilingData_.set_selMaxBlockNum(selTableShape.GetDim(1));
    OPS_ERR_IF(selTableShape.GetDim(0) != tokenNum_,
        OPS_LOG_E(context_->GetNodeName(), "selection block table rows must equal B=%ld.", tokenNum_),
        return ge::GRAPH_FAILED);
    OPS_ERR_IF(selCacheShape.GetDim(0) < tokenNum_ * selTableShape.GetDim(1),
        OPS_LOG_E(context_->GetNodeName(), "selection cache block num is too small."), return ge::GRAPH_FAILED);

    tilingData_.set_fullKvBlockSize(fullCacheShape.GetDim(1));
    OPS_ERR_IF(fullCacheShape.GetDim(2) != selCacheShape.GetDim(2),
        OPS_LOG_E(context_->GetNodeName(), "selection/full KV cache last dim mismatch."), return ge::GRAPH_FAILED);
    OPS_ERR_IF(tilingData_.get_kvCacheDim() > MAX_KV_CACHE_DIM,
        OPS_LOG_E(context_->GetNodeName(), "kvCacheDim exceeds %ld.", MAX_KV_CACHE_DIM), return ge::GRAPH_FAILED);

    if (selRopeShape.GetDimNum() == 1) {
        OPS_ERR_IF(selRopeShape.GetDim(0) != 0 || fullRopeShape.GetDimNum() != 1 || fullRopeShape.GetDim(0) != 0,
            OPS_LOG_E(context_->GetNodeName(), "quantized rope inputs must both be empty 1D tensors."),
            return ge::GRAPH_FAILED);
        tilingData_.set_ifQuant(1);
        tilingData_.set_kRopeDim(0);
    } else {
        OPS_ERR_IF(selRopeShape.GetDimNum() != 3 || fullRopeShape.GetDimNum() != 3,
            OPS_LOG_E(context_->GetNodeName(), "rope cache must be 3D or empty 1D."), return ge::GRAPH_FAILED);
        OPS_ERR_IF(selRopeShape.GetDim(0) != selCacheShape.GetDim(0) ||
                       selRopeShape.GetDim(1) != selCacheShape.GetDim(1) ||
                       fullRopeShape.GetDim(0) != fullCacheShape.GetDim(0) ||
                       fullRopeShape.GetDim(1) != fullCacheShape.GetDim(1) ||
                       fullRopeShape.GetDim(2) != selRopeShape.GetDim(2),
            OPS_LOG_E(context_->GetNodeName(), "rope/cache block shape mismatch."), return ge::GRAPH_FAILED);
        tilingData_.set_ifQuant(0);
        tilingData_.set_kRopeDim(selRopeShape.GetDim(2));
        OPS_ERR_IF(tilingData_.get_kRopeDim() > MAX_K_ROPE_DIM,
            OPS_LOG_E(context_->GetNodeName(), "kRopeDim exceeds %ld.", MAX_K_ROPE_DIM), return ge::GRAPH_FAILED);
    }

    auto fullKvSeqIn = context_->GetInputShape(FULL_KV_ACTSEQ_IDX);
    auto fullQSeqIn = context_->GetInputShape(FULL_Q_ACTSEQ_IDX);
    OPS_ERR_IF(fullKvSeqIn == nullptr || fullQSeqIn == nullptr ||
                   fullKvSeqIn->GetStorageShape().GetDimNum() != 1 ||
                   fullQSeqIn->GetStorageShape().GetDimNum() != 1 ||
                   fullKvSeqIn->GetStorageShape().GetDim(0) != tokenNum_ ||
                   fullQSeqIn->GetStorageShape().GetDim(0) != tokenNum_,
        OPS_LOG_E(context_->GetNodeName(), "actual seq inputs must both be [B]."), return ge::GRAPH_FAILED);

    tilingData_.set_tokenNum(tokenNum_);
    tilingData_.set_topk(topk_);
    return ge::GRAPH_SUCCESS;
}

ge::graphStatus GatherSelectionKvCacheCustomTiling::CheckDtypes()
{
    auto cacheDesc = context_->GetInputDesc(SEL_KV_CACHE_IDX);
    auto selRopeDesc = context_->GetInputDesc(SEL_K_ROPE_IDX);
    auto fullCacheDesc = context_->GetInputDesc(FULL_KV_CACHE_IDX);
    auto fullRopeDesc = context_->GetInputDesc(FULL_K_ROPE_IDX);
    OPS_ERR_IF(cacheDesc == nullptr || selRopeDesc == nullptr || fullCacheDesc == nullptr || fullRopeDesc == nullptr,
        OPS_LOG_E(context_->GetNodeName(), "cache dtype desc is nullptr."), return ge::GRAPH_FAILED);
    cacheDtype_ = cacheDesc->GetDataType();
    OPS_ERR_IF(!IsCacheDtype(cacheDtype_) || selRopeDesc->GetDataType() != cacheDtype_ ||
                   fullCacheDesc->GetDataType() != cacheDtype_ || fullRopeDesc->GetDataType() != cacheDtype_,
        OPS_LOG_E(context_->GetNodeName(), "all cache/rope tensors must use the same supported dtype."),
        return ge::GRAPH_FAILED);

    constexpr int32_t intInputs[] = {SEL_KV_BLOCK_TABLE_IDX, SEL_KV_BLOCK_STATUS_IDX, SEL_TOPK_INDICES_IDX,
        FULL_KV_BLOCK_TABLE_IDX, FULL_KV_ACTSEQ_IDX, FULL_Q_ACTSEQ_IDX};
    for (int32_t index : intInputs) {
        auto desc = context_->GetInputDesc(index);
        OPS_ERR_IF(desc == nullptr || desc->GetDataType() != ge::DT_INT32,
            OPS_LOG_E(context_->GetNodeName(), "input %d must be int32.", index), return ge::GRAPH_FAILED);
    }
    return ge::GRAPH_SUCCESS;
}

ge::graphStatus GatherSelectionKvCacheCustomTiling::DoTiling()
{
    if (tokenNum_ == 0) {
        tilingData_.set_usedCoreNum(0);
        context_->SetBlockDim(1);
        tilingData_.set_workspaceSize(systemWorkspaceSize_);
        return ge::GRAPH_SUCCESS;
    }

    // All AIV cores enter both SyncAll barriers. Token planning/finalization is strided by core id.
    tilingData_.set_usedCoreNum(coreNum_);
    context_->SetBlockDim(coreNum_);

    const int64_t dtypeSize = ge::GetSizeByDataType(cacheDtype_);
    const int64_t kRopeUbSize = CeilAlign(tilingData_.get_kRopeDim() * dtypeSize, ubBlockSize_);
    const int64_t kvCacheUbSize = CeilAlign(tilingData_.get_kvCacheDim() * dtypeSize, ubBlockSize_);
    tilingData_.set_kRopeUbSize(kRopeUbSize);
    tilingData_.set_kvCacheUbSize(kvCacheUbSize);
    tilingData_.set_buffNum(2);

    const int64_t topkSortAlign = CeilAlign(topk_, TOPK_SORT_UNIT);
    const int64_t topkOneSortAlign = std::max(topkSortAlign, CeilAlign(topk_ + 1, INT32_BLOCK_NUM));
    const int64_t tableUb = CeilAlign(tilingData_.get_selMaxBlockNum() * static_cast<int64_t>(sizeof(int32_t)),
        ubBlockSize_);
    const int64_t actualSeqUb = ubBlockSize_;
    const int64_t statusUb = topkOneSortAlign * static_cast<int64_t>(sizeof(int32_t));
    const int64_t topkUb = topkSortAlign * static_cast<int64_t>(sizeof(int32_t));
    const int64_t scratchUb = SORT_SCRATCH_ARRAY_NUM * topkSortAlign * static_cast<int64_t>(sizeof(int32_t));
    const int64_t requiredUb = 2 * (kRopeUbSize + kvCacheUbSize) + tableUb + actualSeqUb + statusUb + topkUb + scratchUb;
    OPS_ERR_IF(requiredUb > ubSize_,
        OPS_LOG_E(context_->GetNodeName(), "UB is insufficient: require %ld, platform has %ld.", requiredUb, ubSize_),
        return ge::GRAPH_FAILED);

    const int64_t itemNum = tokenNum_ * topk_;
    int64_t offset = 0;
    tilingData_.set_planItemNum(itemNum);
    tilingData_.set_planValidNumOffset(offset);
    offset = CeilAlign(offset + tokenNum_ * static_cast<int64_t>(sizeof(int32_t)), ubBlockSize_);
    tilingData_.set_planTopkIdOffset(offset);
    offset = CeilAlign(offset + itemNum * static_cast<int64_t>(sizeof(int32_t)), ubBlockSize_);
    tilingData_.set_planInsertIdxOffset(offset);
    offset = CeilAlign(offset + itemNum * static_cast<int64_t>(sizeof(int32_t)), ubBlockSize_);
    tilingData_.set_planActionOffset(offset);
    offset = CeilAlign(offset + itemNum * static_cast<int64_t>(sizeof(int32_t)), ubBlockSize_);
    tilingData_.set_workspaceSize(offset + systemWorkspaceSize_);
    return ge::GRAPH_SUCCESS;
}

ge::graphStatus GatherSelectionKvCacheCustomTiling::PostTiling()
{
    context_->SetTilingKey(1);
    size_t* workspaces = context_->GetWorkspaceSizes(1);
    OPS_ERR_IF(workspaces == nullptr,
        OPS_LOG_E(context_->GetNodeName(), "workspace sizes is nullptr."), return ge::GRAPH_FAILED);
    workspaces[0] = static_cast<size_t>(tilingData_.get_workspaceSize());
    OPS_ERR_IF(context_->GetRawTilingData() == nullptr,
        OPS_LOG_E(context_->GetNodeName(), "raw tiling data is nullptr."), return ge::GRAPH_FAILED);
    tilingData_.SaveToBuffer(context_->GetRawTilingData()->GetData(), context_->GetRawTilingData()->GetCapacity());
    context_->GetRawTilingData()->SetDataSize(tilingData_.GetDataSize());
    return ge::GRAPH_SUCCESS;
}

ge::graphStatus GatherSelectionKvCacheCustomTiling::RunTiling()
{
    OPS_ERR_IF(context_ == nullptr, OPS_LOG_E("GatherSelectionKvCacheCustom", "context is nullptr."),
        return ge::GRAPH_FAILED);
    ge::graphStatus ret = GetPlatformInfo();
    if (ret != ge::GRAPH_SUCCESS) return ret;
    ret = GetAttrsAndShapes();
    if (ret != ge::GRAPH_SUCCESS) return ret;
    ret = CheckDtypes();
    if (ret != ge::GRAPH_SUCCESS) return ret;
    ret = DoTiling();
    if (ret != ge::GRAPH_SUCCESS) return ret;
    return PostTiling();
}

ge::graphStatus Tiling4GatherSelectionKvCacheCustom(gert::TilingContext* context)
{
    GatherSelectionKvCacheCustomTiling tiling(context);
    return tiling.RunTiling();
}

ge::graphStatus TilingPrepare4GatherSelectionKvCacheCustom(gert::TilingParseContext*)
{
    return ge::GRAPH_SUCCESS;
}

IMPL_OP_OPTILING(GatherSelectionKvCacheCustom)
    .Tiling(Tiling4GatherSelectionKvCacheCustom)
    .TilingParse<GatherSelectionKvCacheCustomCompileInfo>(TilingPrepare4GatherSelectionKvCacheCustom);

} // namespace optiling
