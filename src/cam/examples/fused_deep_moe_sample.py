
import copy
import csv
import datetime
import multiprocessing as mp
import os
import socket
import time
from enum import Enum
from math import ceil
from typing import List
import gc
import sys
from pathlib import Path
import argparse

import numpy as np  # type: ignore
import pandas as pd  # type: ignore
import pytest  # type: ignore
import torch  # type: ignore
import torch.distributed as dist  # type: ignore
import torch.multiprocessing as mp  # type: ignore
import torch_npu  # type: ignore
import torchair
import random

from copy import deepcopy
from torch.distributed import ReduceOp  # type: ignore
from torch.multiprocessing import Manager, Process  # type: ignore
from torchair.configs.compiler_config import CompilerConfig
import xpu_ops
xpu_ops.load_xpu_ops()
# torch._dynamo.config.inline_inbuilt_nn_modules = False
os.environ["TORCHAIR_LOG_LEVEL"] = "debug"
os.environ["TORCHAIR_LOG_FILE"] = "torchair.log"
import umdk_cam_op_lib
import fused_deep_moe_converter
from fused_deep_moe_converter import *
NODE_NUM = int(os.environ.get("NODE_NUM", 1))
NODE_RANK = int(os.environ.get("NODE_RANK", 0))
MASTER_ADDR = os.environ.get("MASTER_ADDR", "127.0.0.1")
MASTER_PORT = int(os.environ.get("MASTER_PORT", 29500))
print(f"NODE_NUM: {NODE_NUM}, NODE_RANK: {NODE_RANK}, MASTER_ADDR: {MASTER_ADDR}, MASTER_PORT: {MASTER_PORT}")

def get_xpu_ops_path():
    current_path = os.getcwd()
    parts = current_path.split(os.sep)
    for i in range(len(parts) - 1, -1, -1):
        if parts[i] == 'xpu_ops':
            return os.path.dirname(os.sep.join(parts[: i + 1]))
    return None


def setup_libs():
    repo_path = get_xpu_ops_path()
    xpu_ops_lib = os.path.join(repo_path, 'xpu_ops/src/build_out/lib/libxpu_ops.so')
    if not os.path.exists(xpu_ops_lib):
        print(xpu_ops_lib)
        raise RuntimeError('can not find xpu ops lib')

    torch.ops.load_library(xpu_ops_lib)
    torch.classes.load_library(xpu_ops_lib)

torch.manual_seed(42)
torch_npu.npu.config.allow_internal_format = True
LOG_NAME = "fused_deep_moe_sample_logs"
BASE_KWARGS = {
    "batch_size": 64,
    "token_hidden_size": 7168,
    "moe_intermediate_size": 2048,
    "ep_world_size": 16,
    "moe_expert_num": 64,
    "shared_expert_rank_num": 0,
    "top_k": 8,
    "test_bfloat16": True,
    "enable_dynamic_bs": False,
    "test_graph": True,
    "with_mc2_mask": False,
    "dynamic_eplb": False,
    "with_share": False,
    "with_smooth": True,
    "share_expert_intermediate_size": 2048  # shared expert gmm1HLen
}

def redirect_output(log_file_path):
    log_path = Path(LOG_NAME) / log_file_path
    log_path.parent.mkdir(parents=True, exist_ok=True)
    f = open(LOG_NAME + "/" + log_file_path, "w")
    os.dup2(f.fileno(), sys.stdout.fileno())
    os.dup2(f.fileno(), sys.stderr.fileno())
    return f

def output_to_file(rank_id):
    return (rank_id % 16) != 0 # and False


