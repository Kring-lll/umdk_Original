from typing import Any, Callable, Dict, Iterator, List, Optional, Tuple, Union, Sequence

import torch
import torchair
from torch.library import impl
from torchair.ge_concrete_graph.fx2ge_converter import register_fx_node_ge_converter
from torchair.ge_concrete_graph.ge_graph import (
    Tensor,
    TensorSpec,
    auto_convert_to_tensor,
    compat_as_bytes,
    compat_as_bytes_list,
    get_default_ge_graph,
    get_invalid_desc,
    next_unique_name,
)
from torchair.ge_concrete_graph.utils import dtype_promote

import umdk_cam_op_lib


# @impl(umdk_cam_op_lib, "fused_deep_moe")
# def fused_deep_moe_meta(
#     x,
#     expert_ids,
#     gmm1_weight,
#     gmm1_weight_scale,
#     gmm2_weight,
#     gmm2_weight_scale,
#     expert_scales,
#     share_gmm1_weight=None,
#     share_gmm1_weight_scale=None,
#     share_gmm2_weight=None,
#     share_gmm2_weight_scale=None,
#     expert_smooth_scales=None,
#     x_active_mask=None,
#     comm_args=None,
#     group_ep="",
#     ep_rank_size=8,
#     ep_rank_id=0,
#     moe_expert_num=0,
#     shared_expert_num=0,
#     shared_expert_rank_num=0,
#     quant_mode=0,
#     global_bs=0,
# ):
#     torch._check(
#         (ep_rank_id >= 0) and (ep_rank_id < ep_world_size),
#         lambda: (
#             f"ep_rank_id should be in [0, ep_world_size), "
#             f"but got {ep_world_size=}, {ep_rank_id=}."
#             f"{ops_error(ErrCode.VALUE)}."
#         ),
#     )
#     torch._check(
#         (shared_expert_rank_num >= 0) and (shared_expert_rank_num < ep_world_size),
#         lambda: (
#             f"shared_expert_rank_num should be in [0, ep_world_size), "
#             f"but got {ep_world_size=}, {shared_expert_rank_num=}."
#             f"{ops_error(ErrCode.VALUE)}."
#         ),
#     )
#     is_shared_default = (shared_expert_num == 1) and (shared_expert_rank_num == 0)
#     is_no_shared = (shared_expert_num == 0) and (shared_expert_rank_num == 0)
#     is_valid_shared = (
#         (shared_expert_num > 0)
#         and ((shared_expert_rank_num // shared_expert_num) > 0)
#         and ((shared_expert_rank_num % shared_expert_num) == 0)
#     )
#     torch._check(
#         is_shared_default or is_no_shared or is_valid_shared,
#         lambda: (
#             f"shared expert setting invalid, "
#             f"got {shared_expert_num=}, {shared_expert_rank_num=}."
#             f"{ops_error(ErrCode.VALUE)}."
#         ),
#     )

#     bs = x.size(0)
#     h = x.size(1)

#     output = x.new_empty((bs, h))
#     share_output = x.new_empty((bs, h))

#     is_share_expert = ep_rank_id < shared_expert_rank_num
#     if is_share_expert:
#         local_expert_num = 1
#     else:
#         local_expert_num = moe_expert_num // (ep_rank_size - shared_expert_rank_num)

#         expert_token_nums = expert_ids.new_empty(
#         (local_expert_num,), dtype=torch.int64
#     )

#     return (output, share_output, expert_token_nums)
 
