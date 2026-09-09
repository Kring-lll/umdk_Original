/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include <string>
#include <register/op_impl_registry.h>
#include "err/ops_err.h"

namespace ops {
namespace {
constexpr size_t QUERY = 0;
constexpr size_t SELECTION_KEY = 1;
constexpr size_t SELECTION_BLOCK_TABLE = 6;
constexpr size_t SELECTION_BLOCK_STATUS = 10;
constexpr size_t ATTENTION_OUT = 0;
constexpr size_t SELECTION_CACHE_OUT = 1;
constexpr size_t SELECTION_BLOCK_TABLE_OUT = 2;
constexpr size_t SELECTION_BLOCK_STATUS_OUT = 3;
constexpr size_t SELECTION_ACTUAL_SEQ_OUT = 4;
constexpr uint32_t LAYOUT_QUERY_ATTR = 4;
constexpr uint32_t ROPE_HEAD_DIM_ATTR = 12;
} // namespace

ge::graphStatus InferShapeGatherSelectionSparseFlashAttention(gert::InferShapeContext *context)
{
    OP_CHECK_IF(context == nullptr,
                OP_LOGE("GatherSelectionSparseFlashAttention", "InferShapeContext is nullptr"),
                return ge::GRAPH_FAILED);
    const gert::Shape *query = context->GetInputShape(QUERY);
    const gert::Shape *selectionKey = context->GetInputShape(SELECTION_KEY);
    const gert::Shape *selectionBlockTable = context->GetInputShape(SELECTION_BLOCK_TABLE);
    const gert::Shape *selectionBlockStatus = context->GetInputShape(SELECTION_BLOCK_STATUS);
    OP_CHECK_NULL_WITH_CONTEXT(context, query);
    OP_CHECK_NULL_WITH_CONTEXT(context, selectionKey);
    OP_CHECK_NULL_WITH_CONTEXT(context, selectionBlockTable);
    OP_CHECK_NULL_WITH_CONTEXT(context, selectionBlockStatus);

    gert::Shape *attentionOut = context->GetOutputShape(ATTENTION_OUT);
    gert::Shape *selectionCacheOut = context->GetOutputShape(SELECTION_CACHE_OUT);
    gert::Shape *selectionBlockTableOut = context->GetOutputShape(SELECTION_BLOCK_TABLE_OUT);
    gert::Shape *selectionBlockStatusOut = context->GetOutputShape(SELECTION_BLOCK_STATUS_OUT);
    gert::Shape *selectionActualSeqOut = context->GetOutputShape(SELECTION_ACTUAL_SEQ_OUT);
    OP_CHECK_NULL_WITH_CONTEXT(context, attentionOut);
    OP_CHECK_NULL_WITH_CONTEXT(context, selectionCacheOut);
    OP_CHECK_NULL_WITH_CONTEXT(context, selectionBlockTableOut);
    OP_CHECK_NULL_WITH_CONTEXT(context, selectionBlockStatusOut);
    OP_CHECK_NULL_WITH_CONTEXT(context, selectionActualSeqOut);

    auto attrs = context->GetAttrs();
    OP_CHECK_NULL_WITH_CONTEXT(context, attrs);
    const char *layout = attrs->GetAttrPointer<char>(LAYOUT_QUERY_ATTR);
    const int64_t *ropeHeadDim = attrs->GetAttrPointer<int64_t>(ROPE_HEAD_DIM_ATTR);
    OP_CHECK_NULL_WITH_CONTEXT(context, layout);
    OP_CHECK_NULL_WITH_CONTEXT(context, ropeHeadDim);
    OP_CHECK_IF(std::string(layout) != "TND",
                OP_LOGE("GatherSelectionSparseFlashAttention", "layout_query must be TND"),
                return ge::GRAPH_FAILED);
    OP_CHECK_IF(*ropeHeadDim < 0,
                OP_LOGE("GatherSelectionSparseFlashAttention", "rope_head_dim must be non-negative"),
                return ge::GRAPH_FAILED);

    *attentionOut = *query;
    const size_t dimNum = query->GetDimNum();
    constexpr size_t dAxis = 2;
    OP_CHECK_IF(dimNum <= dAxis,
                OP_LOGE("GatherSelectionSparseFlashAttention", "query rank is incompatible with layout"),
                return ge::GRAPH_FAILED);
    if (query->GetDim(dAxis) != -1) {
        OP_CHECK_IF(*ropeHeadDim > query->GetDim(dAxis),
                    OP_LOGE("GatherSelectionSparseFlashAttention", "rope_head_dim must not exceed query D"),
                    return ge::GRAPH_FAILED);
        attentionOut->SetDim(dAxis, query->GetDim(dAxis) - *ropeHeadDim);
    }

    *selectionCacheOut = *selectionKey;
    *selectionBlockTableOut = *selectionBlockTable;
    *selectionBlockStatusOut = *selectionBlockStatus;
    *selectionActualSeqOut = *selectionBlockTable;
    OP_CHECK_IF(selectionActualSeqOut->GetDimNum() == 0,
                OP_LOGE("GatherSelectionSparseFlashAttention", "selection block table must have rank >= 1"),
                return ge::GRAPH_FAILED);
    selectionActualSeqOut->SetDimNum(selectionActualSeqOut->GetDimNum() - 1);
    return ge::GRAPH_SUCCESS;
}

ge::graphStatus InferDataTypeGatherSelectionSparseFlashAttention(gert::InferDataTypeContext *context)
{
    OP_CHECK_IF(context == nullptr,
                OP_LOGE("GatherSelectionSparseFlashAttention", "InferDataTypeContext is nullptr"),
                return ge::GRAPH_FAILED);
    context->SetOutputDataType(ATTENTION_OUT, context->GetInputDataType(QUERY));
    context->SetOutputDataType(SELECTION_CACHE_OUT, context->GetInputDataType(SELECTION_KEY));
    context->SetOutputDataType(SELECTION_BLOCK_TABLE_OUT, context->GetInputDataType(SELECTION_BLOCK_TABLE));
    context->SetOutputDataType(SELECTION_BLOCK_STATUS_OUT, context->GetInputDataType(SELECTION_BLOCK_STATUS));
    context->SetOutputDataType(SELECTION_ACTUAL_SEQ_OUT, ge::DT_INT32);
    return ge::GRAPH_SUCCESS;
}

IMPL_OP_INFERSHAPE(GatherSelectionSparseFlashAttention)
    .InferShape(InferShapeGatherSelectionSparseFlashAttention)
    .InferDataType(InferDataTypeGatherSelectionSparseFlashAttention);
} // namespace ops