class DecodeMoeOps(torch.nn.Module):

    def __init__(self,
                 gmm1_weight,
                 gmm1_weight_scale,
                 gmm2_weight,
                 gmm2_weight_scale,
                 ep_hcomm_info,
                 batch_size,
                 token_hidden_size,
                 moe_intermediate_size,
                 ep_world_size,
                 moe_expert_num,
                 global_rank_id,
                 shared_expert_rank_num=0,
                 dynamic_eplb=False,
                 with_share=False,
                 share_expert_intermediate_size=None,
                 share_gmm1_weight=None,
                 share_gmm1_weight_scale=None,
                 share_gmm2_weight=None,
                 share_gmm2_weight_scale=None):
        super().__init__()
        self.ep_hcomm_info = ep_hcomm_info
        self.batch_size = batch_size
        self.token_hidden_size = token_hidden_size
        self.moe_intermediate_size = moe_intermediate_size
        self.share_expert_intermediate_size = share_expert_intermediate_size if share_expert_intermediate_size is not None else moe_intermediate_size
        self.ep_world_size = ep_world_size
        self.moe_expert_num = moe_expert_num
        self.global_rank_id = global_rank_id
        self.shared_expert_rank_num = shared_expert_rank_num
        self.with_share = with_share
        is_shared_expert = global_rank_id < shared_expert_rank_num
        moe_expert_num_per_rank = moe_expert_num // (ep_world_size - shared_expert_rank_num)
        self.local_expert_num = 1 if is_shared_expert else moe_expert_num_per_rank
        self.ep_recv_count_size = self.local_expert_num * ep_world_size
        self.dynamic_eplb = dynamic_eplb
        self.gmm1_weight = torch.empty([
            self.local_expert_num, self.token_hidden_size,
            self.moe_intermediate_size * 2
        ])
        self.gmm1_weight_scale = torch.empty(
            [self.local_expert_num, self.moe_intermediate_size * 2])
        self.gmm2_weight = torch.empty([
            self.local_expert_num, self.moe_intermediate_size,
            self.token_hidden_size
        ])
        self.gmm2_weight_scale = torch.empty(
            [self.local_expert_num, self.token_hidden_size])
        self._process_weights_after_loading(gmm1_weight, gmm1_weight_scale,
                                            gmm2_weight, gmm2_weight_scale,
                                            share_gmm1_weight, share_gmm1_weight_scale,
                                            share_gmm2_weight, share_gmm2_weight_scale)

    def _process_weights_after_loading(self, gmm1_weight, gmm1_weight_scale,
                                       gmm2_weight, gmm2_weight_scale,
                                       share_gmm1_weight=None, share_gmm1_weight_scale=None,
                                       share_gmm2_weight=None, share_gmm2_weight_scale=None):
        gmm1_weight = torch_npu.npu_format_cast(gmm1_weight,
                                                torch_npu.Format.FRACTAL_NZ)
        gmm2_weight = torch_npu.npu_format_cast(gmm2_weight,
                                                torch_npu.Format.FRACTAL_NZ)
        self.gmm1_weight = torch.nn.Parameter(gmm1_weight, requires_grad=False)
        self.gmm1_weight_scale = torch.nn.Parameter(gmm1_weight_scale,
                                                    requires_grad=False)
        self.gmm2_weight = torch.nn.Parameter(gmm2_weight, requires_grad=False)
        self.gmm2_weight_scale = torch.nn.Parameter(gmm2_weight_scale,
                                                    requires_grad=False)

        self.gmm1_weight_scale_fp32 = torch.nn.Parameter(
            gmm1_weight_scale.float(), requires_grad=False)
        self.gmm2_weight_scale_fp32 = torch.nn.Parameter(
            gmm2_weight_scale.float(), requires_grad=False)
        
        # Process shared expert weights (similar to routing expert, but not in list)
        if self.with_share and share_gmm1_weight is not None:
            share_gmm1_weight = torch_npu.npu_format_cast(share_gmm1_weight,
                                                         torch_npu.Format.FRACTAL_NZ)
            share_gmm2_weight = torch_npu.npu_format_cast(share_gmm2_weight,
                                                         torch_npu.Format.FRACTAL_NZ)
            self.share_gmm1_weight = torch.nn.Parameter(share_gmm1_weight, requires_grad=False)
            self.share_gmm1_weight_scale = torch.nn.Parameter(share_gmm1_weight_scale,
                                                             requires_grad=False)
            self.share_gmm2_weight = torch.nn.Parameter(share_gmm2_weight, requires_grad=False)
            self.share_gmm2_weight_scale = torch.nn.Parameter(share_gmm2_weight_scale,
                                                             requires_grad=False)
            self.share_gmm1_weight_scale_fp32 = torch.nn.Parameter(
                share_gmm1_weight_scale.float(), requires_grad=False)
            self.share_gmm2_weight_scale_fp32 = torch.nn.Parameter(
                share_gmm2_weight_scale.float(), requires_grad=False)
        else:
            self.share_gmm1_weight = None
            self.share_gmm1_weight_scale = None
            self.share_gmm2_weight = None
            self.share_gmm2_weight_scale = None
            self.share_gmm1_weight_scale_fp32 = None
            self.share_gmm2_weight_scale_fp32 = None

    def _apply_ops(self, x, expert_ids, smooth_scales, share_smooth_scales, share_smooth_scales_fp32, expert_scales, x_active_mask):
        raise NotImplementedError("To be implemented in subclass")

    def forward(self, x, expert_ids, smooth_scales, share_smooth_scales, share_smooth_scales_fp32, expert_scales, x_active_mask):
        return self._apply_ops(x, expert_ids, smooth_scales, share_smooth_scales, share_smooth_scales_fp32, expert_scales, x_active_mask)

