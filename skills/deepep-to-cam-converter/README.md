# DeepEP-to-CAM-Converter 使用手册

## 功能概述

本技能用于将基于 DeepEP（NVIDIA GPU MoE 通信库）的混合专家模型（Mixture of Experts）代码迁移至华为昇腾 NPU 环境。迁移过程涵盖以下三个层面：

1. **通信域转换**：NCCL → HCCL（`backend='nccl'` → `backend='hccl'`）
2. **设备适配**：CUDA → NPU（`torch.cuda` → `torch.npu`，`.cuda()` → `.npu()`）
3. **算子替换**：DeepEP 的 `dispatch`/`combine` 算子 → 对应的 CAM 算子

本技能会在原地修改用户指定的源文件，并在执行前进行严格的参数约束校验，确保替换方案与用户的实际运行参数匹配。

---

## 适用场景

当代码中存在以下特征时，可触发本技能：

```python
import deep_ep
buffer = deep_ep.Buffer(group, ...)
recv_x, _, handle, event = buffer.dispatch(...)
combined_x, event = buffer.combine(...)
```

只要检测到 `deep_ep` 的导入及对应算子调用，即视为可迁移目标。

---

## 替换方案概览

本技能提供四种 CAM 算子替换方案，根据目标环境（A2/A3）和代码特征进行匹配：

| 方案 | 目标环境 | 说明 | 优势 | 限制 |
| :--- | :--- | :--- | :--- | :--- |
| **A2 算子** | 昇腾 A2 | 将 DeepEP dispatch/combine 替换为 CAM A2 对应接口 | 实现最为简单 | rank 数固定为 16，专家数 ≤ 256，不支持量化 |
| **A3 普通算子** | 昇腾 A3 | 将 DeepEP dispatch/combine 替换为 CAM A3 对应接口 | rank 数支持 [2, 384]，专家数 ≤ 512，支持量化 | 通信与计算分离，性能不及 Shmem 和 Fused Deep MoE |
| **A3 Shmem 算子** | 昇腾 A3 | 基于共享内存（SHMEM）实现通信后端，替代 HCCL 通信 | 性能优于普通 A3 算子 | 需额外编写 SHMEM 初始化与销毁代码，ep_world_size 仅支持 8 个离散值 |
| **Fused Deep MoE** | 昇腾 A3 Decode 阶段 | 将 `[Dispatch + GMM1 + Swiglu + GMM2 + Combine]` 融合为单个算子调用 | 性能最优，通信与计算一体化 | 仅适用于 Decode 阶段，必须严格满足 GMM1+Swiglu+GMM2 范式 |

**选择优先级（性能从高到低）：**
```
Fused Deep MoE > Shmem > 普通 A3 > A2
```

当较高优先级的方案不满足约束时，自动降级到下一方案。若 A3 环境下多个方案同时满足约束，技能将暂停并要求用户手动选择。

---

## 工作流程

```
输入：包含 DeepEP 调用的目标代码
    │
    ▼
┌─────────────────┐    未检测到    ┌──────────────┐
│ 阶段1：代码扫描   │ ────────────→│ 终止流程，    │
│ 识别 deep_ep 调用 │              │ 提示用户      │
└────────┬────────┘              └──────────────┘
         │ 检测到
         ▼
┌─────────────────┐
│ 阶段2：环境确认   │  自动检测（npu-smi info）或用户手动指定 A2/A3
│ 与策略决策       │
│                 │  校验实际运行参数是否满足约束
│                 │  A3 场景下多方案可选时由用户决策
│                 │  功能支持度分析（低延迟/Event/调优参数等）
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ 阶段3：执行替换   │  通信域转换（nccl→hccl, cuda→npu）
│                 │  算子接口替换（deep_ep → CAM）
│                 │  原地修改，不创建新文件
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ 阶段4：验证与报告 │  语法检查 + 修改清单 + 环境变量提示
└─────────────────┘
```

### 阶段 2 关键决策点

**参数确认原则：** 代码中 `argparse` 的 `default` 值不等于实际运行值。对于关键约束参数（如 `hidden_size`、`top_k`、`num_experts` 等），若无法从代码逻辑中 100% 确定其运行时赋值，必须向用户发起确认询问。

**功能支持度矩阵：**

| DeepEP 特性 | CAM 支持情况 | 处理策略 |
| :--- | :--- | :--- |
| 低延迟模式（`low_latency_mode`） | 不支持 | 标记为不兼容，以非低时延模式替换 |
| 精细化调优（`Config`、`num_sms`、`buffer_size`） | 不支持 | 忽略相关参数，使用 CAM 默认配置 |
| 异步控制流（`event = dispatch(...)` / `event.wait()`） | 不支持 | 移除 Event 相关代码，通信与计算重叠控制由 CAM 默认策略接管 |

对于不支持的特性，用户可选择：**停止替换** / **替换支持部分并保留原代码** / **替换支持部分并删除不支持代码**。

---

## 约束条件速查

### 通用约束

