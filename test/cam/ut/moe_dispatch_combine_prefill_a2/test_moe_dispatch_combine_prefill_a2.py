#
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# Description: UT for dispatch/combine prefill a2 op
# Create: 2026-2-11
# Note:
# History: 2026-2-11 create file
#

import os
import pytest
import torch
import torch_npu
import numpy as np
import torch.distributed as dist
import torchair
from torchair.configs.compiler_config import CompilerConfig

import umdk_cam_op_lib

from ..util import tool

from ..util.marker import Author
from ..util.marker import MPTest
from ..util.marker import A2Test
from ..util.marker import A3Test
from ..util.marker import SKIP_ENV_RANKSIZE_UNEQUAL

torch_npu.npu.config.allow_internal_format = True

class Module(torch.nn.Module):
    def __init__(self):
        super().__init__()

    def forward(self, x, topk_idx, topk_weights, group_ep, rank, num_ranks, num_experts, use_quant):
        layout_args = {
            "topk_idx": topk_idx,
            "num_experts": num_experts,
            "num_ranks": num_ranks
        }
        layout_output = torch.ops.umdk_cam_op_lib.get_dispatch_layout_a2(**layout_args)
        (
            num_tokens_per_expert,
            notify_send_data,
        ) = layout_output
        dispatch_args = {
            "x": x,
            "topk_idx": topk_idx,
            "topk_weights": topk_weights,
            "num_tokens_per_expert": num_tokens_per_expert,
            "notify_send_data": notify_send_data,
            "group_ep": group_ep,
            "rank": rank,
            "num_ranks": num_ranks,
            "use_quant": use_quant
        }
        dispatch_out = torch.ops.umdk_cam_op_lib.moe_dispatch_prefill_a2(**dispatch_args)
        (
            recv_x,
            dynamic_scales_out,
            expand_idx_out,
            recv_count,
            offset_inner,
            offset_outer,
            count_outer,
            expand_scales,
        ) = dispatch_out

        recv_x = per_token_cast_back(recv_x, dynamic_scales_out) if use_quant else recv_x

        combine_args = {
            "x": recv_x,
            "topk_idx": topk_idx,
            "topk_weights": topk_weights,
            "src_idx": expand_idx_out,
            "send_head": recv_count,
            "expand_scales": expand_scales,
            "offset_inner": offset_inner,
            "offset_outer": offset_outer,
            "count_outer": count_outer,
            "group_ep": group_ep,
            "rank": rank,
            "num_ranks": num_ranks
        }
        combine_x = torch.ops.umdk_cam_op_lib.moe_combine_prefill_a2(**combine_args)

        return combine_x

def per_token_cast_back(x_fp8: torch.Tensor, x_scales: torch.Tensor):
    if x_scales.dtype == torch.int:
        x_scales = x_scales.view(dtype=torch.int8).to(torch.int) << 23
        x_scales = x_scales.view(dtype=torch.float)
    x_fp32 = x_fp8.to(torch.float32).view(x_fp8.size(0), -1, 128)
    x_scales = x_scales.view(x_fp8.size(0), -1, 1)
    return (x_fp32 * x_scales).view(x_fp8.shape).to(torch.bfloat16)

def calc_diff(x, y):
    x, y = x.astype(np.float64) + 1, y.astype(np.float64) + 1
    denominator = (x * x + y * y).sum()
    sim = 2 * (x * y).sum() / denominator
    return 1 - sim

def gen_x(rank, num_tokens, hidden_size):
    x = np.ones((num_tokens, hidden_size), dtype=np.float32) * rank
    return x

def gen_topk_idx(num_experts, num_tokens, num_topk):
    scores = np.abs(np.random.randn(num_tokens, num_experts).astype(np.float32)) + 1
    topk_idx = np.argpartition(scores, -num_topk, axis=-1)[:, -num_topk:]
    return topk_idx

def gen_topk_weights(rank, num_tokens, num_topk):
    topk_weights = np.ones((num_tokens, num_topk), dtype=np.float32) * rank
    return topk_weights

CASE_16RANK = {
    "num_ranks": 16,
    "num_experts": 16,
    "num_topk": 8,
    "num_tokens": 32,
    "hidden_size": 7168,
}

