

/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: add moe_dispatch_prefill_a2 pybind extention file
 * Create: 2026-01-12
 * Note:
 * History: 2026-01-12 create moe_dispatch_prefill_a2 pybind extention file
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
#include <iomanip>
#include <algorithm>
#include <cstdint>

using torch::autograd::AutogradContext;
using torch::autograd::Function;
using tensor_list = std::vector<at::Tensor>;
using namespace at;
using namespace std;

constexpr int MAX_BATCH_SIZE = 4096;
constexpr int EXPERT_DATA_SIZE = 1 + MAX_BATCH_SIZE;  // 4097
constexpr int LOCAL_RANK_SIZE = 8;
constexpr int A2_MAX_HCCS_PEERS = 8;
constexpr int DYNAMIC_SCALES = 2;
constexpr int NO_SCALES = 0;
constexpr int X_DIM = 2;

#define MOE_DISPATCH_PREFILL_A2_DEF \
    const at::Tensor& x, \
    const at::Tensor& topk_idx, \
    const at::Tensor& topk_weights, \
    const at::Tensor& num_tokens_per_expert, \
    const at::Tensor& notify_send_data, \
    c10::string_view groupEp, \
    int64_t rank, \
    int64_t num_ranks, \
    bool use_quant

#define MOE_DISPATCH_PREFILL_A2_CALL \
    x, \
    topk_idx, \
    topk_weights, \
    num_tokens_per_expert, \
    notify_send_data, \
    groupEp, \
    rank, \
    num_ranks, \
    use_quant