# This api is auto-generated from IR FusedDeepMoe
@auto_convert_to_tensor(
    [False, False, True, True, True, True, False, False, False, False, False, False, False, False, False],
    [False, False, False, False, False, False, False, True, True, True, True, True, True, True, True]
)
def FusedDeepMoe(
    x: Tensor,
    expert_ids: Tensor,
    gmm1_weight: Sequence[Tensor],
    gmm1_weight_scale: Sequence[Tensor],
    gmm2_weight: Sequence[Tensor],
    gmm2_weight_scale: Sequence[Tensor],
    expert_scales: Tensor,
    share_gmm1_weight: Optional[Tensor] = None,
    share_gmm1_weight_scale: Optional[Tensor] = None,
    share_gmm2_weight: Optional[Tensor] = None,
    share_gmm2_weight_scale: Optional[Tensor] = None,
    expert_smooth_scales: Optional[Tensor] = None,
    share_smooth_scales: Optional[Tensor] = None,
    x_active_mask: Optional[Tensor] = None,
    comm_args: Optional[Tensor] = None,
    *,
    group_ep: str = "",
    ep_rank_size: int = 8,
    ep_rank_id: int = 0,
    moe_expert_num: int = 0,
    shared_expert_num: int = 0,
    shared_expert_rank_num: int = 0,
    quant_mode: int = 0,
    global_bs: int = 0,
    dependencies=[],
    node_name=None,
):
    """REG_OP(FusedDeepMoe)\n
    .INPUT(x, TensorType({DT_BF16, DT_FLOAT16}))\n
    .INPUT(expert_ids, TensorType({DT_INT32}))\n
    .INPUT(gmm1_weight, TensorType({DT_INT8}))\n
    .INPUT(gmm1_weight_scale, TensorType({DT_FLOAT}))\n
    .INPUT(gmm2_weight, TensorType({DT_INT8}))\n
    .INPUT(gmm2_weight_scale, TensorType({DT_FLOAT}))\n
    .INPUT(expert_scales, TensorType({DT_FLOAT}))\n
    .OPTIONAL_INPUT(share_gmm1_weight, TensorType({DT_INT8}))\n
    .OPTIONAL_INPUT(share_gmm1_weight_scale, TensorType({DT_FLOAT}))\n
    .OPTIONAL_INPUT(share_gmm2_weight, TensorType({DT_INT8}))\n
    .OPTIONAL_INPUT(share_gmm2_weight_scale, TensorType({DT_FLOAT}))\n
    .OPTIONAL_INPUT(expert_smooth_scales, TensorType({DT_FLOAT}))\n
    .OPTIONAL_INPUT(x_active_mask, TensorType({DT_BOOL}))\n
    .OUTPUT(output, TensorType({DT_BF16, DT_FLOAT16}))\n
    .OUTPUT(share_output, TensorType({DT_BF16, DT_FLOAT16}))\n
    .OUTPUT(expert_token_nums, TensorType({DT_INT64}))\n
    .REQUIRED_ATTR(group_ep, String)\n
    .REQUIRED_ATTR(ep_rank_size, Int)\n
    .REQUIRED_ATTR(ep_rank_id, Int)\n
    .REQUIRED_ATTR(moe_expert_num, Int)\n
    .REQUIRED_ATTR(share_expert_num, Int)\n
    .REQUIRED_ATTR(share_expert_rank_num, Int)\n
    .ATTR(quant_mode, Int, 0)\n
    .ATTR(global_bs, Int, 0)\n
    """
    op = get_default_ge_graph().op.add()
    op.type = "FusedDeepMoe"
    op.name = next_unique_name(node_name, "FusedDeepMoe")

    # process dependencies
    for dependency in dependencies:
        op.input.append(dependency.controller)

    def _add_input(t: Tensor, name: str):
        op.input.append(t.tensor)
        op.input_desc.add().CopyFrom(t.desc)
        op.input_desc[-1].name = name

    _add_input(x, "x")
    _add_input(expert_ids, "expert_ids")

    def _add_dynamic_inputs(ts: Sequence[Tensor], name: str):
        for i, t in enumerate(ts):
            op.input.append(t.tensor)
            op.input_desc.add().CopyFrom(t.desc)
            op.input_desc[-1].name = name + str(i)

    _add_dynamic_inputs(gmm1_weight, "gmm1_weight")
    _add_dynamic_inputs(gmm1_weight_scale, "gmm1_weight_scale")
    _add_dynamic_inputs(gmm2_weight, "gmm2_weight")
    _add_dynamic_inputs(gmm2_weight_scale, "gmm2_weight_scale")

    _add_input(expert_scales, "expert_scales")

    def _add_optional_input(t: Optional[Tensor], name: str):
        if t is not None:
            op.input.append(t.tensor)
            op.input_desc.add().CopyFrom(t.desc)
        else:
            op.input.append("")
            op.input_desc.add().CopyFrom(get_invalid_desc())
        op.input_desc[-1].name = name

    _add_optional_input(share_gmm1_weight, "share_gmm1_weight")
    _add_optional_input(share_gmm1_weight_scale, "share_gmm1_weight_scale")
    _add_optional_input(share_gmm2_weight, "share_gmm2_weight")
    _add_optional_input(share_gmm2_weight_scale, "share_gmm2_weight_scale")
    _add_optional_input(expert_smooth_scales, "expert_smooth_scales")
    _add_optional_input(share_smooth_scales, "share_smooth_scales")
    _add_optional_input(x_active_mask, "x_active_mask")
    _add_optional_input(comm_args, "comm_args")

    op.attr["group_ep"].s = compat_as_bytes(group_ep)
    op.attr["ep_rank_size"].i = ep_rank_size
    op.attr["ep_rank_id"].i = ep_rank_id
    op.attr["moe_expert_num"].i = moe_expert_num
    op.attr["share_expert_num"].i = shared_expert_num
    op.attr["share_expert_rank_num"].i = shared_expert_rank_num
    op.attr["quant_mode"].i = quant_mode
    op.attr["global_bs"].i = global_bs


    output_index = 0
    
    op.output_desc.add().name = "output"
    output = Tensor(op, output_index)
    output_index += 1

    op.output_desc.add().name = "share_output"
    share_output = Tensor(op, output_index)
    output_index += 1

    op.output_desc.add().name = "expert_token_nums"
    expert_token_nums = Tensor(op, output_index)
    output_index += 1

    return output, share_output, expert_token_nums