@MPTest
@A2Test
@SKIP_ENV_RANKSIZE_UNEQUAL(16)
@pytest.mark.parametrize("mode, use_quant", [('Eager', False)])
def test_base_test(mode, use_quant):
    if mode == "GE" and tool.is_run_for_cov():
        return

    rank = tool.get_rank()
    worldSize = tool.get_world_size()

    case = CASE_16RANK
    num_ranks = case["num_ranks"]
    num_experts = case["num_experts"]
    num_topk = case["num_topk"]
    num_tokens = case["num_tokens"]
    hidden_size = case["hidden_size"]

    ep_ranks_list = list(np.arange(0, worldSize))
    ep_group = dist.new_group(backend="hccl", ranks=ep_ranks_list)

    group_ep = ep_group._get_backend(torch.device("npu")).get_hccl_comm_name(rank)

    mod = Module().npu()

    x_np = gen_x(rank, num_tokens, hidden_size)
    topk_idx_np = gen_topk_idx(num_experts, num_tokens, num_topk)
    topk_weights_np = gen_topk_weights(rank, num_tokens, num_topk)

    x = torch.from_numpy(x_np).to(dtype=torch.bfloat16).npu()
    topk_idx = torch.from_numpy(topk_idx_np.astype(np.int64)).npu()
    topk_weights = torch.from_numpy(topk_weights_np).npu()
    os.environ['HCCL_INTRA_PCIE_ENABLE'] = "1"
    os.environ['HCCL_INTRA_ROCE_ENABLE'] = "0"
    
    out = mod(
        x=x,
        topk_idx=topk_idx,
        topk_weights=topk_weights,
        group_ep=group_ep,
        rank=rank,
        num_ranks=worldSize,
        num_experts=num_experts,
        use_quant=use_quant
    )

    torch.npu.synchronize()

    check_x = out.cpu().float().numpy()
    ref_weights = np.where(topk_idx_np == -1, np.float32(0), topk_weights_np).sum(axis=1).reshape(-1, 1)
    ref_x = x_np * ref_weights

    diff = calc_diff(check_x, ref_x)
    assert(diff < 5e-5)
    # clear env
    if 'HCCL_INTRA_PCIE_ENABLE' in os.environ:
        del os.environ['HCCL_INTRA_PCIE_ENABLE']
    if 'HCCL_INTRA_ROCE_ENABLE' in os.environ:
        del os.environ['HCCL_INTRA_ROCE_ENABLE']


# =====================================================================
# A3 host-side coverage tests for A2 operators
# These tests exercise A2 operator tiling (host-side) code on A3 platform.
# On A3, the tiling runs through all validation logic and then fails at
# the platform check at the end.
# =====================================================================

CASE_16RANK_SMALL_TOPK = {
    "num_ranks": 16,
    "num_experts": 16,
    "num_topk": 4,
    "num_tokens": 32,
    "hidden_size": 7168,
}

CASE_16RANK_LARGE_BATCH = {
    "num_ranks": 16,
    "num_experts": 16,
    "num_topk": 8,
    "num_tokens": 64,
    "hidden_size": 7168,
}

CASE_16RANK_MORE_EXPERTS = {
    "num_ranks": 16,
    "num_experts": 32,
    "num_topk": 8,
    "num_tokens": 32,
    "hidden_size": 7168,
}


def _setup_ep_group():
    rank = tool.get_rank()
    worldSize = tool.get_world_size()
    ep_ranks_list = list(np.arange(0, worldSize))
    ep_group = dist.new_group(backend="hccl", ranks=ep_ranks_list)
    group_ep = ep_group._get_backend(torch.device("npu")).get_hccl_comm_name(rank)
    return rank, worldSize, group_ep


def _set_layered_env():
    os.environ['HCCL_INTRA_PCIE_ENABLE'] = "1"
    os.environ['HCCL_INTRA_ROCE_ENABLE'] = "0"


def _clear_layered_env():
    if 'HCCL_INTRA_PCIE_ENABLE' in os.environ:
        del os.environ['HCCL_INTRA_PCIE_ENABLE']
    if 'HCCL_INTRA_ROCE_ENABLE' in os.environ:
        del os.environ['HCCL_INTRA_ROCE_ENABLE']


@MPTest
@A3Test
@SKIP_ENV_RANKSIZE_UNEQUAL(16)
@pytest.mark.parametrize("case_cfg", [
    CASE_16RANK,
    CASE_16RANK_SMALL_TOPK,
    CASE_16RANK_LARGE_BATCH,
    CASE_16RANK_MORE_EXPERTS,
])
def test_layout_a2_on_a3(case_cfg):
    """get_dispatch_layout_a2 uses DispatchLayout which supports A3, should succeed."""
    rank, worldSize, group_ep = _setup_ep_group()

    num_experts = case_cfg["num_experts"]
    num_topk = case_cfg["num_topk"]
    num_tokens = case_cfg["num_tokens"]

    topk_idx = torch.from_numpy(gen_topk_idx(num_experts, num_tokens, num_topk).astype(np.int64)).npu()

    layout_output = torch.ops.umdk_cam_op_lib.get_dispatch_layout_a2(
        topk_idx=topk_idx, num_experts=num_experts, num_ranks=worldSize)
    num_tokens_per_expert, notify_send_data = layout_output
    torch.npu.synchronize()

    assert num_tokens_per_expert is not None
    assert notify_send_data is not None