tensor_list MoeDispatchPrefillA2ImplNpu(
    MOE_DISPATCH_PREFILL_A2_DEF)
{
    at::Tensor new_topk_idx = topk_idx;

    at::Tensor new_x = x;

    // Type checks
    TORCH_BIND_ASSERT(num_tokens_per_expert.scalar_type() == at::kInt);

    // Shape and contiguous checks
    TORCH_BIND_ASSERT(new_x.dim() == X_DIM and new_x.is_contiguous());
    TORCH_BIND_ASSERT(num_tokens_per_expert.dim() == 1 and num_tokens_per_expert.is_contiguous());
    TORCH_BIND_ASSERT(num_tokens_per_expert.size(0) % num_ranks == 0);

    auto num_tokens = static_cast<int>(new_x.size(0));
    auto hidden = static_cast<int>(new_x.size(1));
    auto num_experts = static_cast<int64_t>(num_tokens_per_expert.size(0));
    auto num_local_experts = static_cast<int>(num_experts / num_ranks);

    // Top-k checks
    int num_topk = static_cast<int>(topk_idx.size(1));

    auto device = x.device();
    at::Tensor new_topk_weights;

    new_topk_weights = topk_weights;

    // FP8 scales checks
    std::optional<at::Tensor> x_scales;
    float *x_scales_ptr = nullptr;
    int num_scales = 0;
    int scale_token_stride = 0;
    int scale_hidden_stride = 0;
    if (x_scales.has_value()) {
        TORCH_BIND_ASSERT(new_x.element_size() == 1);
        TORCH_BIND_ASSERT(x_scales->scalar_type() == at::kFloat or x_scales->scalar_type() == at::kInt);
        TORCH_BIND_ASSERT(x_scales->dim() == X_DIM);
        TORCH_BIND_ASSERT(x_scales->size(0) == num_tokens);
        num_scales = x_scales->dim() == 1 ? 1 : static_cast<int>(x_scales->size(1));
        x_scales_ptr = static_cast<float *>(x_scales->data_ptr());
        scale_token_stride = static_cast<int>(x_scales->stride(0));
        scale_hidden_stride = static_cast<int>(x_scales->stride(1));
    }

    // dispatch normal param
    int64_t tp_size = 1;
    int64_t tp_rank = 0;
    int64_t expertShardType = 0;
    int64_t sharedExpertNum = 1;
    int64_t sharedExpertRankNum = 0;
    int64_t expertTokenNumsType = 0;

    int64_t quant_mode = use_quant ? DYNAMIC_SCALES : NO_SCALES;
    int64_t global_bs = static_cast<int64_t>(MAX_BATCH_SIZE * num_ranks);
    at::Tensor expert_ids = new_topk_idx.to(at::kInt);
    at::Tensor xActiveMask = at::empty({1}, at::dtype(at::kInt).device(x.device()));

    auto expertTokenNums = at::zeros({1}, at::dtype(at::kLong).device(x.device()));
    auto epRecvCount = at::zeros({1}, at::dtype(at::kInt).device(x.device()));
    auto tpRecvCount = at::zeros({1}, at::dtype(at::kInt).device(x.device()));
    at::Tensor dispatch_wait_recv_cost_stats_out;
    auto recv_topk_idx = std::optional<at::Tensor>();
    auto recv_topk_weights = std::optional<at::Tensor>();

    int64_t local_rank_size = A2_MAX_HCCS_PEERS;
    int32_t server_num = num_ranks / local_rank_size;
    int64_t local_rank_id = rank % local_rank_size;
    auto new_num_tokens_per_expert = num_tokens_per_expert;
    std::vector<int> num_recv_tokens_per_expert_list;
    // indicates the value type of the output num_recv_tokens_per_expert_list, with a range of [0, 1]
    // 0 means the prefix sum of the number of tokens received by each expert;
    // 1 means the number of tokens received by each expert (default)
    int expert_token_nums_type = 1; // not support prefix sum now
    TORCH_BIND_ASSERT(expert_token_nums_type == 1 or expert_token_nums_type == 0);

    // Corresponding to the output data and length of the layout
    auto new_send_data = notify_send_data;
    const int notify_send_data_size =
        num_experts * EXPERT_DATA_SIZE + server_num + MAX_BATCH_SIZE * (1 + 2 * server_num + num_experts);
    int send_count = notify_send_data_size;

    auto send_data_offset = at::empty({num_experts}, at::dtype(at::kInt).device(x.device()));
    at::Tensor tmp_data =
        at::empty({send_count * num_ranks}, at::dtype(at::kInt).device(x.device()));  // 给notify算子用来临时存数的空间
    at::Tensor recv_data = at::empty({send_count * num_ranks}, at::dtype(at::kInt).device(x.device()));
    at::Tensor token_server_idx =
        at::empty({MAX_BATCH_SIZE, server_num}, at::dtype(at::kInt).device(x.device()));  // offset_outer
    at::Tensor token_unique_per_server = at::empty({server_num}, at::dtype(at::kInt).device(x.device()));
    at::Tensor ep_rank_token_cnt =
        at::empty({num_experts, num_ranks}, at::dtype(at::kInt).device(x.device()));  // 包含全局的
    // The number of tokens received by each expert on this rank, not a prefix sum
    at::Tensor recv_tokens_per_expert = at::empty({num_local_experts}, at::dtype(at::kLong).device(x.device()));
    at::Tensor src_offset_rank_token_idx =
        at::empty({num_experts, num_ranks, MAX_BATCH_SIZE}, at::dtype(at::kInt).device(x.device()));
    at::Tensor dst_offset_rank_token_idx =
        at::empty({num_experts, num_ranks, MAX_BATCH_SIZE}, at::dtype(at::kInt).device(x.device()));
    // The offsetInner for the current rank and the peer rank
    at::Tensor offset_inner = at::empty({2, MAX_BATCH_SIZE, num_experts}, at::dtype(at::kInt).device(x.device()));
    at::Tensor count_outer = at::empty({MAX_BATCH_SIZE}, at::dtype(at::kInt).device(x.device()));
    at::Tensor expand_idx = at::empty({MAX_BATCH_SIZE, num_experts}, at::dtype(at::kInt).device(x.device()));
    at::Tensor total_recv_token = torch::empty({1}, at::dtype(at::kInt).device(x.device()));

    // get ep name
    const std::string groupEpStr(groupEp.data(), groupEp.size());
    const char* groupEpPtr = groupEpStr.c_str();

    EXEC_NPU_CMD(aclnnNotifyDispatchA2, new_send_data, new_num_tokens_per_expert, tmp_data,
                 send_count, num_tokens,
                 num_topk, num_experts,
                 groupEpPtr,  // commGroup
                 num_ranks,     // rankSize
                 rank,          // rankId
                 local_rank_size, local_rank_id,
                 send_data_offset,  // A2 not use
                 recv_data, token_server_idx, token_unique_per_server, ep_rank_token_cnt, recv_tokens_per_expert,
                 src_offset_rank_token_idx, dst_offset_rank_token_idx, offset_inner, count_outer, expand_idx,
                 total_recv_token);
    int total_count = total_recv_token.item<int>();
    int num_recv_tokens = (total_count == 0) ? 1 : total_count;
    auto expandx_out = use_quant ? at::empty({num_recv_tokens, hidden}, at::dtype(at::kChar).device(x.device()))
                                 : at::empty({num_recv_tokens, hidden}, x.options());
    auto dynamic_scales_out = at::empty({num_recv_tokens}, at::dtype(at::kFloat).device(x.device()));
    auto expand_scales = at::empty({num_recv_tokens}, at::dtype(at::kFloat).device(x.device()));

    EXEC_NPU_CMD(aclnnDispatchNormalA2, new_x, expert_ids, x_scales, xActiveMask, new_topk_weights, token_server_idx,
                 token_unique_per_server, ep_rank_token_cnt, src_offset_rank_token_idx, dst_offset_rank_token_idx,
                 groupEpPtr, num_ranks, rank, num_experts, groupEpPtr, tp_size, tp_rank, expertShardType,
                 sharedExpertNum, sharedExpertRankNum, quant_mode, global_bs, expertTokenNumsType, expandx_out,
                 dynamic_scales_out, expand_idx, expertTokenNums, epRecvCount, expand_scales,
                 dispatch_wait_recv_cost_stats_out);
    auto recv_token_per_exp_cpu = recv_tokens_per_expert.to(at::kCPU);
    auto recv_token_per_exp_ptr = recv_token_per_exp_cpu.data_ptr<int64_t>();

    tensor_list result = {
        expandx_out,
        dynamic_scales_out,
        expand_idx,
        ep_rank_token_cnt,
        offset_inner,
        token_server_idx,  // this is offset_outer
        count_outer,
        expand_scales
        };
    return result;
}

