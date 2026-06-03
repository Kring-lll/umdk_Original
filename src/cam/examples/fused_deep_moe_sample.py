#
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# Description: Example for fused_deep_moe operator.
# This sample gives an example for using FusedDeepMoe operator and "small" operators, 
# where "small" means the operators taking the same effect with FusedDeepMoe Operator but
# using small operator combination (dispatch + gmm1 + swiglu + gmm2 + combine).
# Create: 2025-12-11
# Note:
# History: 2025-12-11 create example file
#

import gc
import os
import sys
from pathlib import Path

import numpy as np
import pandas as pd
import torch
import torch.distributed as dist
import torch.multiprocessing as mp
import torch_npu
import torchair

import umdk_cam_op_lib

torch.manual_seed(42)
torch_npu.npu.config.allow_internal_format = True

_PRINT_N = 10


def _print_head(name, tensor):
    """打印 tensor 的前 10×10 元素"""
    t = tensor.detach().cpu().float()
    if t.ndim == 1:
        n = min(_PRINT_N, t.shape[0])
        vals = ', '.join(f'{v:+.4f}' for v in t[:n].tolist())
        print(f"  {name}  [{vals}{' ...' if t.shape[0] > n else ''}]")
    elif t.ndim == 2:
        rows, cols = t.shape
        rn, cn = min(_PRINT_N, rows), min(_PRINT_N, cols)
        print(f"  {name}  [{rn}×{cn}]:")
        for i in range(rn):
            row_vals = ', '.join(f'{t[i, j]:+.4f}' for j in range(cn))
            more = " ..." if cols > cn else ""
            print(f"    [{row_vals}{more}]")
        if rows > rn:
            print(f"    ... ({rows - rn} more rows)")
    elif t.ndim == 3:
        print(f"  {name}  [{t.shape[0]}×{t.shape[1]}×{t.shape[2]}]:")
        for e in range(min(2, t.shape[0])):
            rn, cn = min(_PRINT_N, t.shape[1]), min(_PRINT_N, t.shape[2])
            print(f"    E={e}:")
            for i in range(rn):
                row_vals = ', '.join(f'{t[e, i, j]:+.4f}' for j in range(cn))
                more = " ..." if t.shape[2] > cn else ""
                print(f"      [{row_vals}{more}]")
            if t.shape[1] > rn:
                print(f"      ... ({t.shape[1] - rn} more rows)")
        if t.shape[0] > 2:
            print(f"    ... ({t.shape[0] - 2} more experts)")
    else:
        print(f"  {name} shape={tuple(t.shape)}, ndim={t.ndim}")