@MPTest
@A3Test
@SKIP_ENV_RANKSIZE_UNEQUAL(16)
@pytest.mark.parametrize("case_cfg, use_quant", [
    (CASE_16RANK, False),
    (CASE_16RANK, True),
    (CASE_16RANK_SMALL_TOPK, False),
    (CASE_16RANK_LARGE_BATCH, False),
    (CASE_16RANK_MORE_EXPERTS, False),
])
def test_dispatch_a2_host_tiling_on_a3(case_cfg, use_quant):
    """Exercise NotifyDispatchA2 + DispatchNormalA2 host-side tiling on A3.
    Tiling runs all validation logic then fails at platform check."""
    rank, worldSize, group_ep = _setup_ep_group()

    num_experts = case_cfg["num_experts"]
    num_topk = case_cfg["num_topk"]
    num_tokens = case_cfg["num_tokens"]
    hidden_size = case_cfg["hidden_size"]

    x = torch.from_numpy(gen_x(rank, num_tokens, hidden_size)).to(dtype=torch.bfloat16).npu()
    topk_idx = torch.from_numpy(gen_topk_idx(num_experts, num_tokens, num_topk).astype(np.int64)).npu()
    topk_weights = torch.from_numpy(gen_topk_weights(rank, num_tokens, num_topk)).npu()

    layout_output = torch.ops.umdk_cam_op_lib.get_dispatch_layout_a2(
        topk_idx=topk_idx, num_experts=num_experts, num_ranks=worldSize)
    num_tokens_per_expert, notify_send_data = layout_output

    _set_layered_env()
    try:
        with pytest.raises(RuntimeError):
            torch.ops.umdk_cam_op_lib.moe_dispatch_prefill_a2(
                x=x, topk_idx=topk_idx, topk_weights=topk_weights,
                num_tokens_per_expert=num_tokens_per_expert,
                notify_send_data=notify_send_data,
                group_ep=group_ep, rank=rank, num_ranks=worldSize,
                use_quant=use_quant)
            torch.npu.synchronize()
    finally:
        _clear_layered_env()


@MPTest
@A3Test
@SKIP_ENV_RANKSIZE_UNEQUAL(16)
def test_dispatch_a2_host_tiling_no_layered_on_a3():
    """Exercise dispatch A2 tiling without layered env vars.
    NotifyDispatchA2 tiling will fail at env var check (different host code path)."""
    rank, worldSize, group_ep = _setup_ep_group()

    case = CASE_16RANK
    num_experts = case["num_experts"]
    num_topk = case["num_topk"]
    num_tokens = case["num_tokens"]
    hidden_size = case["hidden_size"]

    x = torch.from_numpy(gen_x(rank, num_tokens, hidden_size)).to(dtype=torch.bfloat16).npu()
    topk_idx = torch.from_numpy(gen_topk_idx(num_experts, num_tokens, num_topk).astype(np.int64)).npu()
    topk_weights = torch.from_numpy(gen_topk_weights(rank, num_tokens, num_topk)).npu()

    layout_output = torch.ops.umdk_cam_op_lib.get_dispatch_layout_a2(
        topk_idx=topk_idx, num_experts=num_experts, num_ranks=worldSize)
    num_tokens_per_expert, notify_send_data = layout_output

    _clear_layered_env()
    with pytest.raises(RuntimeError):
        torch.ops.umdk_cam_op_lib.moe_dispatch_prefill_a2(
            x=x, topk_idx=topk_idx, topk_weights=topk_weights,
            num_tokens_per_expert=num_tokens_per_expert,
            notify_send_data=notify_send_data,
            group_ep=group_ep, rank=rank, num_ranks=worldSize,
            use_quant=False)
        torch.npu.synchronize()