def swish(x):
    return x / (1 + torch.exp(-x))

def swiglu(x, act_left = False):
    dtype = x.dtype
    x = x.float()
    bs, length = x.shape
    print(f"{x.shape=}, {bs= }, {length= }")
    left = x[:,0:(length//2)].contiguous()
    right = x[:, (length//2):].contiguous()
    if act_left:
        left = swish(left)
    else:
        right = swish(right)
    result = left * right
    return result.to(dtype), left.to(dtype), right.to(dtype)

class SmallOps(DecodeMoeOps):

    def __init__(self,
                 gmm1_weight,
                 gmm1_weight_scale,
                 gmm2_weight,
                 gmm2_weight_scale,
                 ep_hcomm_info,
                 batch_size,
                 token_hidden_size,
                 moe_intermediate_size,
                 ep_world_size,
                 moe_expert_num,
                 global_rank_id,
                 shared_expert_rank_num=0,
                 dynamic_eplb=False,
                 with_share=False,
                 share_expert_intermediate_size=None,
                 share_gmm1_weight=None,
                 share_gmm1_weight_scale=None,
                 share_gmm2_weight=None,
                 share_gmm2_weight_scale=None):
        super().__init__(gmm1_weight, gmm1_weight_scale, gmm2_weight,
                         gmm2_weight_scale, ep_hcomm_info, batch_size,
                         token_hidden_size, moe_intermediate_size,
                         ep_world_size, moe_expert_num, global_rank_id,
                         shared_expert_rank_num, dynamic_eplb, with_share,
                         share_expert_intermediate_size,
                         share_gmm1_weight, share_gmm1_weight_scale,
                         share_gmm2_weight, share_gmm2_weight_scale)
        self.tp_hcomm_info = ""

    def share_compute(self, x, share_smooth_scales):
        x1_int8, x1_scale = torch_npu.npu_dynamic_quant(x, smooth_scales=share_smooth_scales)
        gmm1_result = torch_npu.npu_quant_matmul(x1_int8, self.share_gmm1_weight, self.share_gmm1_weight_scale, pertoken_scale=None, output_dtype=torch.int32)
        x2_int8, x2_scale = torch_npu.npu_dequant_swiglu_quant(
            x=gmm1_result,
            weight_scale=self.share_gmm1_weight_scale_fp32,
            activation_scale=x1_scale,
            bias=None,
            quant_scale=None,
            quant_offset=None,
            group_index=None,
            activate_left=True,
            quant_mode=1,
        )
        gmm2_result = torch_npu.npu_quant_matmul(x2_int8, self.share_gmm2_weight, self.share_gmm2_weight_scale, pertoken_scale=x2_scale, output_dtype=x.dtype)
        return gmm2_result

    def _apply_ops(self, x, expert_ids, smooth_scales, share_smooth_scales, share_smooth_scales_fp32, expert_scales, x_active_mask):
        return (x, x, None)
        if self.with_share:
            share_output = self.share_compute(x, share_smooth_scales)
        else:
            share_output = None
        outputs = torch.ops.xpu_ops.moe_distribute_dispatch_v2(
            x,
            expert_ids,
            self.ep_hcomm_info,
            self.ep_world_size,
            self.global_rank_id,
            self.moe_expert_num,
            smooth_scales,
            x_active_mask,
            expert_scales,
            self.tp_hcomm_info,
            1, # tp_world_size
            0, # tp_rank_id
            0, # expert_shard_type
            1, # shared_expert_num
            self.shared_expert_rank_num, # shared_expert_rank_num
            2, # quant_mode
            self.batch_size * self.ep_world_size, # global_bs
            1, # expert_token_nums_type
            "fullmesh_v2")
        # outputs = torch_npu.npu_moe_distribute_dispatch_v2(
        #     x=x,
        #     expert_ids=expert_ids,
        #     expert_scales=expert_scales,
        #     scales=smooth_scales,
        #     x_active_mask=x_active_mask,
        #     group_ep=self.ep_hcomm_info,
        #     ep_world_size=self.ep_world_size,
        #     ep_rank_id=self.global_rank_id,
        #     moe_expert_num=self.moe_expert_num,
        #     group_tp=self.tp_hcomm_info,
        #     tp_world_size=1,
        #     tp_rank_id=0,
        #     expert_shard_type=0,
        #     shared_expert_num=1,
        #     shared_expert_rank_num=self.shared_expert_rank_num,
        #     quant_mode=2,
        #     global_bs=self.batch_size * self.ep_world_size,
        #     expert_token_nums_type=1,  # 0代表前缀和，1代表各自数量
        #     # comm_alg="fullmesh_v2"
        # )

        expand_x, dynamic_scales, assist_info_for_combine, expert_token_nums, ep_send_counts, tp_send_counts, expand_scales = outputs
        output_dtype = x.dtype

        y1_int32 = torch_npu.npu_grouped_matmul(
            x=[expand_x],
            weight=[self.gmm1_weight],
            split_item=3,
            group_list_type=1,  # 默认为0，代表前缀和形式
            group_type=0,  # 0代表m轴分组
            group_list=expert_token_nums,
            output_dtype=torch.int32,
            tuning_config=[256] # xpu-gpu add
            )[0]

        y1, y1_scale = torch_npu.npu_dequant_swiglu_quant(
            x=y1_int32,
            weight_scale=self.gmm1_weight_scale_fp32,
            activation_scale=dynamic_scales,
            bias=None,
            quant_scale=None,
            quant_offset=None,
            group_index=expert_token_nums,
            activate_left=False, # seed add!!!!!!!
            quant_mode=1,
        )
        y2 = torch_npu.npu_grouped_matmul(x=[y1],
                                          weight=[self.gmm2_weight],
                                          scale=[self.gmm2_weight_scale],
                                          per_token_scale=[y1_scale],
                                          split_item=2,
                                          group_list_type=1,
                                          group_type=0,
                                          group_list=expert_token_nums,
                                          output_dtype=output_dtype)[0]
        combine_output = torch_npu.npu_moe_distribute_combine_v2(
            expand_x=y2,
            expert_ids=expert_ids,
            assist_info_for_combine=assist_info_for_combine,
            ep_send_counts=ep_send_counts,
            expert_scales=expert_scales,
            x_active_mask=x_active_mask,
            group_ep=self.ep_hcomm_info,
            ep_world_size=self.ep_world_size,
            ep_rank_id=self.global_rank_id,
            moe_expert_num=self.moe_expert_num,
            tp_send_counts=tp_send_counts,
            expand_scales=expand_scales,
            group_tp=self.tp_hcomm_info,
            tp_world_size=1,
            tp_rank_id=0,
            expert_shard_type=0,
            shared_expert_num=1,
            shared_expert_rank_num=self.shared_expert_rank_num,
            global_bs=self.batch_size * self.ep_world_size)
        return (combine_output, share_output, expert_token_nums)


class FusionOp(DecodeMoeOps):

    def __init__(self,
                 gmm1_weight,
                 gmm1_weight_scale,
                 gmm2_weight,
                 gmm2_weight_scale,
                 ep_hcomm_info,
                 batch_size,
                 token_hidden_size,
                 moe_intermediate_size,
                 ep_world_size,
                 moe_expert_num,
                 global_rank_id,
                 shared_expert_rank_num=0,
                 dynamic_eplb=False,
                 with_share=False,
                 share_expert_intermediate_size=None,
                 share_gmm1_weight=None,
                 share_gmm1_weight_scale=None,
                 share_gmm2_weight=None,
                 share_gmm2_weight_scale=None,
                 comm_args=None):
        super().__init__(gmm1_weight, gmm1_weight_scale, gmm2_weight,
                         gmm2_weight_scale, ep_hcomm_info, batch_size,
                         token_hidden_size, moe_intermediate_size,
                         ep_world_size, moe_expert_num, global_rank_id,
                         shared_expert_rank_num, dynamic_eplb, with_share,
                         share_expert_intermediate_size,
                         share_gmm1_weight, share_gmm1_weight_scale,
                         share_gmm2_weight, share_gmm2_weight_scale)
        self.comm_args = comm_args

    def _apply_ops(self, x, expert_ids, smooth_scales, share_smooth_scales, share_smooth_scales_fp32, expert_scales, x_active_mask):
        # setup_libs()
        # torch.ops.air.scope_enter(["_super_kernel_scope", "_super_kernel_options"],["sp1", ""])
        # try:
        with torchair.scope.super_kernel("sp1", ""):
            output, share_output, expert_token_nums = torch.ops.umdk_cam_op_lib.fused_deep_moe(
                x=x,
                expert_ids=expert_ids,
                gmm1_weight=self.gmm1_weight,
                gmm1_weight_scale=self.gmm1_weight_scale_fp32,
                gmm2_weight=self.gmm2_weight,
                gmm2_weight_scale=self.gmm2_weight_scale_bf16,
                expert_scales=expert_scales,
                share_gmm1_weight=self.share_gmm1_weight,
                share_gmm1_weight_scale=self.share_gmm1_weight_scale_fp32,
                share_gmm2_weight=self.share_gmm2_weight,
                share_gmm2_weight_scale=self.share_gmm2_weight_scale_bf16,
                expert_smooth_scales=smooth_scales,
                share_smooth_scales=share_smooth_scales_fp32,
                x_active_mask=x_active_mask,
                comm_args=self.comm_args,
                group_ep=self.ep_hcomm_info,
                ep_rank_size=self.ep_world_size,
                ep_rank_id=self.global_rank_id,
                moe_expert_num=self.moe_expert_num,
                shared_expert_num=1,
                shared_expert_rank_num=self.shared_expert_rank_num,
                quant_mode=0,
                global_bs=self.batch_size * self.ep_world_size)
        return (output, share_output, expert_token_nums)

    def _process_weights_after_loading(self, gmm1_weight, gmm1_weight_scale,
                                       gmm2_weight, gmm2_weight_scale,
                                       share_gmm1_weight=None, share_gmm1_weight_scale=None,
                                       share_gmm2_weight=None, share_gmm2_weight_scale=None):
        # Process routing expert weights (same as parent, but in list format)
        gmm1_weight = torch_npu.npu_format_cast(gmm1_weight,
                                                torch_npu.Format.FRACTAL_NZ)
        gmm2_weight = torch_npu.npu_format_cast(gmm2_weight,
                                                torch_npu.Format.FRACTAL_NZ)
        gmm1_weight_scale = gmm1_weight_scale.float()
        gmm2_weight_scale = gmm2_weight_scale.bfloat16()

        if self.dynamic_eplb:
            self.gmm1_weight = [
                weight.clone() for weight in gmm1_weight.unbind(dim=0)
            ]
            self.gmm1_weight_scale_fp32 = [
                weight.clone() for weight in gmm1_weight_scale.unbind(dim=0)
            ]
            self.gmm2_weight = [
                weight.clone() for weight in gmm2_weight.unbind(dim=0)
            ]
            self.gmm2_weight_scale_bf16 = [
                weight.clone() for weight in gmm2_weight_scale.unbind(dim=0)
            ]
        else:
            self.gmm1_weight = [gmm1_weight.clone()]
            self.gmm1_weight_scale_fp32 = [gmm1_weight_scale.clone()]
            self.gmm2_weight = [gmm2_weight.clone()]
            self.gmm2_weight_scale_bf16 = [gmm2_weight_scale.clone()]
        
        # Process shared expert weights for fusion op (similar to routing expert, but not in list)
        if self.with_share and share_gmm1_weight is not None:
            share_gmm1_weight = torch_npu.npu_format_cast(share_gmm1_weight, 29)
            share_gmm2_weight = torch_npu.npu_format_cast(share_gmm2_weight, 29)
            self.share_gmm1_weight_scale = share_gmm1_weight_scale
            share_gmm1_weight_scale = share_gmm1_weight_scale.float()
            share_gmm2_weight_scale = share_gmm2_weight_scale.bfloat16()
            
            self.share_gmm1_weight = share_gmm1_weight
            self.share_gmm1_weight_scale_fp32 = share_gmm1_weight_scale.float()
            self.share_gmm2_weight = share_gmm2_weight
            self.share_gmm2_weight_scale_bf16 = share_gmm2_weight_scale
            # self.share_gmm2_weight_scale = share_gmm2_weight_scale.float()
        else:
            self.share_gmm1_weight = None
            self.share_gmm1_weight_scale_fp32 = None
            self.share_gmm2_weight = None
            self.share_gmm2_weight_scale_bf16 = None

def generate_datas(batch_size,
                   token_hidden_size,
                   moe_intermediate_size,
                   ep_world_size,
                   moe_expert_num,
                   global_rank_id,
                   shared_expert_rank_num=0,
                   top_k=8,
                   test_bfloat16=True,
                   enable_dynamic_bs=False,
                   with_mc2_mask=False,
                   with_share=False,
                   with_smooth=False,
                   share_expert_intermediate_size=None):
    is_shared_expert = global_rank_id < shared_expert_rank_num
    moe_expert_num_per_rank = moe_expert_num // (ep_world_size - shared_expert_rank_num)
    actual_bs = int(
        torch.randint(2 if with_mc2_mask else 1, batch_size, [1]).item(
        ) if enable_dynamic_bs else batch_size)
    local_expert_num = 1 if is_shared_expert else moe_expert_num_per_rank
    gmm1_input_dim = token_hidden_size
    gmm1_output_dim = moe_intermediate_size * 2
    gmm2_input_dim = moe_intermediate_size
    gmm2_output_dim = token_hidden_size
    x = torch.rand([actual_bs, token_hidden_size]) * 10 - 5
    expert_ids = torch.arange(
        global_rank_id * batch_size * top_k,
        global_rank_id * batch_size * top_k + actual_bs * top_k).to(
            torch.int32).view(actual_bs, top_k)
    expert_ids = expert_ids % moe_expert_num
    if is_shared_expert:
        gmm1_weight = torch.ones([
            local_expert_num, gmm1_input_dim, gmm1_output_dim
        ]).to(torch.int8) * 4
        gmm2_weight = torch.ones([
            local_expert_num, gmm2_input_dim, gmm2_output_dim
        ]).to(torch.int8) * 4
        gmm1_weight[:, :, ::2] = gmm1_weight[:, :, ::2] * -1
        gmm2_weight[:, :, ::2] = gmm2_weight[:, :, ::2] * -1
        gmm1_weight_scale = torch.ones([local_expert_num, gmm1_output_dim
                                        ]) * 0.0015
        gmm2_weight_scale = torch.ones([local_expert_num, gmm2_output_dim
                                        ]) * 0.0015
    else:
        gmm1_weight = torch.randint(
            -16, 16,
            [local_expert_num, gmm1_input_dim, gmm1_output_dim]).to(torch.int8)
        gmm2_weight = torch.randint(
            -16, 16,
            [local_expert_num, gmm2_input_dim, gmm2_output_dim]).to(torch.int8)
        gmm1_weight_scale = torch.rand([local_expert_num, gmm1_output_dim
                                        ]) * 0.003 + 0.0015
        gmm2_weight_scale = torch.rand([local_expert_num, gmm2_output_dim
                                        ]) * 0.003 + 0.0015
    expert_scales = torch.rand(actual_bs, top_k)
    # Generate shared expert weights
    share_mm1_weight = None
    share_mm1_weight_scale = None
    share_mm2_weight = None
    share_mm2_weight_scale = None
    if with_share:
        # Use share_expert_intermediate_size for shared expert gmm1HLen
        share_gmm2_input_dim = share_expert_intermediate_size if share_expert_intermediate_size is not None else moe_intermediate_size
        share_gmm1_output_dim = share_gmm2_input_dim * 2
        share_mm1_weight = torch.ones([gmm1_input_dim, share_gmm1_output_dim]).to(torch.int8) * 4
        share_mm2_weight = torch.ones([share_gmm2_input_dim, gmm2_output_dim]).to(torch.int8) * 4
        share_mm1_weight_scale = torch.ones([share_gmm1_output_dim]) * 0.0015
        share_mm2_weight_scale = torch.ones([gmm2_output_dim]) * 0.0015
        share_mm1_weight[:, ::2] = share_mm1_weight[:, ::2] * -1
        share_mm2_weight[:, ::2] = share_mm2_weight[:, ::2] * -1
    if test_bfloat16:
        x = x.bfloat16()
        gmm1_weight_scale = gmm1_weight_scale.bfloat16()
        gmm2_weight_scale = gmm2_weight_scale.bfloat16()
        if with_share:
            share_mm1_weight_scale = share_mm1_weight_scale.bfloat16()
            share_mm2_weight_scale = share_mm2_weight_scale.bfloat16()
    else:
        x = x.half()
    smooth_sales = None
    share_smooth_scales = None
    if with_smooth:
        smooth_sales = torch.rand([moe_expert_num, token_hidden_size])
        share_smooth_scales = torch.rand([token_hidden_size], dtype=torch.bfloat16)
        share_smooth_scales_fp32 = share_smooth_scales.float()
    x_active_mask = None
    valid_token_num = actual_bs
    if with_mc2_mask:
        valid_token_num = actual_bs #int(torch.randint(1, actual_bs, [1]).item())
        x_active_mask = torch.cat(
            (torch.ones(valid_token_num),
             torch.zeros(actual_bs - valid_token_num))).bool()

    return (x, expert_ids, smooth_sales, share_smooth_scales, share_smooth_scales_fp32, expert_scales, x_active_mask), \
            (gmm1_weight, gmm1_weight_scale, gmm2_weight, gmm2_weight_scale), \
            (share_mm1_weight, share_mm1_weight_scale, share_mm2_weight, share_mm2_weight_scale), \
            actual_bs, valid_token_num


def run_once(local_rank_id,
             batch_size,
             token_hidden_size,
             moe_intermediate_size,
             ep_world_size,
             moe_expert_num,
             shared_expert_rank_num=0,
             top_k=8,
             test_bfloat16=True,
             enable_dynamic_bs=False,
             test_graph=False,
             with_mc2_mask=False,
             dynamic_eplb=False,
             with_share=False,
             with_smooth=False,
             share_expert_intermediate_size=None):
    ep_world_size = NODE_NUM * 16
    global_rank_id = local_rank_id + NODE_RANK * 16
    device_id = global_rank_id % 16
    torch_npu.npu.set_device(device_id)
    run_small = True
    init_small = True
    print(f"{global_rank_id=}")

    # 配置日志输出文件名
    log_file = redirect_output(f"local_rank_{local_rank_id}.log"
                               ) if output_to_file(local_rank_id) else None

    # 初始化分布式环境
    # os.environ["MASTER_ADDR"] = "127.0.0.1"
    # os.environ["MASTER_PORT"] = "29500"  # 端口号随意
    dist.init_process_group(backend="hccl",
                            rank=global_rank_id,
                            init_method=f"tcp://{MASTER_ADDR}:{MASTER_PORT}",
                            world_size=ep_world_size)
    ep_ranks_list = list(np.arange(0, ep_world_size))
    ep_group = dist.new_group(backend="hccl", ranks=ep_ranks_list)
    ep_group_small = dist.new_group(backend="hccl", ranks=ep_ranks_list)

    ep_hcomm_info_fused = ep_group._get_backend(
        torch.device("npu")).get_hccl_comm_name(global_rank_id)
    ep_hcomm_info_small = ep_group_small._get_backend(
        torch.device("npu")).get_hccl_comm_name(global_rank_id)
    commId = 0
    comm_args = umdk_cam_op_lib.cam_get_comm(commId, global_rank_id, ep_world_size, MASTER_ADDR + ":16007").npu()
    print(f"{comm_args.dtype=}, {comm_args.shape=}, {comm_args.view(-1)[256:].abs().sum()=}")
    torch_npu.npu.synchronize(device_id)

    # 构建必要参数和权重数据
    parameter = (batch_size, token_hidden_size, moe_intermediate_size,
                 ep_world_size, moe_expert_num, global_rank_id,
                 shared_expert_rank_num)
    input_datas, weight_datas, share_weight_datas, actual_bs, valid_token_num = generate_datas(
        *parameter, top_k, test_bfloat16, enable_dynamic_bs, with_mc2_mask, with_share, with_smooth, share_expert_intermediate_size)
    input_datas = [
        data.npu() if data is not None else None for data in input_datas
    ]
    weight_datas = [
        data.npu() if data is not None else None for data in weight_datas
    ]
    share_weight_datas = [
        data.npu() if data is not None else None for data in share_weight_datas
    ] if with_share else (None, None, None, None)

    print(f"input_datas:{[(_.shape, _.dtype) if _ is not None else None for _ in input_datas ]}")
    print(f"weight_datas:{[(_.shape, _.dtype) if _ is not None else None for _ in weight_datas ]}")
    print(f"share_weight_datas:{[(_.shape, _.dtype) if _ is not None else None for _ in share_weight_datas ]}")
    
    small_ops = SmallOps(*weight_datas, ep_hcomm_info_small,
                         *parameter, dynamic_eplb, with_share, share_expert_intermediate_size, *share_weight_datas).npu()  # type: ignore
    fused_ops = FusionOp(*weight_datas, ep_hcomm_info_fused,
                         *parameter, dynamic_eplb, with_share, share_expert_intermediate_size, *share_weight_datas, comm_args).npu()  # type: ignore
    if test_graph:
        config = torchair.CompilerConfig()
        # config.mode = "reduce-overhead"
        npu_backend = torchair.get_npu_backend(compiler_config=config)
        fused_ops = torch.compile(fused_ops, backend=npu_backend, dynamic=False)
        # small_ops = torch.compile(small_ops, backend=npu_backend, dynamic=False)
    for _ in range(100):
        fused_op_output = fused_ops(*input_datas)
    for _ in range(100):
        small_op_output = small_ops(*input_datas)
    torch_npu.npu.synchronize(device_id)
    # if global_rank_id == 0 or True:
    #     print(f"{fused_op_output[0]= }")
    #     print(f"{small_op_output[0]= }")
    #     # print(f"{fused_op_output[0][0:valid_token_num]= }")
    #     print(f"{fused_op_output[2]= }")
    #     print(f"{small_op_output[2]= }")
    #     # print(f"{fused_op_output= }")
    #     print(f"[rank-{global_rank_id}] MOE output diff: max={torch.abs(small_op_output[0] - fused_op_output[0]).max().item():.6f}, mean={torch.abs(small_op_output[0] - fused_op_output[0]).mean().item():.6f}")

    small_op_token_output, small_op_share_output, small_op_count_output = small_op_output
    fused_op_token_output, fused_op_share_output, fused_op_count_output = fused_op_output
    
    torch_npu.npu.synchronize(device_id)
    print(f"rank-{global_rank_id} End, {actual_bs= }, {valid_token_num= }")

    # 处理资源销毁
    dist.destroy_process_group()
    if log_file is not None:
        log_file.close()

    # torch.testing.assert_close(small_op_token_output.cpu(),
    #                            fused_op_token_output.cpu(),
    #                            atol=2.0,
    #                            rtol=0.02)
    # torch.testing.assert_close(small_op_count_output.cpu(),
    #                            fused_op_count_output.cpu())
    if with_share:
        print(f"[rank-{global_rank_id}] MOE output diff: max={torch.abs(small_op_token_output - fused_op_token_output).max().item():.6f}, mean={torch.abs(small_op_token_output - fused_op_token_output).mean().item():.6f}")
        print(f"[rank-{global_rank_id}] Share output diff: max={torch.abs(small_op_share_output - fused_op_share_output).max().item():.6f}, mean={torch.abs(small_op_share_output - fused_op_share_output).mean().item():.6f}")
        # torch.testing.assert_close(small_op_share_output.cpu(),
        #                            fused_op_share_output.cpu(),
        #                            atol=2.0,
        #                            rtol=0.02)
    gc.collect()
    torch.npu.empty_cache()
    torch.npu.reset_peak_memory_stats()


@torch.inference_mode()
def test_fused_deep_moe_base():
    custom_kwargs = BASE_KWARGS
    ep_world_size = custom_kwargs["ep_world_size"]
    custom_args = tuple(custom_kwargs.values())
    print(f"custom_kwargs: {custom_kwargs}")
    mp.spawn(run_once, args=custom_args, nprocs=ep_world_size, join=True)
    print(f"custom_kwargs: {custom_kwargs}")


@torch.inference_mode()
def test_fused_deep_moe_with_mc2_mask():
    custom_kwargs = BASE_KWARGS
    custom_kwargs["with_mc2_mask"] = True
    ep_world_size = custom_kwargs["ep_world_size"]
    custom_args = tuple(custom_kwargs.values())
    mp.spawn(run_once, args=custom_args, nprocs=ep_world_size, join=True)


@torch.inference_mode()
def test_fused_deep_moe_eplb():
    custom_kwargs = BASE_KWARGS
    custom_kwargs["dynamic_eplb"] = True
    ep_world_size = custom_kwargs["ep_world_size"]
    custom_args = tuple(custom_kwargs.values())
    mp.spawn(run_once, args=custom_args, nprocs=ep_world_size, join=True)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--share_expert_rank_num", type=int, default=0)
    parser.add_argument("--moe_expert_num", type=int, default=48)
    parser.add_argument("--batch_size", type=int, default=72)
    parser.add_argument("--token_hidden_size", type=int, default=5120)
    parser.add_argument("--moe_intermediate_size", type=int, default=1536)
    parser.add_argument("--share_expert_intermediate_size", type=int, default=3072)
    parser.add_argument("--ep_world_size", type=int, default=16)
    parser.add_argument("--shared_expert_rank_num", type=int, default=0)
    parser.add_argument("--top_k", type=int, default=6)
    parser.add_argument("--enable_dynamic_bs", action="store_true", default=False)
    parser.add_argument("--test_graph", action="store_true", default=True)
    parser.add_argument("--with_mc2_mask", action="store_true", default=True)
    parser.add_argument("--with_smooth", action="store_true", default=True)
    parser.add_argument("--dynamic_eplb", action="store_true", default=False)
    parser.add_argument("--with_share", action="store_true", default=True)
    args = parser.parse_args()
    BASE_KWARGS["shared_expert_rank_num"] = args.share_expert_rank_num
    BASE_KWARGS["moe_expert_num"] = args.moe_expert_num
    BASE_KWARGS["batch_size"] = args.batch_size
    BASE_KWARGS["token_hidden_size"] = args.token_hidden_size
    BASE_KWARGS["moe_intermediate_size"] = args.moe_intermediate_size
    BASE_KWARGS["share_expert_intermediate_size"] = args.share_expert_intermediate_size if args.share_expert_intermediate_size is not None else args.moe_intermediate_size
    BASE_KWARGS["ep_world_size"] = args.ep_world_size
    BASE_KWARGS["shared_expert_rank_num"] = args.shared_expert_rank_num
    BASE_KWARGS["top_k"] = args.top_k
    BASE_KWARGS["enable_dynamic_bs"] = args.enable_dynamic_bs
    BASE_KWARGS["test_graph"] = args.test_graph
    BASE_KWARGS["with_mc2_mask"] = args.with_mc2_mask
    BASE_KWARGS["dynamic_eplb"] = args.dynamic_eplb
    BASE_KWARGS["with_share"] = args.with_share
    BASE_KWARGS["with_smooth"] = args.with_smooth
    test_fused_deep_moe_base()
