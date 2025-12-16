/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: add moe_dispatch_normal pybind extention file
 * Create: 2025-12-10
 * Note:
 * History: 2025-12-10 create moe_dispatch_normal pybind extention file
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

std::tuple<at::Tensor, at::Tensor, at::Tensor> moe_dispatch_normal_impl_npu(
    const at::Tensor &x, \
    const at::Tensor &topkIdx, \
    const at::Tensor &sendOffset, \
    const at::Tensor &sendTokenIdx, \
    const at::Tensor &recvOffset, \
    const at::Tensor &recvCount, \
    c10::string_view groupEp, \
    int64_t epWorldSize, \
    int64_t epRankId, \
    c10::string_view groupTp, \
    int64_t tpWorldSize, \
    int64_t tpRankId, \
    int64_t moeExpertNum, \
    int64_t quantMode, \
    int64_t globalBs
)
{
    // 必须要求对齐fused_deep_moe.cpp 先input 跟着 attr， 然后output
    vector<char> groupEpChrs(groupEp.begin(), groupEp.end());
    groupEpChrs.push_back('\0');
    char *groupEpPtr = &groupEpChrs[0];
    vector<char> groupTpChrs(groupTp.begin(), groupTp.end());
    groupTpChrs.push_back('\0');
    char *groupTpPtr = &groupTpChrs[0];

    auto recvCountCpu = recvCount.to(at::kCPU);
    auto recvCountPtr = recvCountCpu.data_ptr<int>();
    auto hidden = static_cast<int>(x.size(1));
    int64_t totalRecvTokens = recvCountPtr[moeExpertNum - 1];
    int totalCnt = totalRecvTokens == 0 ? 1 : totalRecvTokens;
    auto expandxOut = at::zeros({totalCnt, hidden}, x.options());
    auto dynamicScalesOut = at::zeros({totalCnt}, at::dtype(at::kFloat).device(x.device()));
    auto expandIdxOut = at::zeros({totalCnt * KERNEL_PARAM_CNT}, at::dtype(at::kInt).device(x.device()));

    EXEC_NPU_CMD(aclnnMoeDispatchNormal,
        // input
        x, topkIdx, sendOffset, sendTokenIdx, recvOffset, recvCount, \
        // attr
        groupEpPtr, epWorldSize, epRankId, groupTpPtr, tpWorldSize, tpRankId, moeExpertNum, quantMode, globalBs, \
        // output
        expandxOut, dynamicScalesOut, expandIdxOut);
    return std::make_tuple(expandxOut, dynamicScalesOut, expandIdxOut);
}

tensor_list moe_dispatch_normal_backward_impl_npu(const at::Tensor &self)
{
    at::Tensor result = at::Tensor(self); // 创建输出内存
    return {result, result, result};
}

std::tuple<at::Tensor, at::Tensor, at::Tensor> moe_dispatch_normal_impl_meta(
    const at::Tensor &x, \
    const at::Tensor &topkIdx, \
    const at::Tensor &sendOffset, \
    const at::Tensor &sendTokenIdx, \
    const at::Tensor &recvOffset, \
    const at::Tensor &recvCount, \
    c10::string_view groupEp, \
    int64_t epWorldSize, \
    int64_t epRankId, \
    c10::string_view groupTp, \
    int64_t tpWorldSize, \
    int64_t tpRankId, \
    int64_t moeExpertNum, \
    int64_t quantMode, \
    int64_t globalBs)
{
    // 必须要求对齐fused_deep_moe.cpp 先input 跟着 attr， 然后output
    vector<char> groupEpChrs(groupEp.begin(), groupEp.end());
    groupEpChrs.push_back('\0');
    char *groupEpPtr = &groupEpChrs[0];
    vector<char> groupTpChrs(groupTp.begin(), groupTp.end());
    groupTpChrs.push_back('\0');
    char *groupTpPtr = &groupTpChrs[0];

    auto recvCountCpu = recvCount.to(at::kCPU);
    auto recvCountPtr = recvCountCpu.data_ptr<int>();
    auto hidden = static_cast<int>(x.size(1));
    int64_t totalRecvTokens = recvCountPtr[moeExpertNum - 1];
    int totalCnt = totalRecvTokens == 0 ? 1 : totalRecvTokens;
    auto expandxOut = at::zeros({totalCnt, hidden}, x.options());
    auto dynamicScalesOut = at::zeros({totalCnt}, at::dtype(at::kFloat).device(x.device()));
    auto expandIdxOut = at::zeros({totalCnt * KERNEL_PARAM_CNT}, at::dtype(at::kInt).device(x.device()));

    return std::make_tuple(expandxOut, dynamicScalesOut, expandIdxOut);
}

