/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: add moe_combine_prefill_a2 pybind extention file
 * Create: 2026-01-12
 * Note:
 * History: 2026-01-12 create moe_combine_prefill_a2 pybind extention file
 */

#include <unistd.h>
#include <hccl/hccl.h>
#include <torch/extension.h>
#include <torch/csrc/autograd/custom_function.h>
#include "torch_npu/csrc/core/npu/NPUStream.h"
#include "pytorch_npu_helper.hpp"
#include "torch_bind_exception.h"
#include <hccl/hccl.h>
#include <iostream>

using torch::autograd::AutogradContext;
using torch::autograd::Function;
using tensor_list = std::vector<at::Tensor>;
using namespace at;
using namespace std;

constexpr int MAX_BATCH_SIZE = 4096;
constexpr int X_DIM = 2;

#define MOE_COMBINE_PREFILL_A2_DEF \
    const at::Tensor& x, \
    const at::Tensor& topk_idx, \
    const at::Tensor& topk_weights, \
    const at::Tensor& src_idx, \
    const at::Tensor& send_head, \
    const at::Tensor& expand_scales, \
    const at::Tensor& offset_inner, \
    const at::Tensor& offset_outer, \
    const at::Tensor& count_outer, \
    c10::string_view groupEp, \
    int64_t rank, \
    int64_t num_ranks

#define MOE_COMBINE_PREFILL_A2_CALL \
    x, \
    topk_idx, \
    topk_weights, \
    src_idx, \
    send_head, \
    expand_scales, \
    offset_inner, \
    offset_outer, \
    count_outer, \
    groupEp, \
    rank, \
    num_ranks

at::Tensor MoeCombinePrefillA2ImplNpu(MOE_COMBINE_PREFILL_A2_DEF)
{
    at::Tensor new_topk_idx = topk_idx;
    TORCH_BIND_ASSERT(x.dim() == X_DIM and x.is_contiguous());
    at::Tensor recv_x = x;

    at::Tensor topk_idx_p = topk_idx;

    auto topk_idx_int32 = topk_idx_p.to(at::kInt);
    at::Tensor expert_ids = topk_idx_int32;
    // In the A2 implementation, the tensor is expanded from [bs, k] to [bs, num_expert].
    at::Tensor expand_idx = src_idx;
    // A2 needs global send counts, [num_expert, num_rank]
    at::Tensor ep_send_counts = send_head;
    auto device = x.device();

    const int num_tokens = topk_idx_p.size(0);
    const int num_topk = topk_idx_p.size(1);
    at::Tensor expert_scales = at::empty({1}, at::dtype(at::kFloat).device(x.device()));

    int64_t hidden = static_cast<int>(recv_x.size(1));
    at::Tensor tp_send_counts = at::empty({1}, at::dtype(at::kInt).device(device));
    int64_t tp_world_size = 1;
    int64_t tp_rankId = 0;
    int64_t moe_expert_number = send_head.size(0);
    int64_t global_bs = static_cast<int64_t>(MAX_BATCH_SIZE * num_ranks);

    // ep comm
    const std::string groupEpStr(groupEp.data(), groupEp.size());
    const char* groupEpPtr = groupEpStr.c_str();
    
    // Combine data
    auto combined_x = torch::empty({new_topk_idx.size(0), hidden}, x.options());
    std::optional<torch::Tensor> recv_topk_weights;
    at::Tensor x_active_mask;
    at::Tensor activation_scale;
    at::Tensor weight_scale;
    at::Tensor group_list;
    int64_t expert_shared_type = 0;
    int64_t out_dtype = 0;
    int64_t comm_quant_mode = 0;
    int64_t group_list_type = 0;
    int64_t shared_expert_num = 1;
    int64_t shared_expert_rank_num = 0; // not support shared expert now;
    EXEC_NPU_CMD(aclnnMoeDistributeCombineA2,
        recv_x,
        expert_ids,
        expand_idx,
        ep_send_counts,
        expert_scales,
        tp_send_counts,
        x_active_mask,
        activation_scale,
        weight_scale,
        group_list,
        expand_scales, // a2 new
        offset_inner, // a2 new
        offset_outer, // a2 new
        count_outer, // a2 new
        groupEpPtr,
        num_ranks,
        rank,
        moe_expert_number,
        groupEpPtr,
        tp_world_size,
        tp_rankId,
        expert_shared_type,
        shared_expert_num,
        shared_expert_rank_num,
        global_bs,
        out_dtype,
        comm_quant_mode,
        group_list_type,
        combined_x);
    return combined_x;
}

tensor_list MoeCombinePrefillA2BackwardImplNpu(const at::Tensor &self)
{
    return {at::Tensor(), at::Tensor(), at::Tensor(), at::Tensor()};
}

at::Tensor MoeCombinePrefillA2Impl(MOE_COMBINE_PREFILL_A2_DEF)
{
    static auto op = torch::Dispatcher::singleton()
                        .findSchemaOrThrow("umdk_cam_op_lib::moe_combine_prefill_a2", "")
                        .typed<decltype(MoeCombinePrefillA2Impl)>();
    return op.call(MOE_COMBINE_PREFILL_A2_CALL);
}

class MoeCombinePrefillA2 : public torch::autograd::Function<MoeCombinePrefillA2> {
public:
    static at::Tensor forward(
        AutogradContext *ctx, \
        MOE_COMBINE_PREFILL_A2_DEF)
    {
        at::AutoDispatchBelowADInplaceOrView guard;
        auto result = MoeCombinePrefillA2Impl(MOE_COMBINE_PREFILL_A2_CALL);
        return result;
    }

    static tensor_list backward(
        AutogradContext *ctx, \
        tensor_list grad_outputs)
    {
        return {at::Tensor(), at::Tensor(), at::Tensor(), at::Tensor()};
    }
};

at::Tensor MoeCombinePrefillA2ImplAutograd(MOE_COMBINE_PREFILL_A2_DEF)
{
    auto result = MoeCombinePrefillA2::apply(MOE_COMBINE_PREFILL_A2_CALL);
    return result;
}

// moe_combine_prefill_a2
TORCH_LIBRARY_IMPL(umdk_cam_op_lib, PrivateUse1, m)
{
    m.impl("moe_combine_prefill_a2", &MoeCombinePrefillA2ImplNpu);
    m.impl("moe_combine_prefill_a2_backward", &MoeCombinePrefillA2BackwardImplNpu);
}

TORCH_LIBRARY_IMPL(umdk_cam_op_lib, AutogradPrivateUse1, m)
{
    m.impl("moe_combine_prefill_a2", &MoeCombinePrefillA2ImplAutograd);
}
