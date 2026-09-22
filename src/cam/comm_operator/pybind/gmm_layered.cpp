/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: add gmm_layered pybind extention file
 * Create: 2026-09-22
 * Note:
 * History: 2026-09-22
 */
#include <torch/extension.h>
#include <torch/csrc/autograd/custom_function.h>
#include "pytorch_npu_helper.hpp"

using torch::autograd::AutogradContext;
using torch::autograd::Function;
using tensor_list = std::vector<at::Tensor>;
using namespace at;
using namespace std;

constexpr int64_t SPLIT_ITEM_MIN = 0;
constexpr int64_t SPLIT_ITEM_MAX = 3;
constexpr int64_t B4_PER_B32 = 8;  // eight int4 nibbles per int32 word

// Output dtype derivation (native npu_grouped_matmul model): explicit
// output_dtype wins; otherwise derive per scenario - A8W4 antiquant (int8 x +
// int4 weight) only accepts BF16/FP16 y in the def, A8W8 quant keeps the x
// dtype (int8) for the y columns. Never inherit from the packed weight tensor.
static at::ScalarType DeriveOutputDtype(const at::TensorList &x, const at::TensorList &allWeight,
                                        const c10::optional<at::ScalarType> &outputDtype)
{
    if (outputDtype.has_value()) {
        return *outputDtype;
    }
    if (x[0].scalar_type() == at::kChar && allWeight[0].scalar_type() == at::kInt) {
        return at::kBFloat16;  // A8W4 antiquant
    }
    return x[0].scalar_type();  // A8W8 and friends: keep activation dtype
}

// Native is_weight_trans model: last-two-dims stride swap. A transposed int32
// weight packs the int4 nibbles along a different axis, which this wrapper
// does not model - reject it explicitly rather than compute a wrong N.
static bool IsWeightTransposed(const at::Tensor &tensor)
{
    if (tensor.dim() < 2) {
        return false;
    }
    int64_t dim1 = tensor.dim() - 1;
    int64_t dim2 = tensor.dim() - 2;
    return tensor.stride(dim2) == 1 && tensor.stride(dim1) == tensor.size(dim2);
}

// Layered output model: x may be a single [m, k] activation (the deep-layer
// scenario) or a per-group list; y is always a single [rows, n] tensor with the
// per-group rows concatenated (the kernel writes each group's rows at their
// offsets in one output buffer). A fresh at::empty storage is format-neutral
// (NPU format tags live per-storage), so the NZ weight tag cannot leak.
static tensor_list AllocGmmOutputs(const at::TensorList &xList, const at::TensorList &allWeight,
                                   const std::vector<int64_t> &groupRows,
                                   const c10::optional<at::ScalarType> &outputDtype)
{
    TORCH_CHECK(!xList.empty() && !allWeight.empty(), "x and all_weight must be non-empty lists");
    // torch_npu expresses int4 weight as int32 (eight nibbles per word, last
    // dim n/8); the logical N is the unpacked int4 view.
    int64_t n = allWeight[0].size(-1);
    if (allWeight[0].scalar_type() == at::kInt) {
        TORCH_CHECK(!IsWeightTransposed(allWeight[0]),
                    "transposed int32-packed int4 weight is not supported; pass a non-transposed weight");
        n *= B4_PER_B32;
    }
    auto outOpt = allWeight[0].options().dtype(DeriveOutputDtype(xList, allWeight, outputDtype));
    int64_t rows = 0;
    if (xList.size() == 1) {
        rows = xList[0].size(0);
    } else {
        // per-group x: rows come from the group list values
        TORCH_CHECK(!groupRows.empty(), "per-group x requires a non-empty group_list");
        for (auto r : groupRows) {
            rows += r;
        }
    }
    return {at::empty({rows, n}, outOpt)};
}

