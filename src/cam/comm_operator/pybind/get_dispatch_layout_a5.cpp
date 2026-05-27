/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: add get_dispatch_layout_a5 pybind extention file
 * Create: 2026-05-25
 * Note:
 * History: 2026-05-25 create get_dispatch_layout_a5 pybind extention file (multi-mesh layered support)
 */

#include "pytorch_npu_helper.hpp"
#include "torch_npu/csrc/core/npu/NPUStream.h"
#include "torch_bind_exception.h"
#include <hccl/hccl.h>
#include <iostream>
#include <torch/csrc/autograd/custom_function.h>
#include <torch/extension.h>
#include <unistd.h>

using torch::autograd::AutogradContext;
using torch::autograd::Function;
using TensorVector = std::vector<at::Tensor>;
using namespace at;
using namespace std;

namespace {
const int LOCAL_MESH_SIZE = 4;
const int MAX_BATCH_SIZE = 4096;
const int EXPERT_DATA_SIZE = 1 + MAX_BATCH_SIZE; // 4097
const uint32_t DIM_TWO = 2;
const uint32_t ZERO = 0;
const uint32_t FIRST = 1;
constexpr int A5_TOPK_MIN = 2;
constexpr int A5_TOPK_MAX = 8;
} // namespace

// dispatch requires return: num_tokens_per_expert, notify_send_data
std::tuple<at::Tensor, at::Tensor> GetDispatchLayoutA5ImplNpu(const at::Tensor &topkIdx, int64_t numExperts,
    int64_t numRanks)
{
    // topk_idx dtype must be int or long
    TORCH_BIND_ASSERT(topkIdx.scalar_type() == at::kInt || topkIdx.scalar_type() == at::kLong);

    // Convert topk_idx to int64 if necessary
    at::Tensor topkIdxInt64 = topkIdx.scalar_type() == at::kLong ? topkIdx : topkIdx.to(at::kLong);

    TORCH_BIND_ASSERT(topkIdxInt64.dim() == DIM_TWO);
    TORCH_BIND_ASSERT(topkIdxInt64.is_contiguous());
    TORCH_BIND_ASSERT(numExperts > 0);

    const int numTokens = topkIdxInt64.size(0);
    const int numTopk = topkIdxInt64.size(1);
    TORCH_BIND_ASSERT(numTopk >= A5_TOPK_MIN && numTopk <= A5_TOPK_MAX);

    // numRanks must be greater than local mesh size and divisible by it
    TORCH_BIND_ASSERT(numRanks > LOCAL_MESH_SIZE);
    TORCH_BIND_ASSERT(numRanks % LOCAL_MESH_SIZE == 0);
    const int localMeshSize = LOCAL_MESH_SIZE;
    auto meshNum = numRanks / localMeshSize;

    auto device = topkIdxInt64.device();
    auto numTokensPerExpert = at::zeros({numExperts}, at::dtype(at::kInt).device(device));
    auto numTokensPerRank = at::zeros({numRanks}, at::dtype(at::kInt).device(device));
    auto isTokenInRank = at::zeros({numTokens, numRanks}, at::dtype(at::kInt).device(device));
    TORCH_BIND_ASSERT(numTokens <= MAX_BATCH_SIZE);
    const int notifySendDataSize =
        numExperts * EXPERT_DATA_SIZE + meshNum + MAX_BATCH_SIZE * (1 + 2 * meshNum + numExperts);
    auto sendTokenIdxSmall = at::zeros({numTokens, numTopk}, at::dtype(at::kInt).device(device));
    auto notifySendData = at::zeros({notifySendDataSize}, at::dtype(at::kInt).device(device));
    EXEC_NPU_CMD(aclnnDispatchLayout, topkIdxInt64, numTokens, numRanks, numExperts, numTopk, localMeshSize,
                 numTokensPerRank, numTokensPerExpert, isTokenInRank, notifySendData, sendTokenIdxSmall);
    return std::make_tuple(numTokensPerExpert, notifySendData);
}

TensorVector GetDispatchLayoutA5BackwardImplNpu(const at::Tensor &self)
{
    at::Tensor result = at::Tensor(self);
    return {result, result};
}

std::tuple<at::Tensor, at::Tensor> GetDispatchLayoutA5Impl(const at::Tensor &topkIdx, int64_t numExperts,
    int64_t numRanks)
{
    static auto op = torch::Dispatcher::singleton()
                         .findSchemaOrThrow("umdk_cam_op_lib::get_dispatch_layout_a5", "")
                         .typed<decltype(GetDispatchLayoutA5Impl)>();
    return op.call(topkIdx, numExperts, numRanks);
}

class GetDispatchLayoutA5 : public torch::autograd::Function<GetDispatchLayoutA5> {
public:
    static TensorVector forward(AutogradContext *ctx, const at::Tensor &topkIdx, int64_t numExperts, int64_t numRanks)
    {
        at::AutoDispatchBelowADInplaceOrView guard;
        auto result = GetDispatchLayoutA5Impl(topkIdx, numExperts, numRanks);

        return {std::get<0>(result), std::get<1>(result)};
    }

    static TensorVector backward(AutogradContext *ctx, TensorVector gradOutputs)
    {
        return {at::Tensor(), at::Tensor()};
    }
};

std::tuple<at::Tensor, at::Tensor> GetDispatchLayoutA5ImplAutograd(const at::Tensor &topkIdx, int64_t numExperts,
    int64_t numRanks)
{
    auto result = GetDispatchLayoutA5::apply(topkIdx, numExperts, numRanks);
    return std::make_tuple(result[ZERO], result[FIRST]);
}

// get_dispatch_layout_a5
TORCH_LIBRARY_IMPL(umdk_cam_op_lib, PrivateUse1, m)
{
    m.impl("get_dispatch_layout_a5", &GetDispatchLayoutA5ImplNpu);
    m.impl("get_dispatch_layout_a5_backward", &GetDispatchLayoutA5BackwardImplNpu);
}

TORCH_LIBRARY_IMPL(umdk_cam_op_lib, AutogradPrivateUse1, m)
{
    m.impl("get_dispatch_layout_a5", &GetDispatchLayoutA5ImplAutograd);
}