@MPTest
@A3Test
@SKIP_ENV_RANKSIZE_UNEQUAL(16)
@pytest.mark.parametrize("case_cfg", [
    CASE_16RANK,
    CASE_16RANK_SMALL_TOPK,
    CASE_16RANK_LARGE_BATCH,
    CASE_16RANK_MORE_EXPERTS,
])
def test_combine_a2_host_tiling_on_a3(case_cfg):
    """Exercise MoeDistributeCombineA2 host-side tiling on A3 (layered path).
    Constructs valid dummy inputs to pass shape/attr checks, fails at platform check."""
    rank, worldSize, group_ep = _setup_ep_group()

    num_experts = case_cfg["num_experts"]
    num_topk = case_cfg["num_topk"]
    num_tokens = case_cfg["num_tokens"]
    hidden_size = case_cfg["hidden_size"]
    total_tokens = num_tokens * num_topk

    # Construct valid inputs for combine A2 pybind
    recv_x = torch.from_numpy(np.ones((total_tokens, hidden_size), dtype=np.float32)).to(dtype=torch.bfloat16).npu()
    topk_idx = torch.from_numpy(gen_topk_idx(num_experts, num_tokens, num_topk).astype(np.int64)).npu()
    topk_weights = torch.from_numpy(gen_topk_weights(rank, num_tokens, num_topk)).npu()
    src_idx = torch.from_numpy(np.zeros(total_tokens, dtype=np.int32)).npu()
    send_head = torch.from_numpy(np.ones(num_experts, dtype=np.int32)).npu()
    expand_scales = torch.from_numpy(np.ones(total_tokens, dtype=np.float32)).npu()
    offset_inner = torch.from_numpy(np.zeros(total_tokens, dtype=np.int32)).npu()
    offset_outer = torch.from_numpy(np.zeros(total_tokens, dtype=np.int32)).npu()
    count_outer = torch.from_numpy(np.zeros(num_tokens, dtype=np.int32)).npu()

    _set_layered_env()
    try:
        with pytest.raises(RuntimeError):
            torch.ops.umdk_cam_op_lib.moe_combine_prefill_a2(
                x=recv_x, topk_idx=topk_idx, topk_weights=topk_weights,
                src_idx=src_idx, send_head=send_head,
                expand_scales=expand_scales,
                offset_inner=offset_inner,
                offset_outer=offset_outer,
                count_outer=count_outer,
                group_ep=group_ep, rank=rank, num_ranks=worldSize)
            torch.npu.synchronize()
    finally:
        _clear_layered_env()


@MPTest
@A3Test
@SKIP_ENV_RANKSIZE_UNEQUAL(16)
@pytest.mark.parametrize("case_cfg", [
    CASE_16RANK,
    CASE_16RANK_MORE_EXPERTS,
])
def test_combine_a2_host_tiling_non_layered_on_a3(case_cfg):
    """Exercise MoeDistributeCombineA2 host-side tiling on A3 (non-layered path)."""
    rank, worldSize, group_ep = _setup_ep_group()

    num_experts = case_cfg["num_experts"]
    num_topk = case_cfg["num_topk"]
    num_tokens = case_cfg["num_tokens"]
    hidden_size = case_cfg["hidden_size"]
    total_tokens = num_tokens * num_topk

    recv_x = torch.from_numpy(np.ones((total_tokens, hidden_size), dtype=np.float32)).to(dtype=torch.bfloat16).npu()
    topk_idx = torch.from_numpy(gen_topk_idx(num_experts, num_tokens, num_topk).astype(np.int64)).npu()
    topk_weights = torch.from_numpy(gen_topk_weights(rank, num_tokens, num_topk)).npu()
    src_idx = torch.from_numpy(np.zeros(total_tokens, dtype=np.int32)).npu()
    send_head = torch.from_numpy(np.ones(num_experts, dtype=np.int32)).npu()
    expand_scales = torch.from_numpy(np.ones(total_tokens, dtype=np.float32)).npu()
    offset_inner = torch.from_numpy(np.zeros(total_tokens, dtype=np.int32)).npu()
    offset_outer = torch.from_numpy(np.zeros(total_tokens, dtype=np.int32)).npu()
    count_outer = torch.from_numpy(np.zeros(num_tokens, dtype=np.int32)).npu()

    _clear_layered_env()
    with pytest.raises(RuntimeError):
        torch.ops.umdk_cam_op_lib.moe_combine_prefill_a2(
            x=recv_x, topk_idx=topk_idx, topk_weights=topk_weights,
            src_idx=src_idx, send_head=send_head,
            expand_scales=expand_scales,
            offset_inner=offset_inner,
            offset_outer=offset_outer,
            count_outer=count_outer,
            group_ep=group_ep, rank=rank, num_ranks=worldSize)
        torch.npu.synchronize()