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
#include "buffer.hpp"

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m)
{
    m.def("fused_deep_moe", &FusedDeepMoeImplAutograd, "fused_deep_moe");
    m.def("get_dispatch_layout", &GetDispatchLayoutImplAutograd, "get_dispatch_layout");
    m.def("moe_dispatch_prefill", &MoeDispatchPrefillImplAutograd, "moe_dispatch_prefill");
    m.def("moe_combine_prefill", &MoeCombinePrefillImplAutograd, "moe_combine_prefill");
    m.def("moe_dispatch_shmem", &MoeDispatchShmemImplAutograd, "moe_dispatch_shmem");
    m.def("moe_combine_shmem", &MoeCombineShmemImplAutograd, "moe_combine_shmem");
    m.def("get_dispatch_layout_a2", &GetDispatchLayoutA2ImplAutograd, "get_dispatch_layout_a2");
    m.def("moe_dispatch_prefill_a2", &MoeDispatchPrefillA2ImplAutograd, "moe_dispatch_prefill_a2");
    m.def("moe_combine_prefill_a2", &MoeCombinePrefillA2ImplAutograd, "moe_combine_prefill_a2");

    pybind11::class_<fused_deep_moe::Buffer>(m, "FusedDeepMoeBuffer")
        .def(pybind11::init<>())
        .def("init", &fused_deep_moe::Buffer::init,
             pybind11::arg("rank"), pybind11::arg("num_ranks"),
             pybind11::arg("memsize"), pybind11::arg("ip_port"))
        .def("zb_fused_deep_moe", &fused_deep_moe::Buffer::zb_fused_deep_moe,
             pybind11::arg("x"), pybind11::arg("expert_ids"),
             pybind11::arg("gmm1_weight"), pybind11::arg("gmm1_weight_scale"),
             pybind11::arg("gmm2_weight"), pybind11::arg("gmm2_weight_scale"),
             pybind11::arg("expert_scales"),
             pybind11::arg("share_gmm1_weight") = pybind11::none(),
             pybind11::arg("share_gmm1_weight_scale") = pybind11::none(),
             pybind11::arg("share_gmm2_weight") = pybind11::none(),
             pybind11::arg("share_gmm2_weight_scale") = pybind11::none(),
             pybind11::arg("expert_smooth_scales") = pybind11::none(),
             pybind11::arg("share_smooth_scales") = pybind11::none(),
             pybind11::arg("x_active_mask") = pybind11::none(),
             pybind11::arg("group_ep") = "",
             pybind11::arg("ep_rank_size") = 0,
             pybind11::arg("ep_rank_id") = 0,
             pybind11::arg("moe_expert_num") = 0,
             pybind11::arg("quant_mode") = 0,
             pybind11::arg("global_bs") = 0)
        .def("is_initialized", &fused_deep_moe::Buffer::is_initialized)
        .def("get_ext_info", &fused_deep_moe::Buffer::get_ext_info)
        .def("get_shmem_workspace", &fused_deep_moe::Buffer::get_shmem_workspace);
}

TORCH_LIBRARY(umdk_cam_op_lib, m)
{
    m.def("fused_deep_moe(Tensor x, Tensor expert_ids, Tensor[] gmm1_weight, Tensor[] gmm1_weight_scale, \
    Tensor[] gmm2_weight, Tensor[] gmm2_weight_scale, Tensor expert_scales, \
    Tensor? share_gmm1_weight, Tensor? share_gmm1_weight_scale, \
    Tensor? share_gmm2_weight, Tensor? share_gmm2_weight_scale, \
    Tensor? expert_smooth_scales, Tensor? share_smooth_scales, Tensor? x_active_mask, \
    str group_ep, int ep_rank_size, int ep_rank_id, int moe_expert_num, \
    int quant_mode, int global_bs) -> Tensor[]");
    m.def("get_dispatch_layout(Tensor topk_idx, int num_experts, int num_ranks) -> (Tensor, Tensor)");
    m.def("moe_dispatch_prefill(Tensor x, Tensor topk_idx, Tensor topk_weights, Tensor num_tokens_per_expert, \
    Tensor send_token_idx_small, str group_ep, int rank, int num_ranks, bool use_quant) \
    -> (Tensor, Tensor, Tensor, Tensor, Tensor)");
    m.def("moe_combine_prefill(Tensor x, Tensor topk_idx, Tensor topk_weights, Tensor src_idx, Tensor send_head, \
    str group_ep, int rank, int num_ranks) -> Tensor");
    m.def("moe_dispatch_shmem(Tensor x, Tensor expert_ids, Tensor? scales, Tensor? x_active_mask, \
    int ep_world_size, int ep_rank_id, int moe_expert_num, int tp_world_size, int tp_rank_id, \
    int expert_shard_type, int shared_expert_num, int shared_expert_rank_num, int quant_mode, int global_bs, \
    int expert_token_nums_type, int ext_info) -> Tensor[]");
    m.def("moe_combine_shmem(Tensor expand_x, Tensor expert_ids, Tensor expand_idx, Tensor ep_send_counts, \
    Tensor expert_scales, Tensor? tp_send_counts, Tensor? x_active_mask, Tensor? activation_scale, \
    Tensor? weight_scale, Tensor? group_list, Tensor? expand_scales, int ep_world_size, int ep_rank_id, \
    int moe_expert_num, int tp_world_size, int tp_rank_id, int expert_shard_type, int shared_expert_num, \
    int shared_expert_rank_num, int global_bs, int comm_quant_mode, int ext_info, int out_dtype, \
    int group_list_type) -> Tensor");
    m.def("get_dispatch_layout_a2(Tensor topk_idx, int num_experts, int num_ranks) -> (Tensor, Tensor)");
    m.def("moe_dispatch_prefill_a2(Tensor x, Tensor topk_idx, Tensor topk_weights, Tensor num_tokens_per_expert,\
    Tensor notify_send_data, str group_ep, int rank, int num_ranks, bool use_quant) -> Tensor[]");
    m.def("moe_combine_prefill_a2(Tensor x, Tensor topk_idx, Tensor topk_weights, Tensor src_idx, Tensor send_head, \
    Tensor expand_scales, Tensor offset_inner, Tensor offset_outer, Tensor count_outer, str group_ep, int rank, \
    int num_ranks) -> Tensor");
}