std::tuple<at::Tensor, at::Tensor, at::Tensor> moe_dispatch_normal_impl(
    const at::Tensor &x, \
    const at::Tensor &topkIdx, \
    const at::Tensor &sendOffset, \
    const at::Tensor &sendTokenIdx, \
    const at::Tensor &recvOffset, \
    const at::Tensor &recvCount, \
    c10::string_view groupEp, \
    int64_t epWorldSize, \
    int64_t epRankId, \
    c10::string_view groupTp, \
    int64_t tpWorldSize, \
    int64_t tpRankId, \
    int64_t moeExpertNum, \
    int64_t quantMode, \
    int64_t globalBs)
{
    static auto op = torch::Dispatcher::singleton()
                        .findSchemaOrThrow("umdk_cam_op_lib::moe_dispatch_normal", "")
                        .typed<decltype(moe_dispatch_normal_impl)>();
    return op.call(x, topkIdx, sendOffset, sendTokenIdx, recvOffset, recvCount, groupEp, epWorldSize, epRankId, \
        groupTp, tpWorldSize, tpRankId, moeExpertNum, quantMode, globalBs);
}

// 通过继承torch::autograd::Function类实现前反向绑定
class ExtMoeDispatchNormal : public torch::autograd::Function<ExtMoeDispatchNormal> {
public:
    static tensor_list forward(
        AutogradContext *ctx, \
        const at::Tensor &x, \
        const at::Tensor &topkIdx, \
        const at::Tensor &sendOffset, \
        const at::Tensor &sendTokenIdx, \
        const at::Tensor &recvOffset, \
        const at::Tensor &recvCount, \
        c10::string_view groupEp, \
        int64_t epWorldSize, \
        int64_t epRankId, \
        c10::string_view groupTp, \
        int64_t tpWorldSize, \
        int64_t tpRankId, \
        int64_t moeExpertNum, \
        int64_t quantMode, \
        int64_t globalBs)
    {
        at::AutoDispatchBelowADInplaceOrView guard;
        auto result = moe_dispatch_normal_impl(x, topkIdx, sendOffset, sendTokenIdx, recvOffset, \
            recvCount, groupEp, epWorldSize, epRankId, \
            groupTp, tpWorldSize, tpRankId, moeExpertNum, quantMode, globalBs);

        return {std::get<0>(result), std::get<1>(result), std::get<2>(result)};
    }

    static tensor_list backward(
        AutogradContext *ctx, \
        tensor_list grad_outputs)
    {
        return {at::Tensor(), at::Tensor(), at::Tensor()};
    }
};

std::tuple<at::Tensor, at::Tensor, at::Tensor> moe_dispatch_normal_impl_autograd(
    const at::Tensor &x, \
    const at::Tensor &topkIdx, \
    const at::Tensor &sendOffset, \
    const at::Tensor &sendTokenIdx, \
    const at::Tensor &recvOffset, \
    const at::Tensor &recvCount, \
    c10::string_view groupEp, \
    int64_t epWorldSize, \
    int64_t epRankId, \
    c10::string_view groupTp, \
    int64_t tpWorldSize, \
    int64_t tpRankId, \
    int64_t moeExpertNum, \
    int64_t quantMode, \
    int64_t globalBs)
{
    auto result = ExtMoeDispatchNormal::apply(x, topkIdx, sendOffset, sendTokenIdx, recvOffset, \
        recvCount, groupEp, epWorldSize, epRankId, \
        groupTp, tpWorldSize, tpRankId, moeExpertNum, quantMode, globalBs);
    return std::make_tuple(result[0], result[1], result[2]);
}

// moe_dispatch_normal
TORCH_LIBRARY_IMPL(umdk_cam_op_lib, PrivateUse1, m)
{
    m.impl("moe_dispatch_normal", &moe_dispatch_normal_impl_npu);
    m.impl("moe_dispatch_normal_backward", &moe_dispatch_normal_backward_impl_npu);
}

TORCH_LIBRARY_IMPL(umdk_cam_op_lib, AutogradPrivateUse1, m)
{
    m.impl("moe_dispatch_normal", &moe_dispatch_normal_impl_autograd);
}

// 为Meta设备注册前反向实现
TORCH_LIBRARY_IMPL(umdk_cam_op_lib, Meta, m)
{
    m.impl("moe_dispatch_normal", &moe_dispatch_normal_impl_meta);
}