// group_list follows the V5 contract (device int64 tensor). The compiled kernel
// variants cover count-style lists in every scenario (the A8W4 templates are
// count-only in the tiling key), so a cumsum-style list is normalized to counts
// host-side; the same values drive the y row allocation. The H2D upload rides
// the current stream, so nothing synchronizes.
static void PrepareGroupList(const c10::optional<std::vector<int64_t>> &groupListOptional,
                             int64_t &groupListType, const at::Device &device, at::Tensor &groupListTensor,
                             std::vector<int64_t> &groupRows)
{
    if (!groupListOptional.has_value() || groupListOptional->empty()) {
        return;
    }
    const auto &gl = *groupListOptional;
    if (groupListType == 0) {  // cumsum -> count normalization
        std::vector<int64_t> counts;
        counts.reserve(gl.size());
        int64_t prev = 0;
        for (auto v : gl) {
            counts.push_back(v - prev);
            prev = v;
        }
        groupListTensor = at::from_blob(const_cast<int64_t *>(counts.data()), {static_cast<int64_t>(counts.size())},
                                        at::TensorOptions().dtype(at::kLong)).clone().to(device);
        groupRows = std::move(counts);
    } else {
        groupListTensor = at::from_blob(const_cast<int64_t *>(gl.data()), {static_cast<int64_t>(gl.size())},
                                        at::TensorOptions().dtype(at::kLong)).clone().to(device);
        groupRows = gl;
    }
    groupListType = 1;  // the normalized form sent downstream
}

tensor_list cam_gmm_layered_impl_npu(
    const at::TensorList &x,
    const at::TensorList &allWeight,
    const at::TensorList &allBias,
    const at::TensorList &allScale,
    const at::Tensor &layerIndex,
    const c10::optional<at::Tensor> &perTokenScaleOptional,
    const c10::optional<std::vector<int64_t>> &groupListOptional,
    const int64_t groupListType,
    const int64_t splitItem,
    const c10::optional<at::ScalarType> &outputDtype)
{
    // Pin the NPU device to the one the tensors live on. Without this, the
    // current device stays at the process default (0) even when the inputs are
    // on, say, npu:7, and EXEC_NPU_CMD derives its stream from the *current*
    // device. The kernel would then be launched in device 0's context while
    // holding device 7 addresses, which faults as an MTE DDR address error or a
    // D-cache bus error, at a location that moves between cores and runs.
    //
    // This must precede PrepareGroupList and AllocGmmOutputs below: both
    // allocate (clone().to(device) and at::empty), so they are the first device
    // operations in the function and need the guard already in effect.
    //
    // device_of is the idiom ATen/DeviceGuard.h documents for this (a bare
    // DeviceGuard(tensor) constructor was removed); it yields nullopt for an
    // undefined tensor, leaving the guard uninitialized rather than throwing.
    const c10::OptionalDeviceGuard device_guard(at::device_of(x[0]));

    TORCH_CHECK(splitItem >= SPLIT_ITEM_MIN && splitItem <= SPLIT_ITEM_MAX,
                "split_item must be one of 0/1/2/3, got ", splitItem);
    TORCH_CHECK(groupListType == 0 || groupListType == 1,
                "group_list_type must be 0 (cumsum) or 1 (count), got ", groupListType);

    at::Tensor groupListTensor;
    std::vector<int64_t> groupRows;
    int64_t groupListTypeNorm = groupListType;
    PrepareGroupList(groupListOptional, groupListTypeNorm, x[0].device(), groupListTensor, groupRows);
    auto yOuts = AllocGmmOutputs(x, allWeight, groupRows, outputDtype);

    at::TensorList yOutList(yOuts);
    // EXEC_NPU_CMD's parameter packing requires lvalues. per_token_scale follows
    // the upstream TensorList contract (per_token_scale=[tensor]); when omitted we
    // synthesize a shape-[0] tensor like the op_api empty normalization would.
    at::Tensor perTokenScaleTensor = perTokenScaleOptional.has_value()
                                         ? *perTokenScaleOptional
                                         : at::empty({0}, x[0].options().dtype(at::kFloat));
    at::TensorList perTokenScaleList(&perTokenScaleTensor, 1);
    EXEC_NPU_CMD(aclnnGroupedMatmulLayered,
        // input
        x, allWeight, allBias, allScale, perTokenScaleList, layerIndex, groupListTensor,
        // attr
        splitItem, groupListTypeNorm,
        // output
        yOutList);
    return yOuts;
}

