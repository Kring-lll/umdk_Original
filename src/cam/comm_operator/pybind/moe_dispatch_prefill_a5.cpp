/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: add moe_dispatch_prefill_a5 pybind extention file
 * Create: 2026-05-27
 * Note:
 * History: 2026-05-27 create moe_dispatch_prefill_a5 pybind extention file (multi-mesh layered support)
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
using tensorList = std::vector<at::Tensor>;
using namespace at;
using namespace std;

namespace {
constexpr int MESH_RANK_SIZE = 4;
constexpr int MAX_BATCH_SIZE = 4096;
constexpr int EXPERT_DATA_SIZE = 1 + MAX_BATCH_SIZE;  // 4097
constexpr int DYNAMIC_SCALES = 2;
constexpr int NO_SCALES = 0;
constexpr int X_DIM = 2;
constexpr int A5_TOPK_MIN = 2;
constexpr int A5_TOPK_MAX = 8;
} // namespace

#define MOE_DISPATCH_PREFILL_A5_DEF \
    const at::Tensor& x, \
    const at::Tensor& topkIdx, \
    const at::Tensor& topkWeights, \
    const at::Tensor& numTokensPerExpert, \
    const at::Tensor& notifySendData, \
    c10::string_view groupEp, \
    int64_t rank, \
    int64_t numRanks, \
    bool useQuant

#define MOE_DISPATCH_PREFILL_A5_CALL \
    x, \
    topkIdx, \
    topkWeights, \
    numTokensPerExpert, \
    notifySendData, \
    groupEp, \
    rank, \
    numRanks, \
    useQuant

