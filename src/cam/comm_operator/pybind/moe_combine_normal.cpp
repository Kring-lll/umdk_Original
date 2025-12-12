/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: add moe_combine_normal pybind extention file
 * Create: 2025-12-10
 * Note:
 * History: 2025-12-10 create moe_combine_normal pybind extention file
 */

#include <unistd.h>
#include <hccl/hccl.h>
#include <torch/extension.h>
#include <torch/csrc/autograd/custom_function.h>
#include "torch_npu/csrc/core/npu/NPUStream.h"
#include "pytorch_npu_helper.hpp"
#include <hccl/hccl.h>
#include <iostream>

using torch::autograd::AutogradContext;
using torch::autograd::Function;
using tensor_list = std::vector<at::Tensor>;
using namespace at;
using namespace std;

constexpr int KERNEL_PARAM_CNT = 3;

at::Tensor moe_combine_normal_impl_npu(
    const at::Tensor &recvX, \
    const at::Tensor &tokenSrcInfo, \
    const at::Tensor &epRecvCounts, \
    const at::Tensor &recvTopkWeights, \
    const c10::optional<at::Tensor> &tpRecvCounts, \
    c10::string_view epGroupName, \
    int64_t epWorldSize, \
    int64_t epRankId, \
    c10::string_view tpGroupName, \
    int64_t tpWorldSize, \
    int64_t tpRankId, \
    int64_t moeExpertNum, \
    int64_t globalBs)
{
    std::vector<at::Tensor> weightBlocks;
    if (recvTopkWeights.size(0) != 0) {
        weightBlocks.emplace_back(recvTopkWeights);
    }
    at::Tensor expertScales = torch::cat(weightBlocks, 0);

    // Combine data
    auto combinedX = torch::empty({expertScales.size(0), recvX.size(1)}, recvX.options());

    EXEC_NPU_CMD(aclnnMoeCombineNormal,
        // input
        recvX, tokenSrcInfo, epRecvCounts, recvTopkWeights, tpRecvCounts, \
        // attr
        epGroupName, epWorldSize, epRankId, tpGroupName, tpWorldSize, tpRankId, moeExpertNum, globalBs, \
        // output
        combinedX);
    return combinedX;
}

tensor_list moe_combine_normal_backward_impl_npu(const at::Tensor &self)
{
    return {at::Tensor(), at::Tensor(), at::Tensor()};
}

at::Tensor moe_combine_normal_impl_meta(
    const at::Tensor &recvX, \
    const at::Tensor &tokenSrcInfo, \
    const at::Tensor &epRecvCounts, \
    const at::Tensor &recvTopkWeights, \
    const c10::optional<at::Tensor> &tpRecvCounts, \
    c10::string_view epGroupName, \
    int64_t epWorldSize, \
    int64_t epRankId, \
    c10::string_view tpGroupName, \
    int64_t tpWorldSize, \
    int64_t tpRankId, \
    int64_t moeExpertNum, \
    int64_t globalBs)
{
    std::vector<at::Tensor> weightBlocks;
    if (recvTopkWeights.size(0) != 0) {
        weightBlocks.emplace_back(recvTopkWeights);
    }
    at::Tensor expertScales = torch::cat(weightBlocks, 0);

    // Combine data
    auto combinedX = torch::empty({expertScales.size(0), recvX.size(1)}, recvX.options());

    return combinedX;
}

at::Tensor moe_combine_normal_impl(
    const at::Tensor &recvX, \
    const at::Tensor &tokenSrcInfo, \
    const at::Tensor &epRecvCounts, \
    const at::Tensor &recvTopkWeights, \
    const c10::optional<at::Tensor> &tpRecvCounts, \
    c10::string_view epGroupName, \
    int64_t epWorldSize, \
    int64_t epRankId, \
    c10::string_view tpGroupName, \
    int64_t tpWorldSize, \
    int64_t tpRankId, \
    int64_t moeExpertNum, \
    int64_t globalBs)
{
    static auto op = torch::Dispatcher::singleton()
                        .findSchemaOrThrow("umdk_cam_op_lib::moe_combine_normal", "")
                        .typed<decltype(moe_combine_normal_impl)>();
    return op.call(recvX, tokenSrcInfo, epRecvCounts, recvTopkWeights, tpRecvCounts, \
        epGroupName, epWorldSize, epRankId, tpGroupName, tpWorldSize, tpRankId, moeExpertNum, globalBs);
}

// 通过继承torch::autograd::Function类实现前反向绑定
class ExtMoeCombineNormal : public torch::autograd::Function<ExtMoeCombineNormal> {
public:
    static at::Tensor forward(
        AutogradContext *ctx, \
        const at::Tensor &recvX, \
        const at::Tensor &tokenSrcInfo, \
        const at::Tensor &epRecvCounts, \
        const at::Tensor &recvTopkWeights, \
        const c10::optional<at::Tensor> &tpRecvCounts, \
        c10::string_view epGroupName, \
        int64_t epWorldSize, \
        int64_t epRankId, \
        c10::string_view tpGroupName, \
        int64_t tpWorldSize, \
        int64_t tpRankId, \
        int64_t moeExpertNum, \
        int64_t globalBs)
    {
        at::AutoDispatchBelowADInplaceOrView guard;
        auto result = moe_combine_normal_impl(recvX, tokenSrcInfo, epRecvCounts, recvTopkWeights, tpRecvCounts, \
        epGroupName, epWorldSize, epRankId, tpGroupName, tpWorldSize, tpRankId, moeExpertNum, globalBs);
        return result;
    }

    static tensor_list backward(
        AutogradContext *ctx, \
        tensor_list grad_outputs)
    {
        return {at::Tensor(), at::Tensor(), at::Tensor()};
    }
};

at::Tensor moe_combine_normal_impl_autograd(
    const at::Tensor &recvX, \
    const at::Tensor &tokenSrcInfo, \
    const at::Tensor &epRecvCounts, \
    const at::Tensor &recvTopkWeights, \
    const c10::optional<at::Tensor> &tpRecvCounts, \
    c10::string_view epGroupName, \
    int64_t epWorldSize, \
    int64_t epRankId, \
    c10::string_view tpGroupName, \
    int64_t tpWorldSize, \
    int64_t tpRankId, \
    int64_t moeExpertNum, \
    int64_t globalBs)
{
    auto result = ExtMoeCombineNormal::apply(recvX, tokenSrcInfo, epRecvCounts, recvTopkWeights, tpRecvCounts, \
        epGroupName, epWorldSize, epRankId, tpGroupName, tpWorldSize, tpRankId, moeExpertNum, globalBs);
    return result;
}

// moe_dispatch_normal
TORCH_LIBRARY_IMPL(umdk_cam_op_lib, PrivateUse1, m)
{
    m.impl("moe_combine_normal", &moe_combine_normal_impl_npu);
    m.impl("moe_combine_normal_backward", &moe_combine_normal_backward_impl_npu);
}

TORCH_LIBRARY_IMPL(umdk_cam_op_lib, AutogradPrivateUse1, m)
{
    m.impl("moe_combine_normal", &moe_combine_normal_impl_autograd);
}

// 为Meta设备注册前反向实现
TORCH_LIBRARY_IMPL(umdk_cam_op_lib, Meta, m)
{
    m.impl("moe_combine_normal", &moe_combine_normal_impl_meta);
}