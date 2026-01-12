/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: add pybind
 * Create: 2025-12-10
 * Note:
 * History: 2025-12-10 add pybind
 */

#include "functions.h"
#include <torch/extension.h>

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m)
{
    m.def("fused_deep_moe", &FusedDeepMoeImplAutograd, "fused_deep_moe");
    m.def("get_dispatch_layout", &GetDispatchLayoutImplAutograd, "get_dispatch_layout");
    m.def("moe_dispatch_prefill", &MoeDispatchPrefillImplAutograd, "moe_dispatch_prefill");
    m.def("moe_combine_prefill", &MoeCombinePrefillImplAutograd, "moe_combine_prefill");
    m.def("moe_dispatch_shmem", &MoeDispatchShmemImplAutograd, "moe_dispatch_shmem");
    m.def("moe_combine_shmem", &MoeCombineShmemImplAutograd, "moe_combine_shmem");
}

TORCH_LIBRARY(umdk_cam_op_lib, m)
{
    m.def("fused_deep_moe(Tensor x, Tensor expertIds, Tensor[] gmm1PermutedWeight, Tensor[] gmm1PermutedWeightScale, \
    Tensor[] gmm2Weight, Tensor[] gmm2WeightScale, Tensor expertScales, Tensor? expertSmoothScales, \
    Tensor? xActiveMask, str groupEp, int epRankSize, int epRankId, int moeExpertNum, int sharedExpertNum, \
    int sharedExpertRankNum, int quantMode, int globalBs) -> Tensor[]");
    m.def("get_dispatch_layout(Tensor topkIdx, int numExperts, int numRanks) -> (Tensor, Tensor)");
    m.def("moe_dispatch_prefill(Tensor x, Tensor topkIdx, Tensor topkWeights, Tensor numTokensPerExpert, \
    Tensor sendTokenIdxSmall, str groupEp, int rank, int numRanks, bool useQuant) \
    -> (Tensor, Tensor, Tensor, Tensor, Tensor)");
    m.def("moe_combine_prefill(Tensor x, Tensor topkIdx, Tensor topkWeights, Tensor srcIdx, Tensor sendHead, \
    str groupEp, int rank, int numRanks) -> Tensor");
    m.def("moe_dispatch_shmem(Tensor x, Tensor expertIds, Tensor? scales, Tensor? xActiveMask, \
    int epWorldSize, int epRankId, int moeExpertNum, int tpWorldSize, int tpRankId, \
    int expertShardType, int sharedExpertNum, int sharedExpertRankNum, int quantMode, int globalBS, int expertTokenNumsType, int extInfo) -> Tensor[]");
    m.def("moe_combine_shmem(Tensor expandX, Tensor expertIds, Tensor expandIdx, Tensor epSendCounts, \
    Tensor expertScales, Tensor? tpSendCounts, Tensor? xActiveMask, Tensor? activationScale, Tensor? weightScale, \
    Tensor? groupList, Tensor? expandScales, int epWorldSize, int epRankId, int moeExpertNum, int tpWorldSize, \
    int tpRankId, int expertShardType, int sharedExpertNum, int sharedExpertRankNum, int globalBS, int commQuantMode, \
    int extInfo, int outDtype, int groupListType) -> Tensor");
}