tensorList MoeDispatchPrefillA5ImplNpu(
    MOE_DISPATCH_PREFILL_A5_DEF)
{
    at::Tensor newTopkIdx = topkIdx;

    at::Tensor newX = x;

    // Type checks
    TORCH_BIND_ASSERT(numTokensPerExpert.scalar_type() == at::kInt);
    // topkIdx dtype must be int or long
    TORCH_BIND_ASSERT(topkIdx.scalar_type() == at::kInt || topkIdx.scalar_type() == at::kLong);

    // numRanks must be greater than MESH_RANK_SIZE and divisible by it
    TORCH_BIND_ASSERT(numRanks > MESH_RANK_SIZE);
    TORCH_BIND_ASSERT(numRanks % MESH_RANK_SIZE == 0);

    // Shape and contiguous checks
    TORCH_BIND_ASSERT(newX.dim() == X_DIM and newX.is_contiguous());
    TORCH_BIND_ASSERT(numTokensPerExpert.dim() == 1 and numTokensPerExpert.is_contiguous());
    TORCH_BIND_ASSERT(numTokensPerExpert.size(0) % numRanks == 0);

    auto numTokens = static_cast<int>(newX.size(0));
    TORCH_BIND_ASSERT(numTokens <= MAX_BATCH_SIZE);
    auto hidden = static_cast<int>(newX.size(1));
    auto numExperts = static_cast<int64_t>(numTokensPerExpert.size(0));
    auto numLocalExperts = static_cast<int>(numExperts / numRanks);

    // Top-k checks
    int numTopk = static_cast<int>(topkIdx.size(1));
    TORCH_BIND_ASSERT(numTopk >= A5_TOPK_MIN && numTopk <= A5_TOPK_MAX);

    // topkWeights dtype must be float (not bf16)
    TORCH_BIND_ASSERT(topkWeights.scalar_type() == at::kFloat);

    auto device = x.device();
    at::Tensor newTopkWeights;

    newTopkWeights = topkWeights;

    // FP8 scales
    at::Tensor xScales;

    // dispatch normal param
    int64_t tpSize = 1;
    int64_t tpRank = 0;
    int64_t expertShardType = 0;
    int64_t sharedExpertNum = 1;
    int64_t sharedExpertRankNum = 0;
    int64_t expertTokenNumsType = 0;

    int64_t quantMode = useQuant ? DYNAMIC_SCALES : NO_SCALES;
    int64_t globalBs = static_cast<int64_t>(MAX_BATCH_SIZE * numRanks);
    at::Tensor expertIds = newTopkIdx.to(at::kInt);
    at::Tensor xActiveMask = at::empty({1}, at::dtype(at::kInt).device(x.device()));

    auto expertTokenNums = at::zeros({1}, at::dtype(at::kLong).device(x.device()));
    auto epRecvCount = at::zeros({1}, at::dtype(at::kInt).device(x.device()));
    at::Tensor dispatchWaitRecvCostStatsOut;

    int64_t localRankSize = MESH_RANK_SIZE;
    int32_t meshNum = numRanks / localRankSize;
    int64_t localRankId = rank % localRankSize;
    auto newNumTokensPerExpert = numTokensPerExpert;

    // Corresponding to the output data and length of the layout
    auto newSendData = notifySendData;
    const int notifySendDataSize =
        numExperts * EXPERT_DATA_SIZE + meshNum + MAX_BATCH_SIZE * (1 + 2 * meshNum + numExperts);
    int sendCount = notifySendDataSize;

    auto sendDataOffset = at::empty({numExperts}, at::dtype(at::kInt).device(x.device()));
    at::Tensor tmpData =
        at::empty({sendCount * numRanks}, at::dtype(at::kInt).device(x.device()));  // for notify temporary use
    at::Tensor recvData = at::empty({sendCount * numRanks}, at::dtype(at::kInt).device(x.device()));
    at::Tensor tokenMeshIdx =
        at::empty({MAX_BATCH_SIZE, meshNum}, at::dtype(at::kInt).device(x.device()));  // offset_outer
    at::Tensor tokenUniquePerMesh = at::empty({meshNum}, at::dtype(at::kInt).device(x.device()));
    at::Tensor epRankTokenCnt =
        at::empty({numExperts, numRanks}, at::dtype(at::kInt).device(x.device()));  // global experts
    at::Tensor localEpTokenCnt =
        at::empty({numLocalExperts}, at::dtype(at::kLong).device(x.device()));
    at::Tensor srcOffsetRankTokenIdx =
        at::empty({numExperts, numRanks, MAX_BATCH_SIZE}, at::dtype(at::kInt).device(x.device()));
    at::Tensor dstOffsetRankTokenIdx =
        at::empty({numExperts, numRanks, MAX_BATCH_SIZE}, at::dtype(at::kInt).device(x.device()));
    // The offsetInner for the current rank and the peer rank
    at::Tensor offsetInner = at::empty({2, MAX_BATCH_SIZE, numExperts}, at::dtype(at::kInt).device(x.device()));
    at::Tensor countOuter = at::empty({MAX_BATCH_SIZE}, at::dtype(at::kInt).device(x.device()));
    at::Tensor expandIdx = at::empty({MAX_BATCH_SIZE, numExperts}, at::dtype(at::kInt).device(x.device()));
    at::Tensor totalRecvToken = torch::empty({1}, at::dtype(at::kInt).device(x.device()));

    // get ep name
    const std::string groupEpStr(groupEp.data(), groupEp.size());
    const char* groupEpPtr = groupEpStr.c_str();

    EXEC_NPU_CMD(
        aclnnNotifyDispatchA5,
        newSendData,
        newNumTokensPerExpert,
        tmpData,
        sendCount,
        numTokens,
        numTopk,
        numExperts,
        groupEpPtr,  // commGroup
        numRanks,     // rankSize
        rank,          // rankId
        localRankSize,
        localRankId,
        sendDataOffset,  // A5 not use
        recvData,
        tokenMeshIdx,
        tokenUniquePerMesh,
        epRankTokenCnt,
        localEpTokenCnt,
        srcOffsetRankTokenIdx,
        dstOffsetRankTokenIdx,
        offsetInner,
        countOuter,
        expandIdx,
        totalRecvToken);
    int totalCount = totalRecvToken.item<int>();
    int numRecvTokens = (totalCount == 0) ? 1 : totalCount;
    auto expandxOut = useQuant ? at::empty({numRecvTokens, hidden}, at::dtype(at::kChar).device(x.device()))
                                 : at::empty({numRecvTokens, hidden}, x.options());
    auto dynamicScalesOut = at::empty({numRecvTokens}, at::dtype(at::kFloat).device(x.device()));
    auto expandScales = at::empty({numRecvTokens}, at::dtype(at::kFloat).device(x.device()));

    EXEC_NPU_CMD(
        aclnnDispatchNormalA5,
        newX,
        expertIds,
        xScales,
        xActiveMask,
        newTopkWeights,
        tokenMeshIdx,
        tokenUniquePerMesh,
        epRankTokenCnt,
        srcOffsetRankTokenIdx,
        dstOffsetRankTokenIdx,
        groupEpPtr,
        numRanks,
        rank,
        numExperts,
        groupEpPtr,
        tpSize,
        tpRank,
        expertShardType,
        sharedExpertNum,
        sharedExpertRankNum,
        quantMode,
        globalBs,
        expertTokenNumsType,
        expandxOut,
        dynamicScalesOut,
        expandIdx,
        expertTokenNums,
        epRecvCount,
        expandScales,
        dispatchWaitRecvCostStatsOut);

    tensorList result = {
        expandxOut,
        dynamicScalesOut,
        expandIdx,
        epRankTokenCnt,
        offsetInner,
        tokenMeshIdx,  // this is offset_outer
        countOuter,
        expandScales
        };
    return result;
}