@register_fx_node_ge_converter(torch.ops.umdk_cam_op_lib.fused_deep_moe.default)
def convert_fused_deep_moe_default(
    x: Tensor,
    expert_ids: Tensor,
    gmm1_weight,
    gmm1_weight_scale,
    gmm2_weight,
    gmm2_weight_scale,
    expert_scales: Tensor,
    share_gmm1_weight: Optional[Tensor] = None,
    share_gmm1_weight_scale: Optional[Tensor] = None,
    share_gmm2_weight: Optional[Tensor] = None,
    share_gmm2_weight_scale: Optional[Tensor] = None,
    expert_smooth_scales: Optional[Tensor] = None,
    share_smooth_scales: Optional[Tensor] = None,
    x_active_mask: Optional[Tensor] = None,
    comm_args: Optional[Tensor] = None,
    group_ep: str = "",
    ep_rank_size: int = 8,
    ep_rank_id: int = 0,
    moe_expert_num: int = 0,
    shared_expert_num: int = 0,
    shared_expert_rank_num: int = 0,
    quant_mode: int = 0,
    global_bs: int = 0,
    meta_outputs: TensorSpec = None,
):
    return FusedDeepMoe(
        x=x,
        expert_ids=expert_ids,
        gmm1_weight=gmm1_weight,
        gmm1_weight_scale=gmm1_weight_scale,
        gmm2_weight=gmm2_weight,
        gmm2_weight_scale=gmm2_weight_scale,
        expert_scales=expert_scales,
        share_gmm1_weight=share_gmm1_weight,
        share_gmm1_weight_scale=share_gmm1_weight_scale,
        share_gmm2_weight=share_gmm2_weight,
        share_gmm2_weight_scale=share_gmm2_weight_scale,
        expert_smooth_scales=expert_smooth_scales,
        share_smooth_scales=share_smooth_scales,
        x_active_mask=x_active_mask,
        comm_args=comm_args,
        group_ep=group_ep,
        ep_rank_size=ep_rank_size,
        ep_rank_id=ep_rank_id,
        moe_expert_num=moe_expert_num,
        shared_expert_num=shared_expert_num,
        shared_expert_rank_num=shared_expert_rank_num,
        quant_mode=quant_mode,
        global_bs=global_bs,
    )