| 参数 | 约束 |
| :--- | :--- |
| `num_experts` | A2: ≤ 256；A3: ≤ 512 |
| `num_experts % num_ranks` | 必须整除 |
| `hidden_size` | ≤ 7168 且 `hidden_size % 32 == 0` |
| 量化 | A2 不支持；A3 / Shmem 支持 |

### 各方案独有约束

| 方案 | num_ranks / ep_world_size | batch_size | top_k | 其他要求 |
| :--- | :--- | :--- | :--- | :--- |
| A2 | 16 | [1, 4096] | (2, 16] | 需配置 `HCCL_INTRA_PCIE_ENABLE=1`、`HCCL_INTRA_ROCE_ENABLE=0` |
| A3 普通 | [2, 384] | (0, 8000] | (0, 16] | `num_experts ≥ num_ranks` |
| Shmem | {8, 16, 32, 64, 128, 144, 256, 288} | — | — | 需 SHMEM 初始化，不支持 TP |
| Fused Deep MoE | 无额外限制 | [0, 256] | [0, 12] | `token_length ∈ [1024, 7168]` 且 `% 256 == 0`；`gmm1_hidden_size ∈ [1024, 6144]` 且 `% 256 == 0`；必须为 GMM1+Swiglu+GMM2 范式 |

---

## 替换示例

### 示例 1：普通 A3 算子替换

**替换前（DeepEP）：**
```python
import deep_ep

buffer = deep_ep.Buffer(group, hidden_size, ...)

num_tokens_per_rank, ... , num_tokens_per_expert, ... = \
    buffer.get_dispatch_layout(topk_idx, num_experts)

recv_x, _, handle, event = buffer.dispatch(
    x=x, topk_idx=topk_idx, topk_weights=topk_weights, ...)
event.current_stream_wait()

# 专家计算逻辑（用户自定义）
# ...

combined_x, event = buffer.combine(
    x=recv_x, handle=handle, topk_weights=topk_weights, ...)
event.current_stream_wait()
```

**替换后（CAM A3）：**
```python
import torch_npu
import umdk_cam_op_lib

# 前置布局
num_tokens_per_expert, send_token_idx = \
    torch.ops.umdk_cam_op_lib.get_dispatch_layout(
        topk_idx, num_experts, num_ranks)

# 获取 HCCL 通信域名称
ep_hcomm_info = group._get_backend(
    torch.device('npu')).get_hccl_comm_name(rank)
ep_hcomm_info = ep_hcomm_info.encode('utf-8')

# Dispatch
recv_x, dynamic_scales, expand_idx, recv_count, recv_tokens_per_expert = \
    torch.ops.umdk_cam_op_lib.moe_dispatch_prefill(
        x=x, topk_idx=topk_idx, topk_weights=topk_weights,
        num_tokens_per_expert=num_tokens_per_expert,
        send_token_idx_small=send_token_idx,
        group_ep=ep_hcomm_info, rank=rank, num_ranks=num_ranks,
        use_quant=False)

# 专家计算逻辑
# ...

# Combine
combined_x = torch.ops.umdk_cam_op_lib.moe_combine_prefill(
    x=recv_x, topk_idx=topk_idx, topk_weights=topk_weights,
    src_idx=expand_idx, send_head=recv_count,
    group_ep=ep_hcomm_info, rank=rank, num_ranks=num_ranks)
```

**主要变更点：**
- 移除 `deep_ep.Buffer` 对象，CAM 算子为无状态调用
- `get_dispatch_layout` 返回值变化：A3 返回 `(num_tokens_per_expert, send_token_idx)`，而 A2 返回 `(num_tokens_per_expert, notify_send_data)`
- Dispatch 返回值从 4 个变为 5 个，不再包含 `event` 对象
- Combine 的 `src_idx` 对应 Dispatch 的 `expand_idx` 出参，`send_head` 对应 `recv_count` 出参
- 需手动获取 HCCL 通信域名称并进行 UTF-8 编码

### 示例 2：Shmem 算子替换

与普通 A3 的区别在于需要额外的 SHMEM 初始化与资源释放逻辑：

```python
import shmem as shm

# SHMEM 初始化
init_attrs = shm.InitAttr()
init_attrs.my_rank = rank
init_attrs.n_ranks = ep_world_size
init_attrs.local_mem_size = 1024 ** 3
init_attrs.ip_port = "tcp://127.0.0.1:8666"
shm.aclshmem_init(init_attrs)
shmem_ptr = shm.aclshmem_malloc(1024 ** 3)

# Dispatch（ext_info 参数传入 SHMEM 基地址指针）
dispatch_output = torch.ops.umdk_cam_op_lib.moe_dispatch_shmem(
    x=x, expert_ids=expert_ids, scales=None, x_active_mask=None,
    ep_world_size=ep_world_size, ep_rank_id=ep_rank_id,
    moe_expert_num=moe_expert_num,
    tp_world_size=1, tp_rank_id=0, expert_shard_type=0,
    shared_expert_num=1, shared_expert_rank_num=0,
    quant_mode=0, global_bs=0, expert_token_nums_type=0,
    ext_info=shmem_ptr)
expand_x, _, expand_idx, _, ep_send_count, _, _ = dispatch_output

# Combine
combined_x = torch.ops.umdk_cam_op_lib.moe_combine_shmem(
    expand_x=expand_x, expert_ids=expert_ids, expand_idx=expand_idx,
    ep_send_counts=ep_send_count, expert_scales=scales,
    ep_world_size=ep_world_size, ep_rank_id=ep_rank_id,
    moe_expert_num=moe_expert_num,
    tp_world_size=1, tp_rank_id=0, ...,
    ext_info=shmem_ptr)

# 资源释放必须在同步之后
torch.npu.synchronize()
shm.aclshmem_free(shmem_ptr)
shm.aclshmem_finialize()
```