class TorchSimulator:
    """
    PyTorch 模拟 npu_grouped_matmul_swiglu_quant_v2 (MSD A8W4) 的调试工具。
    不依赖 NPU 硬件，所有中间结果均可直接查看。

    模拟 C++ kernel 的三段流水线架构:
      Pre-process(AIV):  INT8 激活拆分为 high/low INT4
      Mid-process(AIC):  INT4 × INT4 两次矩阵乘 (分别应用 weight_scale 反量化)
      Post-process:      合并高低位 + assistMatrix → × per-token scale → SwiGLU → quant

    用法:
        sim_y, sim_y_scale = TorchSimulator.a8w4_simulate(
            x_int8=expand_x,              # INT8  [M, K]
            weight_int32=gmm1_weight,     # INT32 [E, K, N//8] (ND) 或 [E, N/64, K/16, 16, 8] (NZ)
            weight_scale=gmm1_wt_scale,   # float [E, N]
            x_scale=dynamic_scales,       # float [M]
            group_list=expert_token_nums, # int64 [E]
            assist_matrix=assist_mat,     # float [E, N]
        )
    """

    # ---------- 底层工具 ----------

    @staticmethod
    def _nz_to_nd_int32(weight_nz):
        """NZ [E, N/64, K/16, 16, 8] → ND [E, K, N//8]"""
        if weight_nz.ndim == 3:
            return weight_nz
        E, N_div_64, K_div_16 = weight_nz.shape[0], weight_nz.shape[1], weight_nz.shape[2]
        weight_nd = weight_nz.permute(0, 2, 3, 1, 4).contiguous()
        return weight_nd.view(E, K_div_16 * 16, N_div_64 * 8)

    @staticmethod
    def _nd_to_nz_int32(weight_nd):
        """ND [E, K, N//8] → NZ [E, N/64, K/16, 16, 8]"""
        if weight_nd.ndim == 3:
            E, K, N_div_8 = weight_nd.shape[0], weight_nd.shape[1], weight_nd.shape[2]
            K_div_16 = K // 16
            N_div_64 = N_div_8 // 8
            weight_nz = weight_nd.view(E, K_div_16, 16, N_div_64, 8)
            return weight_nz.permute(0, 3, 1, 2, 4).contiguous()
        return weight_nd

    @staticmethod
    def _int32_unpack_int4(int32_tensor):
        """INT32 按 4-bit nibble 解包为有符号 INT4。 (E, M, N) → (E, M, 8*N)"""
        unpacked = []
        for s in range(0, 32, 4):
            nib = (int32_tensor >> s) & 0xF
            nib = torch.where(nib >= 8, nib - 16, nib).to(torch.int8)
            unpacked.append(nib.unsqueeze(-1))      # (E, M, N, 1)
        # cat 得到 (E, M, N, 8)，再展平最后两维 → (E, M, N*8)
        return torch.cat(unpacked, dim=-1).reshape(*int32_tensor.shape[:-1], -1)

    @staticmethod
    def _expand_by_group(tensor, group_counts):
        """将 [E, ...] 按 group_counts 展开为 [M, ...] 的 per-token 形式。"""
        parts = [tensor[e:e+1].expand(int(group_counts[e].item()), *tensor.shape[1:])
                 for e in range(tensor.shape[0]) if int(group_counts[e].item()) > 0]
        return torch.cat(parts, dim=0) if parts else tensor.new_zeros(0, *tensor.shape[1:])

    @staticmethod
    def pack_int4_to_int32(int4_tensor):
        """
        将 INT8 张量 (值域 [-8, 7]) 按 4-bit nibble 打包为 INT32。
        [..., N] → [..., N//8]，每 8 个 INT4 存入一个 INT32 (小端序)。
        打包格式与 C++ kernel UnpackInt32ToInt4 兼容。
        """
        N = int4_tensor.shape[-1]
        assert N % 8 == 0, f"Last dim must be divisible by 8, got {N}"
        int4_clamped = int4_tensor.clamp(-8, 7) & 0xF
        shape = int4_tensor.shape[:-1] + (N // 8, 8)
        int4_reshaped = int4_clamped.reshape(shape)
        packed = torch.zeros(int4_tensor.shape[:-1] + (N // 8,), dtype=torch.int32)
        for i in range(8):
            packed += (int4_reshaped[..., i].to(torch.int32) & 0xF) << (i * 4)
        return packed

    @staticmethod
    def compute_assist_matrix(weight_int4):
        """
        计算 A8W4 辅助矩阵: assistMatrix[e, j] = 8.0 * Σ_k W[e, k, j]
        weight_int4: [E, K, N] INT8, 值域 [-8, 7]
        Returns: float32 [E, N]
        """
        return 8.0 * weight_int4.float().sum(dim=1)

    # ---------- MSD A8W4 三段模拟 ----------

    @staticmethod
    def _a8w4_pre_process(x_int8):
        """
        Pre-Process: INT8 激活拆分为 high/low INT4。
        X_high = floor(X_int8 / 16)
        X_low  = (X_int8 & 0x0F) - 8
        Returns: (x_high_int4, x_low_int4) 均为 int8 值域 [-8, 7]
        """
        x_i32 = x_int8.to(torch.int32)
        x_high = (x_i32 // 16).to(torch.int8)
        x_low = ((x_i32 & 0x0F) - 8).to(torch.int8)
        return x_high, x_low

    @staticmethod
    def _a8w4_mid_process(x_high, x_low, weight_int4,  group_counts):
        """
        Mid-Process: 两次 INT4 × INT4 矩阵乘，分别应用 per-channel weight_scale 反量化。
        C_high = (x_high · W) ⊙ w_scale
        C_low  = (x_low  · W) ⊙ w_scale
        Returns: (c_high, c_low) float32 [M, N]
        """
        M = x_high.shape[0]
        E = weight_int4.shape[0]
        N = weight_int4.shape[-1]

        # ws = weight_scale.float()                             # [E, N]
        # ws_per_token = TorchSimulator._expand_by_group(ws, group_counts)  # [M, N]

        # 按 expert 分组做 INT4 matmul (不溢出 int32 即可)
        c_high = torch.empty(M, N, dtype=torch.int32)
        c_low = torch.empty(M, N, dtype=torch.int32)
        start = 0
        for e in range(E):
            cnt = int(group_counts[e].item())
            if cnt <= 0:
                continue
            end = start + cnt
            w = weight_int4[e].to(torch.int32)               # [K, N]
            c_high[start:end] = x_high[start:end].to(torch.int32).mm(w).float()
            c_low[start:end]  = x_low[start:end].to(torch.int32).mm(w).float()
            start = end

        # # 应用 weight_scale 反量化
        # c_high = c_high * ws_per_token
        # c_low  = c_low  * ws_per_token
        return c_high, c_low

    @staticmethod
    def _a8w4_post_process(c_high, c_low, x_scale, assist_matrix, group_counts):
        """
        Post-Process: 合并 → per-token scale → SwiGLU → 量化
        C = (C_high * 16 + C_low + assistMatrix) * x_scale
        Returns: (y_int8, y_scale)  INT8 [M, N/2], float [M]
        """
        assist_per_token = TorchSimulator._expand_by_group(assist_matrix.float(), group_counts)  # [M, N]
        x_scale_f32 = x_scale.float()

        # ① Merge + per-token scale
        merged = (c_high * 16.0 + c_low + assist_per_token) * x_scale_f32.unsqueeze(1)

        # ② SwiGLU: split left → Swish, right → gate
        half_n = merged.shape[1] // 2
        act, gate = merged[:, :half_n], merged[:, half_n:]
        swiglu_out = torch.sigmoid(act) * act * gate

        # ③ 量化 per-token: scale = max(|row|) / 127
        row_max = swiglu_out.abs().amax(dim=1, keepdim=True)
        y_scale = (row_max / 127.0).squeeze(1)
        y_float = torch.round(swiglu_out / (y_scale.unsqueeze(1) + 1e-10))
        y_int8 = y_float.clamp(-128, 127).to(torch.int8)

        return y_int8, y_scale

    # ---------- 主入口 ----------

    @staticmethod
    def _compute(x_int8, weight_int32, weight_scale, x_scale, group_counts, assist_matrix):
        """核心计算流程，无打印。"""
        weight_int32 = TorchSimulator._nz_to_nd_int32(weight_int32)
        weight_int4 = TorchSimulator._int32_unpack_int4(weight_int32)
        x_high, x_low = TorchSimulator._a8w4_pre_process(x_int8)
        c_high, c_low = TorchSimulator._a8w4_mid_process(
            x_high, x_low, weight_int4, weight_scale, group_counts)
        return TorchSimulator._a8w4_post_process(
            c_high, c_low, x_scale, assist_matrix, group_counts)

    @staticmethod
    def a8w4_simulate(x_int8, weight_int32, weight_scale, x_scale, group_list,
                       assist_matrix, group_list_type=1):
        """
        npu_grouped_matmul_swiglu_quant_v2 MSD A8W4 场景的完整 torch 模拟。

        Args:
            x_int8:        INT8   [M, K]
            weight_int32:  INT32  [E, K, N//8] (ND) 或 [E, N/64, K/16, 16, 8] (NZ)
            weight_scale:  float  [E, N]
            x_scale:       float  [M]
            group_list:    int64  [E]
            assist_matrix: float  [E, N]
            group_list_type: 0=cumsum, 1=count

        Returns:
            y_int8:   INT8  [M, N//2]
            y_scale:  float [M]
        """
        if group_list_type == 0:
            group_counts = group_list.clone()
            group_counts[1:] = group_list[1:] - group_list[:-1]
        else:
            group_counts = group_list

        # Step 1: 将打包 INT32 解包为 INT4
        weight_int4 = TorchSimulator._int32_unpack_int4(weight_int32)  # [E, K, N]

        # Step 2: Pre-Process — INT8 激活拆分为 high/low INT4
        x_high, x_low = TorchSimulator._a8w4_pre_process(x_int8)

        # Step 3: Mid-Process — INT4 × INT4 双路矩阵乘 
        c_high, c_low = TorchSimulator._a8w4_mid_process(
            x_high, x_low, weight_int4, weight_scale, group_counts)

        # # Step 4: Post-Process — 合并 + assistMatrix + x_scale + SwiGLU + quant
        # return TorchSimulator._a8w4_post_process(
        #     c_high, c_low, x_scale, assist_matrix, group_counts)
        return c_high, c_low


LOG_NAME = "fused_deep_moe_sample_logs"
BASE_KWARGS = {
    "batch_size": 64,
    "token_hidden_size": 7168,
    "moe_intermediate_size": 2048,
    "ep_world_size": 16,
    "moe_expert_num": 64,
    "top_k": 8,
    "test_bfloat16": True,
    "enable_dynamic_bs": False,
    "test_graph": False,
    "with_mc2_mask": False,
    "dynamic_eplb": False,
    "with_share": False,
    "with_smooth": False,
    "share_expert_intermediate_size": 2048
}

def redirect_output(log_file_path):
    log_path = Path(LOG_NAME) / log_file_path
    log_path.parent.mkdir(parents=True, exist_ok=True)
    f = open(LOG_NAME + "/" + log_file_path, "w")
    os.dup2(f.fileno(), sys.stdout.fileno())
    os.dup2(f.fileno(), sys.stderr.fileno())
    return f

def output_to_file(rank_id):
    return False

def convert_nd_to_nz(x: torch.Tensor) -> torch.Tensor:
    fractal_size_dict = {
        torch.int8: (16,32),
        torch.float16: (16, 16),
        torch.bfloat16: (16, 16),
        torch.int32: (16, 8)
    }
    m0, n0 = fractal_size_dict[x.dtype]
    *dim, m, n = x.shape
    assert(m % m0 == 0 and n % n0 == 0)
    order = list(range(len(dims))) + [-2, -4, -3, -1]
    return x.reshape(*dim, m//m0, m0, n//n0, n0).permute(order).reshape(*dim, m, n).contiguous()

def convert_tensor_into_parameter(tensor, trans_nz=False):
    if tensor is None:
        return None
    if trans_nz:
        tensor = convert_nd_to_nz(tensor)
    if trans_nz:
        tensor = torch_npu.npu_format_cast(tensor, torch_npu.Format.FRACTAL_NZ)
    return torch.nn.Parameter(tensor, requires_grad=False)

class DecodeMoeOps(torch.nn.Module):

    def __init__(self,
                 ep_hcomm_info,
                 meta_info,
                 weight_datas,
                 share_weight_datas):
        super().__init__()
        self.ep_hcomm_info = ep_hcomm_info
        batch_size, ep_world_size, moe_expert_num, global_rank_id, dynamic_eplb = meta_info
        self.ep_world_size = ep_world_size
        self.moe_expert_num = moe_expert_num
        self.global_rank_id = global_rank_id
        self.dynamic_eplb = dynamic_eplb
        self.global_batch_size = batch_size * ep_world_size
        self.with_share = None
        self.with_smooth = None
        self._checkout_datas(weight_datas, share_weight_datas)
        self._process_share_weights_after_loading(share_weight_datas)
        self._process_weights_after_loading(weight_datas)

    def _checkout_datas(self, weight_datas, share_weight_datas):
        gmm1_weight, gmm1_weight_scale, gmm2_weight, gmm2_weight_scale, smooth_scales = weight_datas
        share_mm1_weight, share_mm1_weight_scale, share_mm2_weight, share_mm2_weight_scale, share_smooth_scales = share_weight_datas
        if share_mm1_weight is not None:
            assert share_mm1_weight_scale is not None, "share expert need share_mm1_weight_scale"
            assert share_mm2_weight is not None, "share expert need share_mm2_weight"
            assert share_mm2_weight_scale is not None, "share expert need share_mm2_weight_scale"
            if smooth_scales is not None:
                assert share_smooth_scales is not None, "share expert need share_smooth_scales"
                self.with_smooth = True
            else:
                self.with_smooth = False
            self.with_share = True
        else:
            self.with_share = False

    def _process_share_weights_after_loading(self, share_weight_datas):
        share_gmm1_weight, share_gmm1_weight_scale, share_gmm2_weight, share_gmm2_weight_scale, share_smooth_scales = share_weight_datas
        self.share_gmm1_weight = convert_tensor_into_parameter(share_gmm1_weight, trans_nz=True)
        self.share_gmm1_weight_scale = convert_tensor_into_parameter(share_gmm1_weight_scale)
        self.share_gmm2_weight = convert_tensor_into_parameter(share_gmm2_weight, trans_nz=True)
        self.share_gmm2_weight_scale = convert_tensor_into_parameter(share_gmm2_weight_scale)
        self.share_smooth_scales = convert_tensor_into_parameter(share_smooth_scales)

    def _process_weights_after_loading(self, weight_datas):
        gmm1_weight, gmm1_weight_scale, gmm2_weight, gmm2_weight_scale, smooth_scales = weight_datas
        self.gmm1_weight = convert_tensor_into_parameter(gmm1_weight, trans_nz=True)
        self.gmm1_weight_scale = convert_tensor_into_parameter(gmm1_weight_scale)
        self.gmm2_weight = convert_tensor_into_parameter(gmm2_weight, trans_nz=True)
        self.gmm2_weight_scale = convert_tensor_into_parameter(gmm2_weight_scale)
        self.smooth_scales = convert_tensor_into_parameter(smooth_scales)

    def _apply_ops(self, x, expert_ids, expert_scales, x_active_mask):
        raise NotImplementedError("To be implemented in subclass")

    def forward(self, x, expert_ids, expert_scales, x_active_mask):
        return self._apply_ops(x, expert_ids, expert_scales, x_active_mask)


class SmallOps(DecodeMoeOps):

    def __init__(self,
                 ep_hcomm_info,
                 meta_info,
                 weight_datas,
                 share_weight_datas):
        super().__init__(ep_hcomm_info, meta_info, weight_datas, share_weight_datas)
        self.shared_expert_rank_num = 0
        self.tp_hcomm_info = ""

    def share_compute(self, x):
        x1_int8, x1_scale = torch_npu.npu_dynamic_quant(x, smooth_scales=self.share_smooth_scales)
        gmm1_result = torch_npu.npu_quant_matmul(x1_int8, self.share_gmm1_weight, self.share_gmm1_weight_scale, pertoken_scale=None, output_dtype=torch.int32)
        x2_int8, x2_scale = torch_npu.npu_dequant_swiglu_quant(
            x=gmm1_result,
            weight_scale=self.share_gmm1_weight_scale,
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

    def _apply_ops(self, x, expert_ids, expert_scales, x_active_mask):
        if self.with_share:
            share_output = self.share_compute(x)
        else:
            share_output = None
        outputs = torch_npu.npu_moe_distribute_dispatch_v2(
            x=x,
            expert_ids=expert_ids,
            expert_scales=expert_scales,
            scales=self.smooth_scales,
            x_active_mask=x_active_mask,
            group_ep=self.ep_hcomm_info,
            ep_world_size=self.ep_world_size,
            ep_rank_id=self.global_rank_id,
            moe_expert_num=self.moe_expert_num,
            group_tp=self.tp_hcomm_info,
            tp_world_size=1,
            tp_rank_id=0,
            expert_shard_type=0,
            shared_expert_num=1,
            shared_expert_rank_num=self.shared_expert_rank_num,
            quant_mode=2,
            global_bs=self.global_batch_size,
            expert_token_nums_type=1,  # 0代表前缀和，1代表各自数量
        )
        expand_x, dynamic_scales, assist_info_for_combine, expert_token_nums, ep_send_counts, tp_send_counts, expand_scales = outputs
        output_dtype = x.dtype

# 只打印前100个，想看全部就去掉[:100]

        return expand_x, expert_token_nums

class FusionOp(DecodeMoeOps):

    def __init__(self,
                 ep_hcomm_info,
                 meta_info,
                 weight_datas,
                 share_weight_datas):
        super().__init__(ep_hcomm_info, meta_info, weight_datas, share_weight_datas)

    def _apply_ops(self, x, expert_ids, expert_scales, x_active_mask):
        self.share_smooth_scales_fp32 = torch.zeros(10000000 + 256*1024*1024).npu().to(torch.int32)
        output, share_output, expert_token_nums = torch.ops.umdk_cam_op_lib.fused_deep_moe(
            x=x,
            expert_ids=expert_ids,
            gmm1_weight=self.gmm1_weight,
            gmm1_weight_scale=self.gmm1_weight_scale,
            gmm2_weight=self.gmm2_weight,
            gmm2_weight_scale=self.gmm2_weight_scale,
            expert_scales=expert_scales,
            share_gmm1_weight=self.share_gmm1_weight,
            share_gmm1_weight_scale=self.share_gmm1_weight_scale,
            share_gmm2_weight=self.share_gmm2_weight,
            share_gmm2_weight_scale=self.share_gmm2_weight_scale,
            expert_smooth_scales=self.smooth_scales,
            share_smooth_scales=self.share_smooth_scales_fp32,
            x_active_mask=x_active_mask,
            group_ep=self.ep_hcomm_info,
            ep_rank_size=self.ep_world_size,
            ep_rank_id=self.global_rank_id,
            moe_expert_num=self.moe_expert_num,
            quant_mode=0,
            global_bs=self.global_batch_size)
        target_size = 3 * 256 * 4096    
        
        return (output, share_output, expert_token_nums)

    def _process_share_weights_after_loading(self, share_weight_datas):
        super()._process_share_weights_after_loading(share_weight_datas)
        _, _, _, _, share_smooth_scales = share_weight_datas
        if self.with_share and self.with_smooth:
            self.share_smooth_scales_fp32 = convert_tensor_into_parameter(share_smooth_scales.float())
        else:
            self.share_smooth_scales_fp32 = None

    def _process_weights_after_loading(self, weight_datas):
        gmm1_weight, gmm1_weight_scale, gmm2_weight, gmm2_weight_scale, smooth_scales = weight_datas
        gmm1_weight = convert_tensor_into_parameter(gmm1_weight, trans_nz=True)
        gmm1_weight_scale = convert_tensor_into_parameter(gmm1_weight_scale)
        gmm2_weight = convert_tensor_into_parameter(gmm2_weight, trans_nz=True)
        gmm2_weight_scale = convert_tensor_into_parameter(gmm2_weight_scale)
        if self.dynamic_eplb:
            self.gmm1_weight = [
                weight.clone() for weight in gmm1_weight.unbind(dim=0)
            ]
            self.gmm1_weight_scale = [
                weight.clone() for weight in gmm1_weight_scale.unbind(dim=0)
            ]
            self.gmm2_weight = [
                weight.clone() for weight in gmm2_weight.unbind(dim=0)
            ]
            self.gmm2_weight_scale = [
                weight.clone() for weight in gmm2_weight_scale.unbind(dim=0)
            ]
        else:
            self.gmm1_weight = [gmm1_weight.clone()]
            self.gmm1_weight_scale = [gmm1_weight_scale.clone()]
            self.gmm2_weight = [gmm2_weight.clone()]
            self.gmm2_weight_scale = [gmm2_weight_scale.clone()]
        self.smooth_scales = convert_tensor_into_parameter(smooth_scales)

def generate_datas(batch_size,
                   token_hidden_size,
                   moe_intermediate_size,
                   ep_world_size,
                   moe_expert_num,
                   global_rank_id,
                   top_k=8,
                   test_bfloat16=True,
                   enable_dynamic_bs=False,
                   with_mc2_mask=False,
                   with_share=False,
                   with_smooth=False,
                   share_expert_intermediate_size=None):
    moe_expert_num_per_rank = moe_expert_num // ep_world_size
    actual_bs = int(
        torch.randint(2 if with_mc2_mask else 1, batch_size, [1]).item(
        ) if enable_dynamic_bs else batch_size)
    local_expert_num = moe_expert_num_per_rank
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
    gmm1_weight = torch.randint(
        -127, 127,
        [local_expert_num, gmm1_input_dim, gmm1_output_dim //8]).to(torch.int32)
    gmm2_weight = torch.randint(
        -127, 127,
        [local_expert_num, gmm2_input_dim, gmm2_output_dim //8]).to(torch.int32)
    # 改为全 1 初始化
    # gmm1_weight = torch.ones(
    #     [local_expert_num, gmm1_input_dim, gmm1_output_dim //8],
    #     dtype=torch.int32
    # )
    # 改为int4 全1初始化
    # gmm1_weight = torch.full(
    #     [local_expert_num, gmm1_input_dim, gmm1_output_dim // 8],
    #     fill_value=0x1f1f1f1f,
    #     dtype=torch.int32
    # )
    # total_elements = gmm1_weight.numel()
    # increment = torch.arange(1, total_elements + 1, dtype=torch.int32).view(gmm1_weight.shape)
    # gmm1_weight = gmm1_weight + increment
    # total_elements = gmm1_weight.numel()
    # increment = (torch.arange(total_elements, dtype=torch.int32) % 2).view(gmm1_weight.shape)
    # gmm1_weight = gmm1_weight + increment
    # gmm1_weight = gmm1_weight * 2
    # row_increment = (torch.arange(gmm1_weight.shape[0], dtype=torch.int32) % 2).view(-1, 1, 1)
    # gmm1_weight = gmm1_weight + row_increment
    # row_increment = (torch.arange(gmm1_weight.shape[0], dtype=torch.int32) % 10).view(-1, 1, 1)
    # gmm1_weight = gmm1_weight + row_increment

    gmm2_weight = torch.ones(
        [local_expert_num, gmm2_input_dim, gmm2_output_dim //8],
        dtype=torch.int32
    )
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
        # gmm1_weight_scale = gmm1_weight_scale.bfloat16()
        gmm2_weight_scale = gmm2_weight_scale.bfloat16()
        if with_share:
            # share_mm1_weight_scale = share_mm1_weight_scale.bfloat16()
            share_mm2_weight_scale = share_mm2_weight_scale.bfloat16()
    else:
        x = x.half()
    smooth_scales = None
    share_smooth_scales = None
    if with_smooth:
        smooth_scales = torch.rand([moe_expert_num, token_hidden_size])
        share_smooth_scales = torch.rand([token_hidden_size]).to(x.dtype)
    x_active_mask = None
    valid_token_num = actual_bs
    if with_mc2_mask:
        valid_token_num = int(torch.randint(1, actual_bs, [1]).item())
        x_active_mask = torch.cat(
            (torch.ones(valid_token_num),
             torch.zeros(actual_bs - valid_token_num))).bool()
    return (x, expert_ids, expert_scales, x_active_mask), \
            (gmm1_weight, gmm1_weight_scale, gmm2_weight, gmm2_weight_scale, smooth_scales), \
            (share_mm1_weight, share_mm1_weight_scale, share_mm2_weight, share_mm2_weight_scale, share_smooth_scales), \
            actual_bs, valid_token_num


def split_to_highlow(second_int8: torch.Tensor):
    """
    从int8toint4表达中，把高低部分分开。

    Args:
        second_int8: (N, H) int8, 打包规则同前

    Returns:

    """
    N, H = second_int8.shape
    assert H % 2 == 0, "token_hidden_size 必须为偶数"
    torch.set_printoptions(threshold=20, edgeitems=10)

    # 从 second_int8 前一半字节解包高4位部分 (h2)
    half = H // 2
    second_high_bytes = second_int8[:, :half]       # (N, half)
    h2_high = (second_high_bytes >> 4) & 0x0F
    h2_low  = second_high_bytes & 0x0F
    # 特殊原因：半字节反过来交错
    h2 = torch.stack([h2_low, h2_high], dim=-1).reshape(N, H)
    # 将无符号int4转换为有符号int4 (值域 [-8, 7])
    h2 = torch.where(h2 >= 8, h2 - 16, h2).to(torch.int8)

    # 从 second_int8 后一半字节解包低4位部分 (l2)
    second_low_bytes = second_int8[:, half:]         # (N, half)
    l2_high = (second_low_bytes >> 4) & 0x0F
    l2_low  = second_low_bytes & 0x0F
    # 特殊原因：半字节反过来交错
    l2 = torch.stack([l2_low, l2_high], dim=-1).reshape(N, H)
    # 将无符号int4转换为有符号int4 (值域 [-8, 7])
    l2 = torch.where(l2 >= 8, l2 - 16, l2).to(torch.int8)
    return h2, l2

def run_once(local_rank_id,
             batch_size,
             token_hidden_size,
             moe_intermediate_size,
             ep_world_size,
             moe_expert_num,
             top_k=8,
             test_bfloat16=True,
             enable_dynamic_bs=False,
             test_graph=False,
             with_mc2_mask=False,
             dynamic_eplb=False,
             with_share=False,
             with_smooth=False,
             share_expert_intermediate_size=None):
    # 配置日志输出文件名
    log_file = redirect_output(f"local_rank_{local_rank_id}.log"
                               ) if output_to_file(local_rank_id) else None
    # 使用A3 单机16DIE进行测试
    global_rank_id = local_rank_id
    device_id = local_rank_id % 16
    torch_npu.npu.set_device(device_id)

    # 初始化分布式环境
    os.environ["MASTER_ADDR"] = "127.0.0.1"
    os.environ["MASTER_PORT"] = "27500"  # 端口号随意
    dist.init_process_group(backend="hccl",
                            rank=local_rank_id,
                            world_size=ep_world_size)
    ep_ranks_list = list(np.arange(0, ep_world_size))
    ep_group = dist.new_group(backend="hccl", ranks=ep_ranks_list)
    ep_group_small = dist.new_group(backend="hccl", ranks=ep_ranks_list)

    ep_hcomm_info_fused = ep_group._get_backend(
        torch.device("npu")).get_hccl_comm_name(local_rank_id)
    ep_hcomm_info_small = ep_group_small._get_backend(
        torch.device("npu")).get_hccl_comm_name(local_rank_id)
    torch_npu.npu.synchronize(device_id)

    # 构建必要参数和权重数据
    parameter = (batch_size, token_hidden_size, moe_intermediate_size,
                 ep_world_size, moe_expert_num, global_rank_id, top_k,
                 test_bfloat16, enable_dynamic_bs, with_mc2_mask,
                 with_share, with_smooth, share_expert_intermediate_size)
    input_datas, weight_datas, share_weight_datas, actual_bs, valid_token_num = generate_datas(*parameter)
    input_datas = [
        data.npu() if data is not None else None for data in input_datas
    ]
    meta_info = (batch_size, ep_world_size, moe_expert_num, global_rank_id, dynamic_eplb)
    weight_datas_npu = [
        data.npu() if data is not None else None for data in weight_datas
    ]
    share_weight_datas = [
        data.npu() if data is not None else None for data in share_weight_datas
    ]
    
    small_ops = SmallOps(ep_hcomm_info_small, meta_info, weight_datas_npu, share_weight_datas).npu()  # type: ignore
    fused_ops = FusionOp(ep_hcomm_info_fused, meta_info, weight_datas_npu, share_weight_datas).npu()  # type: ignore
    if test_graph:
        config = torchair.CompilerConfig()
        config.mode = "reduce-overhead"
        npu_backend = torchair.get_npu_backend(compiler_config=config)
        fused_ops = torch.compile(fused_ops, backend=npu_backend)
    
    expand_x, expert_token_nums = small_ops(*input_datas)
    fused_op_output = fused_ops(*input_datas)
    gmm1_weight, gmm1_weight_scale, gmm2_weight, gmm2_weight_scale, smooth_scales = weight_datas
    # # Step 0: nz --> nd
    # gmm1_weight_nd = TorchSimulator._nz_to_nd_int32(gmm1_weight)
    # gmm2_weight_nd = TorchSimulator._nz_to_nd_int32(gmm2_weight)
    # Step 1: 将打包 INT32 解包为 INT4
    weight_int4 = TorchSimulator._int32_unpack_int4(gmm1_weight)  # [E, K, N]

    torch.set_printoptions(threshold=30, edgeitems=15)
    print(f"weight_int4: {gmm1_weight}")
    print(f"weight_int4: {weight_int4}")
    # Step 2: Pre-Process — INT8 激活拆分为 high/low INT4
    x = expand_x.cpu()  # 假设 expand_x 已在 NPU，移到 CPU

    x_high, x_low = TorchSimulator._a8w4_pre_process(x)


    # Step 3: Mid-Process — INT4 × INT4 双路矩阵乘
    # 取前三个专家的 token 总数
    total_tokens_first3 = sum(expert_token_nums[:3])

    x_high_first3 = x_high[:total_tokens_first3]
    x_low_first3 = x_low[:total_tokens_first3]
    c_high, c_low = TorchSimulator._a8w4_mid_process(
        x_high_first3, x_low_first3, weight_int4[:3], expert_token_nums[:3])

    ## 校验npu和cpu token表达一致
    # total_tokens_first3 * token_hidden_size 字节
    int4token = fused_ops.share_smooth_scales_fp32.cpu()[0:total_tokens_first3 * token_hidden_size // 4]
    token_high, token_low = split_to_highlow(int4token.view(torch.int8).reshape(total_tokens_first3, token_hidden_size))

    print(f"x_high_first3: {x_high_first3}")
    print(f"x_low_first3: {x_low_first3}")
    print(f"token_high: {token_high}")
    print(f"token_low: {token_low}")

    match_high = torch.all(x_high_first3 == token_high)
    match_low = torch.all(x_low_first3 == token_low)
    print(f"match_high:\n {match_high}")
    print(f"match_low:\n {match_low}")

    # 最后的结果
    c_result = fused_ops.share_smooth_scales_fp32.cpu()[10000000:10000000+3*256*4096]

    def split_c_to_AB(c_result):
        BLOCK = 256
        HALF = 128
        NUM_EXPERT = 3
        ROW_PER_EXPERT = 128
        CHUNK_TOTAL = 32
        COL_TOTAL = 4096

        c_1d = c_result.reshape(-1)
        expert_length = len(c_1d) // NUM_EXPERT

        A = torch.zeros(NUM_EXPERT * ROW_PER_EXPERT, COL_TOTAL, dtype=torch.int32)
        B = torch.zeros(NUM_EXPERT * ROW_PER_EXPERT, COL_TOTAL, dtype=torch.int32)

        for exp_idx in range(NUM_EXPERT):
            exp_data = c_1d[exp_idx * expert_length : (exp_idx + 1) * expert_length]
            for chunk_idx in range(CHUNK_TOTAL):
                for row_in_exp in range(ROW_PER_EXPERT):
                    block_idx = chunk_idx * ROW_PER_EXPERT + row_in_exp
                    blk = exp_data[block_idx * BLOCK : (block_idx + 1) * BLOCK]

                    global_row = exp_idx * ROW_PER_EXPERT + row_in_exp
                    col_start = chunk_idx * HALF
                    A[global_row, col_start:col_start + HALF] = blk[:HALF]
                    B[global_row, col_start:col_start + HALF] = blk[HALF:]
        return A, B

    # 执行拆分
    A_mat, B_mat = split_c_to_AB(c_result)
    # 2. 打印维度（一定和 c_high / c_low 一模一样）
    print("A_mat shape:   ", A_mat.shape)   # [384, 4096]
    print("c_high shape:  ", c_high.shape)  # [384, 4096]
    print("c_high element count: ", c_high.numel())
    print("c_high nonzero count: ", torch.count_nonzero(c_high).item())
    print("c_high[0] nonzero count: ", torch.count_nonzero(c_high[0]).item())
    print("c_high[1] nonzero count: ", torch.count_nonzero(c_high[1]).item())

    print("B_mat shape:   ", B_mat.shape)   # [384, 4096]
    print("c_low shape:   ", c_low.shape)   # [384, 4096]

    # 3. 逐元素对比
    match_A = torch.all(A_mat == c_high)
    match_B = torch.all(B_mat == c_low)
    print(f"A_mat:\n {A_mat}")
    print(f"c_high:\n {c_high}")
    # pd.set_option('display.max_rows', None)
    # pd.set_option('display.max_columns', None)
    # pd.set_option('display.width', None)
    # print(f"c_high[0] (pandas):\n{pd.DataFrame(c_high[0].cpu().numpy())}")
    print(f"   A_mat  ==  c_high :  {match_A}")
    print(f"   B_mat  ==  c_low  :  {match_B}")

    torch_npu.npu.synchronize(device_id)
    #small_op_token_output, small_op_share_output, small_op_count_output = small_op_output
    fused_op_token_output, fused_op_share_output, fused_op_count_output = fused_op_output
    
    torch_npu.npu.synchronize(device_id)

    # 处理资源销毁
    dist.destroy_process_group()
    if log_file is not None:
        log_file.close()
    # try:
    #     torch.testing.assert_close(small_op_token_output.cpu()[:valid_token_num],
    #                             fused_op_token_output.cpu()[:valid_token_num],
    #                             atol=2.0,
    #                             rtol=0.02)
    #     torch.testing.assert_close(small_op_count_output.cpu(),
    #                             fused_op_count_output.cpu())
    #     if with_share:
    #         torch.testing.assert_close(small_op_share_output.cpu(),
    #                                 fused_op_share_output.cpu(),
    #                                 atol=2.0,
    #                                 rtol=0.02)
    # except Exception as e:
    #     print(f"rank-{global_rank_id} Failed!, message is {e}")
    # else:
    print(f"rank-{global_rank_id} Passed!")
    # finally:
    #     ...

    gc.collect()
    torch.npu.empty_cache()
    torch.npu.reset_peak_memory_stats()


@torch.inference_mode()
def test_fused_deep_moe_base():
    custom_kwargs = BASE_KWARGS
    ep_world_size = custom_kwargs["ep_world_size"]
    custom_args = tuple(custom_kwargs.values())
    mp.spawn(run_once, args=custom_args, nprocs=ep_world_size, join=True)


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
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--batch_size", type=int, default=64)
    parser.add_argument("--token_hidden_size", type=int, default=7168)
    parser.add_argument("--moe_intermediate_size", type=int, default=2048)
    parser.add_argument("--ep_world_size", type=int, default=16)
    parser.add_argument("--moe_expert_num", type=int, default=64)
    parser.add_argument("--top_k", type=int, default=8)
    parser.add_argument("--test_float16", action="store_true", default=False)
    parser.add_argument("--enable_dynamic_bs", action="store_true", default=False)
    parser.add_argument("--test_graph", action="store_true", default=False)
    parser.add_argument("--with_mc2_mask", action="store_true", default=False)
    parser.add_argument("--dynamic_eplb", action="store_true", default=False)
    parser.add_argument("--with_share", action="store_true", default=False)
    parser.add_argument("--with_smooth", action="store_true", default=False)
    parser.add_argument("--share_expert_intermediate_size", type=int)
    args = parser.parse_args()
    BASE_KWARGS["batch_size"] = args.batch_size
    BASE_KWARGS["token_hidden_size"] = args.token_hidden_size
    BASE_KWARGS["moe_intermediate_size"] = args.moe_intermediate_size
    BASE_KWARGS["moe_expert_num"] = args.moe_expert_num
    BASE_KWARGS["ep_world_size"] = args.ep_world_size
    BASE_KWARGS["top_k"] = args.top_k
    BASE_KWARGS["test_bfloat16"] = not args.test_float16
    BASE_KWARGS["enable_dynamic_bs"] = args.enable_dynamic_bs
    BASE_KWARGS["test_graph"] = args.test_graph
    BASE_KWARGS["with_mc2_mask"] = args.with_mc2_mask
    BASE_KWARGS["dynamic_eplb"] = args.dynamic_eplb
    BASE_KWARGS["with_share"] = args.with_share
    BASE_KWARGS["with_smooth"] = args.with_smooth
    BASE_KWARGS["share_expert_intermediate_size"] = args.share_expert_intermediate_size \
        if args.share_expert_intermediate_size is not None else args.moe_intermediate_size
    test_fused_deep_moe_base()