tensor_list MoeDispatchPrefillA2BackwardImplNpu(const at::Tensor &self)
{
    at::Tensor result = at::Tensor(self);
    return {result, result, result, result};
}

tensor_list MoeDispatchPrefillA2Impl(MOE_DISPATCH_PREFILL_A2_DEF)
{
    static auto op = torch::Dispatcher::singleton()
                        .findSchemaOrThrow("umdk_cam_op_lib::moe_dispatch_prefill_a2", "")
                        .typed<decltype(MoeDispatchPrefillA2Impl)>();
    return op.call(MOE_DISPATCH_PREFILL_A2_CALL);
}

class MoeDispatchPrefillA2 : public torch::autograd::Function<MoeDispatchPrefillA2> {
public:
    static tensor_list forward(
        AutogradContext *ctx, \
        MOE_DISPATCH_PREFILL_A2_DEF)
    {
        at::AutoDispatchBelowADInplaceOrView guard;
        auto result = MoeDispatchPrefillA2Impl(MOE_DISPATCH_PREFILL_A2_CALL);
        return result;
    }

    static tensor_list backward(
        AutogradContext *ctx, \
        tensor_list grad_outputs)
    {
        return {at::Tensor(), at::Tensor(), at::Tensor(), at::Tensor()};
    }
};

tensor_list MoeDispatchPrefillA2ImplAutograd(
    MOE_DISPATCH_PREFILL_A2_DEF)
{
    auto result = MoeDispatchPrefillA2::apply(MOE_DISPATCH_PREFILL_A2_CALL);
    return result;
}

// moe_dispatch_prefill_a2
TORCH_LIBRARY_IMPL(umdk_cam_op_lib, PrivateUse1, m)
{
    m.impl("moe_dispatch_prefill_a2", &MoeDispatchPrefillA2ImplNpu);
    m.impl("moe_dispatch_prefill_a2_backward", &MoeDispatchPrefillA2BackwardImplNpu);
}

TORCH_LIBRARY_IMPL(umdk_cam_op_lib, AutogradPrivateUse1, m)
{
    m.impl("moe_dispatch_prefill_a2", &MoeDispatchPrefillA2ImplAutograd);
}