### 示例 3：Fused Deep MoE 算子替换

将 Dispatch + FFN(GMM1 + Swiglu + GMM2) + Combine 整个链路替换为单次调用：

```python
output, share_output, expert_token_nums = \
    torch.ops.umdk_cam_op_lib.fused_deep_moe(
        x=x,
        expert_ids=expert_ids,
        gmm1_weight=gmm1_weight_list,
        gmm1_weight_scale=gmm1_weight_scale_list,
        gmm2_weight=gmm2_weight_list,
        gmm2_weight_scale=gmm2_weight_scale_list,
        expert_scales=expert_scales,
        share_gmm1_weight=...,        # 可选
        share_gmm1_weight_scale=...,  # 可选
        share_gmm2_weight=...,        # 可选
        share_gmm2_weight_scale=...,  # 可选
        expert_smooth_scales=...,     # 可选
        share_smooth_scales=...,      # 可选
        x_active_mask=...,
        group_ep=ep_hcomm_info,
        ep_rank_size=ep_world_size,
        ep_rank_id=rank,
        moe_expert_num=moe_expert_num,
        quant_mode=0,
        global_bs=global_batch_size)
```

**注意事项：**
- 权重参数类型为 `Tensor[]`（列表）。耦合模式为单元素列表，分离模式每个专家一个元素
- 四组权重（`gmm1_weight`、`gmm1_weight_scale`、`gmm2_weight`、`gmm2_weight_scale`）必须统一为耦合模式或分离模式，不可混用
- 若原始代码使用非 Swiglu 激活函数或非分组矩阵乘法，则无法使用此算子

---

## 通信域转换对照

| 原代码 | 替换后 |
| :--- | :--- |
| `dist.init_process_group(backend='nccl', ...)` | `dist.init_process_group(backend='hccl', ...)` |
| `torch.cuda.set_device(local_rank)` | `torch.npu.set_device(local_rank)` |
| `torch.set_default_device('cuda')` | `torch.set_default_device('npu')` |
| `torch.device('cuda:0')` | `torch.device('npu:0')` |
| `tensor.cuda()` | `tensor.npu()` |
| `dist.new_group(backend="nccl", ...)` | `dist.new_group(backend="hccl", ...)` |

---

## 环境变量配置

```bash
# 通用配置
export HCCL_BUFFERSIZE=4096

# A2 环境额外配置
export HCCL_INTRA_PCIE_ENABLE=1
export HCCL_INTRA_ROCE_ENABLE=0

# Fused Deep MoE 场景下，HCCL_BUFFERSIZE 需根据实际参数动态计算：
# 不小于 ceil[(ep_rank_size × max_batch_size × moe_expert_num_per_rank × total_length × sizeof(dtype) × 2) / (1024 × 1024)]
```

---

## 常见问题与注意事项

### 1. import 顺序

`umdk_cam_op_lib` 依赖 `torch_npu` 的初始化，因此 `torch_npu` 必须优先导入：

```python
# 正确
import torch_npu
import umdk_cam_op_lib

# 错误（可能导致初始化失败）
import umdk_cam_op_lib
import torch_npu
```

### 2. 参数默认值与实际运行值的区别

以下代码中的 `default=7168` 不可直接作为约束判断依据：

```python
parser.add_argument('--hidden', type=int, default=7168)
```

用户可能通过 `--hidden 4096` 传入不同的运行值。所有约束判断必须基于确认后的实际运行参数。

### 3. Shmem 资源释放顺序

SHMEM 资源的释放必须在算子完全执行完毕后进行，否则可能导致内存访问异常：

```python
# 正确
torch.npu.synchronize()
shm.aclshmem_free(shmem_ptr)
shm.aclshmem_finialize()

# 错误（算子可能在释放后仍访问已回收内存）
shm.aclshmem_free(shmem_ptr)
torch.npu.synchronize()
```

---

## 方案选择决策树

```
dispatch 后紧接 GMM1 → Swiglu → GMM2 → combine？
  ├── 是 → 参数满足 Fused Deep MoE 约束？
  │         ├── 是 → 选用 Fused Deep MoE
  │         └── 否 → 进入下一判断
  └── 否 → 目标环境是？
              ├── A2 → 选用 A2 算子
              └── A3 → ep_world_size 在 Shmem 支持列表中？
                        ├── 是 → 选用 Shmem 或普通 A3（根据性能需求决策）
                        └── 否 → 选用普通 A3 算子
```