tensor_list cam_gmm_layered_impl_meta(
    const at::TensorList &x,
    const at::TensorList &allWeight,
    const at::TensorList &allBias,
    const at::TensorList &allScale,
    const at::Tensor &layerIndex,
    const c10::optional<at::Tensor> &perTokenScaleOptional,
    const c10::optional<std::vector<int64_t>> &groupListOptional,
    const int64_t groupListType,
    const int64_t splitItem,
    const c10::optional<at::ScalarType> &outputDtype)
{
    at::Tensor groupListTensor;
    std::vector<int64_t> groupRows;
    int64_t groupListTypeNorm = groupListType;
    PrepareGroupList(groupListOptional, groupListTypeNorm, x[0].device(), groupListTensor, groupRows);
    return AllocGmmOutputs(x, allWeight, groupRows, outputDtype);
}

tensor_list cam_gmm_layered_impl(
    const at::TensorList &x,
    const at::TensorList &allWeight,
    const at::TensorList &allBias,
    const at::TensorList &allScale,
    const at::Tensor &layerIndex,
    const c10::optional<at::Tensor> &perTokenScaleOptional,
    const c10::optional<std::vector<int64_t>> &groupListOptional,
    const int64_t groupListType,
    const int64_t splitItem,
    const c10::optional<at::ScalarType> &outputDtype)
{
    static auto op = torch::Dispatcher::singleton()
                         .findSchemaOrThrow("umdk_cam_op_lib::gmm_layered", "")
                         .typed<decltype(cam_gmm_layered_impl)>();
    return op.call(x, allWeight, allBias, allScale, layerIndex, perTokenScaleOptional, groupListOptional,
        groupListType, splitItem, outputDtype);
}

// bind forward/backward via torch::autograd::Function subclass
class ExtCamGmmLayered : public torch::autograd::Function<ExtCamGmmLayered> {
public:
    static tensor_list forward(AutogradContext *ctx,
                               const at::TensorList &x,
                               const at::TensorList &allWeight,
                               const at::TensorList &allBias,
                               const at::TensorList &allScale,
                               const at::Tensor &layerIndex,
                               const c10::optional<at::Tensor> &perTokenScaleOptional,
                               const c10::optional<std::vector<int64_t>> &groupListOptional,
                               const int64_t groupListType,
                               const int64_t splitItem,
                               const c10::optional<at::ScalarType> &outputDtype)
    {
        at::AutoDispatchBelowADInplaceOrView guard;

        auto result = cam_gmm_layered_impl(x, allWeight, allBias, allScale, layerIndex, perTokenScaleOptional,
            groupListOptional, groupListType, splitItem, outputDtype);
        return result;
    }

    static tensor_list backward(AutogradContext *ctx, tensor_list grad_outputs)
    {
        return {at::Tensor(), at::Tensor(), at::Tensor(), at::Tensor(), at::Tensor(),
                at::Tensor(), at::Tensor(), at::Tensor(), at::Tensor(), at::Tensor()};
    }
};

tensor_list cam_gmm_layered_impl_autograd(
    const at::TensorList &x,
    const at::TensorList &allWeight,
    const at::TensorList &allBias,
    const at::TensorList &allScale,
    const at::Tensor &layerIndex,
    const c10::optional<at::Tensor> &perTokenScaleOptional,
    const c10::optional<std::vector<int64_t>> &groupListOptional,
    const int64_t groupListType,
    const int64_t splitItem,
    const c10::optional<at::ScalarType> &outputDtype)
{
    auto result = ExtCamGmmLayered::apply(x, allWeight, allBias, allScale, layerIndex, perTokenScaleOptional,
        groupListOptional, groupListType, splitItem, outputDtype);
    return result;
}

// gmm_layered
TORCH_LIBRARY_IMPL(umdk_cam_op_lib, PrivateUse1, m)
{
    m.impl("gmm_layered", &cam_gmm_layered_impl_npu);
}

TORCH_LIBRARY_IMPL(umdk_cam_op_lib, AutogradPrivateUse1, m)
{
    m.impl("gmm_layered", &cam_gmm_layered_impl_autograd);
}

// register forward/backward impl for Meta device
TORCH_LIBRARY_IMPL(umdk_cam_op_lib, Meta, m)
{
    m.impl("gmm_layered", &cam_gmm_layered_impl_meta);
}