tensorList MoeDispatchPrefillA5BackwardImplNpu(const at::Tensor &self)
{
    at::Tensor result = at::Tensor(self);
    return {result, result, result, result};
}

tensorList MoeDispatchPrefillA5Impl(MOE_DISPATCH_PREFILL_A5_DEF)
{
    static auto op = torch::Dispatcher::singleton()
                        .findSchemaOrThrow("umdk_cam_op_lib::moe_dispatch_prefill_a5", "")
                        .typed<decltype(MoeDispatchPrefillA5Impl)>();
    return op.call(MOE_DISPATCH_PREFILL_A5_CALL);
}

class MoeDispatchPrefillA5 : public torch::autograd::Function<MoeDispatchPrefillA5> {
public:
    static tensorList forward(
        AutogradContext *ctx, \
        MOE_DISPATCH_PREFILL_A5_DEF)
    {
        at::AutoDispatchBelowADInplaceOrView guard;
        auto result = MoeDispatchPrefillA5Impl(MOE_DISPATCH_PREFILL_A5_CALL);
        return result;
    }

    static tensorList backward(
        AutogradContext *ctx, \
        tensorList grad_outputs)
    {
        return {at::Tensor(), at::Tensor(), at::Tensor(), at::Tensor()};
    }
};

tensorList MoeDispatchPrefillA5ImplAutograd(
    MOE_DISPATCH_PREFILL_A5_DEF)
{
    auto result = MoeDispatchPrefillA5::apply(MOE_DISPATCH_PREFILL_A5_CALL);
    return result;
}

// moe_dispatch_prefill_a5
TORCH_LIBRARY_IMPL(umdk_cam_op_lib, PrivateUse1, m)
{
    m.impl("moe_dispatch_prefill_a5", &MoeDispatchPrefillA5ImplNpu);
    m.impl("moe_dispatch_prefill_a5_backward", &MoeDispatchPrefillA5BackwardImplNpu);
}

TORCH_LIBRARY_IMPL(umdk_cam_op_lib, AutogradPrivateUse1, m)
{
    m.impl("moe_dispatch_prefill_a5", &MoeDispatchPrefillA5ImplAutograd);
}
