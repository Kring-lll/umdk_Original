# 共享专家（Shared Expert）处理流程分析

## 概述

共享专家是 MoE（Mixture of Experts）中与普通专家并行的一条独立计算路径。普通专家通过 top-k 路由仅处理部分 token，而共享专家处理全量 batch token，其输出最终加到普通专家输出之上。

整个共享专家流程由编译期标志 `EXEC_FLAG & EXEC_FLAG_SHARED_EXPERT` 条件控制。共享专家和普通专家共用同一套 kernel 模板（`GmmDeqSwigluQuant`、`GmmDeq`），但通过独立的 share 参数（`gmShare*`）区分数据路径。

---

## 核心入口

**文件**: [fused_deep_moe.h:407](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe.h#L407)

```cpp
template <TemplateMC2TypeClass>
__aicore__ inline void FusedDeepMoe<TemplateMC2TypeFunc>::Process()
```

`Process()` 是 fused_deep_moe 算子的顶层调度函数，按顺序编排 Dispatch → GMM1+SwiGLU+Quant → GMM2+Dequant+Combine 三个阶段，每个阶段内部通过 `if constexpr (EXEC_FLAG & EXEC_FLAG_SHARED_EXPERT)` 插入共享专家处理。

---

## 一、工作空间布局

**位置**: [fused_deep_moe.h:430-482](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe.h#L430-L482)

共享专家的关键特征：`shareExpertTokenNum = bs_`（全量 batch），因为共享专家处理所有 token，不经过 top-k 筛选。

```cpp
size_t shareExpertTokenNum = 0;
if constexpr (EXEC_FLAG & EXEC_FLAG_SHARED_EXPERT) {
    shareExpertTokenNum = bs_;   // 全量 token 都经过共享专家
}
size_t maxHandleTokenNum = maxTokenNum_ + shareExpertTokenNum;
```

GM workspace 以"共享专家在前、普通专家在后"的方式排布，最大化内存复用：

| GM 地址 | 用途 | 大小 |
|---------|------|------|
| `gmShareX1` → `gmX1` | 量化后的输入 token (int8)，share 在前 | `maxHandleTokenNum * hidden * sizeof(int8)` |
| `gmShareX1Scale` → `gmX1Scale` | per-token 量化 scale (float) | `maxHandleTokenNum * sizeof(float)` |
| `gmShareSwigluOut` → `gmSwigluOut` | GMM1 后 SwiGLU 输出 (float) | `shareExpertTokenNum * shareGmm1HLen + maxTokenNum * gmm1HLen` |
| `gmShareX2` → `gmX2` | GMM2 输入 (int8)，复用 X1 的空间 | 同 X1 |
| `gmShareX2Scale` → `gmX2Scale` | GMM2 per-token scale | `maxHandleTokenNum * sizeof(float)` |

地址计算示例：

```cpp
gmShareX1 = workspaceGM_ + workspaceOffset;
gmShareX2 = workspaceGM_ + workspaceOffset;  // 复用同一块空间
gmX1 = gmShareX1 + shareExpertTokenNum * tokenHiddenSize_ * sizeof(int8_t);  // 偏移到普通专家区
gmX2 = gmShareX2 + shareExpertTokenNum * shareGmm2InputDim_ * sizeof(int8_t);
```

---

## 二、阶段一：分发与共享专家量化 — `QuantForShareExpert`

**调用链**:

```
FusedDeepMoe::Process()                                                    [fused_deep_moe.h:484]
  └─ CamMoeDistributeDispatch::Process()                                   [cam_moe_distribute_dispatch.h:1031]
       ├─ QuantForShareExpert()   ← 共享专家入口                             [cam_moe_distribute_dispatch.h:383]
       ├─ AlltoAllDispatch()      ← 普通专家 dispatch
       ├─ SetStatus()
       ├─ WaitDispatch()
       ├─ LocalWindowCopy()
       └─ UpdataTokenNumsOut()
```

**`QuantForShareExpert()` 实现**: [cam_moe_distribute_dispatch.h:383-414](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe/raw_distributed/cam_moe_distribute_dispatch.h#L383-L414)

**作用**：将原始输入 x 做 smooth quantization → int8，写入共享专家 workspace 区。

```
输入: x [bs, hidden] (fp16/bf16)
处理:
  每个 AIV 核心处理 [startTokenId, endTokenId) 范围的 token:
    for token in range:
      ① 从 xGMTensor_ 读原始 token
      ② QuantProcess(expertIndex=0, shareScalesGMTensor_):
         - Cast float16→float32
         - Mul(smoothScale)           // 使用 share_smooth_scales
         - Abs + ReduceMax → 动态量化 scale
         - Muls(dynamicScale) → Cast→int8
      ③ int8 结果写入 gmShareX1 (workspace)
      ④ 量化 scale(dequant scale)写入 gmShareX1Scale
输出: gmShareX1 [bs, hidden] (int8), gmShareX1Scale [bs] (float)
```

**关键点**: `QuantProcess(expertIndex=0, ...)` — 第一个参数固定为 0，表示使用共享 smooth scale（`shareScalesGMTensor_`），而非 per-expert scale（`scalesGMTensor_`）。共享专家只有一套 scale，所以 index 恒为 0。

**核心分配**: 使用 `aivNum_` 个核心并行处理 `bs_` 个 token，每个核心处理 `bs / aivNum` 个 token，余数均匀分配到前几个核心。

---

## 三、阶段二：GMM1 + Dequant + SwiGLU + Dynamic Quant

**调用链**:

```
FusedDeepMoe::Process()                                                    [fused_deep_moe.h:506]
  └─ GmmDeqSwigluQuant<TemplateMC2TypeFunc, Gmm1L1TileShape, ...>(...)    [fused_deep_moe.h:56-192]
       └─ GroupedMatmulSliceMPerTokenDequantSwigluQuantMultiStageWorkspace
            [grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h]
```

这个 kernel 完成三件事：**GMM1 MatMul → Dequant + SwiGLU 激活 → Dynamic Quant**。

### 3.1 参数传递

**位置**: [fused_deep_moe.h:506-513](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe.h#L506-L513)

```cpp
GmmDeqSwigluQuant(
    gmm1ProblemShape,                      // {maxTokenNum, gmm1OutputDim, tokenHiddenSize}
    groupCount_,                           // moeExpertNumPerRank
    gmGroupList, gmX1, layoutX1,
    gmShareWeight1_, layoutShareWeight1,   // ← 共享专家 GMM1 weight (zN 格式)
    gmWeight1_, layoutWeight1,             // ← 普通专家 GMM1 weight (zN 格式)
    gmShareWeight1Scale_,                  // ← 共享专家 weight scale
    layoutShareW1Scale,
    gmScale1_, layoutW1Scale,
    gmX1Scale, layoutX1Scale,             // per-token activation scale
    gmX2, layoutX2, gmX2Scale, layoutX2Scale,
    gmShareX1, gmShareX1Scale,            // ← 共享专家输入 token 及 scale
    gmShareSwigluOut, gmShareX2,          // ← 共享专家 SwiGLU 输出 & 量化输出
    layoutShareX2, gmShareX2Scale,
    gmSwigluOut,
    gmWorkspace, gmX_, gmSmoothScales_,
    gmShareSmoothScales_,                 // ← 共享专家 smooth scales
    gmexpertIds_, gmExpandIdx, gmEpSendCount,
    xActiveMask_, gmResvered, gmExpertTokenNums_,
    tilingData_->disGmmDeqSwigluQuantGmmDeqComInfo
);
```

其中 `shareN`（= `shareGmm1HLen`）来自 tiling 数据，决定共享专家 GMM1 的 N 维度：`shareN = share_expert_intermediate_size * 2`（gate + up 两部分）。

### 3.2 Kernel 参数结构

**位置**: [grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h:449-536](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe/gemm/kernel/grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L449-L536)

```cpp
struct Params {
    // ... common params ...
    GM_ADDR gmShareSmoothScales;   // 共享专家 smooth scales
    GM_ADDR gmShareX1;             // 共享专家 GMM1 输入 (int8 token)
    GM_ADDR gmShareX1Scale;        // 共享专家 per-token scale
    GM_ADDR gmShareSwigluOut;      // 共享专家 SwiGLU 输出 (float)
    GM_ADDR gmShareX2;             // 共享专家量化输出 (int8, GMM2 输入)
    GM_ADDR gmShareX2Scale;        // 共享专家 GMM2 per-token scale
    uint32_t shareN;               // = shareGmm1HLen
};
```

### 3.3 共享专家 GMM1 MatMul

**位置**: [grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h:594-624](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe/gemm/kernel/grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L594-L624)

```
处理流程:
  if (EXEC_FLAG & EXEC_FLAG_SHARED_EXPERT):
    ① 先于普通专家，执行共享专家的 GMM1:
       gmA = gmShareX1              // [bs, hidden] int8
       gmB = gmShareWeight1         // [hidden, shareGmm1HLen] int8 (zN format)
       执行 MatMul → [bs, shareGmm1HLen] int32
    
    ② 然后是普通专家的 GMM1（按 group 遍历）:
       gmA = gmX1                   // [total_tokens, hidden] int8
       gmB = gmWeight1              // per-expert weight list
       按 groupList 分组执行 MatMul
```

共享专家的 GMM1 使用单个 weight tensor（`gmShareWeight1_`），不需要 group 遍历。普通专家的 GMM1 按 `groupList` 遍历每个 expert group。

### 3.4 Dequant + SwiGLU — `CompCoreFunc`

**位置**: [grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h:1573-1606](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe/gemm/kernel/grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L1573-L1606)

```
CompCoreFunc 处理共享专家:
  if (EXEC_FLAG & EXEC_FLAG_SHARED_EXPERT):
    // 共享专家 dequant + swiglu
    // 输入: GMM1 int32 结果 + gmShareMm1Scale + gmShareTokenScale
    // 输出: gmShareSwigluOutput [bs, shareN] float32
    // 对每个 token:
    //   1. Dequant: int32 * weight_scale * per_token_scale → float
    //   2. SwiGLU: 沿 shareN/2 切分为 gate 和 up
    //      output = gate * sigmoid(gate) * up
```

### 3.5 Dynamic Quant

**位置**: [grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h:1947-1959](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe/gemm/kernel/grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L1947-L1959)

```
if (EXEC_FLAG & EXEC_FLAG_SHARED_EXPERT):
    BlockQuant(
      gmShareSwigluOut → gmShareX2,    // float → int8
      gmShareX2Scale                    // per-token quant scale
    )
// 将 SwiGLU float32 输出量化为 int8，供 GMM2 使用
```

### 3.6 shareQuantCoreFunc（Deep Fuse 模式）

**位置**: [grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h:1100-1125](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe/gemm/kernel/grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L1100-L1125)

在 deep fuse 模式下（`EXEC_FLAG & EXEC_FLAG_DEEP_FUSE`），dispatch 阶段被融合进 GMM1 kernel，因此在 kernel 内部需要重新执行共享专家的 smooth quantization（与阶段一的 `QuantForShareExpert` 功能重叠，但仅在 deep fuse 且 aivId==0 的核心上执行）。

---

## 四、阶段三：GMM2 + Dequant + Combine

**调用链**:

```
FusedDeepMoe::Process()                                                     [fused_deep_moe.h:528]
  └─ GmmDeq<TemplateMC2TypeFunc, Gmm2L1TileShape, ...>(...)                 [fused_deep_moe.h:194-278]
       └─ GroupedMatmulSliceMPerTokenDequantMultiStageWorkspace
            [grouped_matmul_slice_m_per_token_dequant_multistage_workspace.h]
```

### 4.1 参数传递

**位置**: [fused_deep_moe.h:528-533](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe.h#L528-L533)

```cpp
GmmDeq(
    gmm2ProblemShape, groupCount_,
    gmGroupList, gmX2, layoutX2, gmWeight2_, layoutWeight2,
    gmScale2_, layoutW2Scale, gmX2Scale, layoutX2Scale,
    gmGmm2DepOut, layoutOutput,
    bs_,                                // batch_size (普通 combine 用)
    shareGmm2ProblemShape,              // {bs, hidden, shareGmm2InputDim} ← 独立 problem shape
    gmShareX2,                          // ← 共享专家 GMM2 输入
    gmShareWeight2_,                    // ← 共享专家 GMM2 weight (zN format)
    gmShareOutput_,                     // ← 共享专家输出 [bs, hidden]
    gmShareWeight2Scale_,               // ← 共享专家 weight scale
    gmShareX2Scale,                     // ← 共享专家 per-token scale
    layoutShareX2, layoutShareWeight2,
    layoutShareX2Scale, layoutShareOutput,
    epRankId_, gmWorkspace, &combiner
);
```

### 4.2 共享专家 GMM2 MatMul

**位置**: [grouped_matmul_slice_m_per_token_dequant_multistage_workspace.h:269-333](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe/gemm/kernel/grouped_matmul_slice_m_per_token_dequant_multistage_workspace.h#L269-L333)

```
处理流程:
  if (EXEC_FLAG & EXEC_FLAG_SHARED_EXPERT):
    ① 先于普通专家，执行共享专家的 GMM2:
       gmA = params.ptrSharedA     // gmShareX2 [bs, shareGmm2InputDim] int8
       gmB = params.ptrSharedB     // gmShareWeight2 [shareGmm2InputDim, hidden] int8
       problemShape = sharedGmm2ProblemShape  // {bs, hidden, shareGmm2InputDim}
       
       执行 MatMul → Dequant → 输出
    
    ② 然后是普通专家的 GMM2:
       按 group 遍历，执行 MatMul + Dequant + Combine
```

共享专家 GMM2 的关键特点：
- 使用独立的 `sharedGmm2ProblemShape`，K 维是 `shareGmm2InputDim`（可与普通专家的 `gmm2InputDim` 不同）
- 使用独立的 soft pipeline stage 管理（`softStageUsed`），不与普通专家的 stage 冲突
- 输出直接写入 `gmShareOutput_`，不经过 combine 归约

---

## 五、Combine 归约（仅普通专家）

**文件**: [cam_moe_distribute_combine.h](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe/raw_distributed/cam_moe_distribute_combine.h)

`CamMoeDistributeCombine` 负责将分散在各 rank 的普通专家输出通过 all-to-all 通信归约回原 token 顺序。

**共享专家不经过 Combine**：因为每个 rank 都有全量 token 的共享专家输出 `gmShareOutput_ [bs, hidden]`，无需跨 rank 通信。Python 层在拿到 `share_output` 后直接加到 `output` 上。

---

## 整体数据流图

```
输入 x [bs, hidden]
    │
    ├─── 共享专家路径 ──────────────────────────────────────┐
    │                                                       │
    │  ① Dispatch: QuantForShareExpert()                    │
    │     x → smooth quant → gmShareX1 [bs, hidden] int8    │
    │     per-token scale → gmShareX1Scale [bs] float       │
    │                                                       │
    │  ② GMM1: gmShareX1 @ share_gmm1_weight               │
    │     [bs, hidden] @ [hidden, shareGmm1HLen]            │
    │     → [bs, shareGmm1HLen] int32                       │
    │                                                       │
    │  ③ Dequant + SwiGLU:                                  │
    │     int32 → float (dequant) → gate*sigmoid(gate)*up   │
    │     → gmShareSwigluOut [bs, shareGmm1HLen] float      │
    │                                                       │
    │  ④ Dynamic Quant:                                     │
    │     float → int8 → gmShareX2 [bs, shareGmm2Input]     │
    │     quant scale → gmShareX2Scale [bs] float           │
    │                                                       │
    │  ⑤ GMM2: gmShareX2 @ share_gmm2_weight               │
    │     [bs, shareGmm2Input] @ [shareGmm2Input, hidden]   │
    │     → Dequant → gmShareOutput [bs, hidden]            │
    │                                                       │
    ├─── 普通专家路径 ──────────────────────────────────────┤
    │                                                       │
    │  Dispatch: AlltoAll (路由 token)                      │
    │  GMM1 → SwiGLU → Quant (per-group)                   │
    │  GMM2 → Dequant                                      │
    │  Combine: AlltoAll (归约 token)                       │
    │                                                       │
    └───────────────────────────────────────────────────────┘
                          │
                          ▼
            output += share_output  (Python 层)
```

---

## 与普通专家路径的关键区别

| 维度 | 普通专家 | 共享专家 |
|------|---------|----------|
| **Token 范围** | 经 top-k 路由分发，每个专家只处理部分 token | 全量 `bs_` 个 token |
| **Weight 结构** | `[expert_num, K, N]` TensorList | 单个 `[K, N]` optional Tensor |
| **GMM1 N 维度** | `gmm1OutputDim_ (=intermediate*2)` | `shareGmm1OutputDim_`（可独立配置） |
| **GMM2 K 维度** | `gmm2InputDim_ (=intermediate)` | `shareGmm2InputDim_`（可独立配置） |
| **通信** | 需要 all-to-all dispatch + combine | 无需跨 rank 通信 |
| **输出** | `[total_tokens, hidden]` 需 combine 归约 | `[bs, hidden]` 直接输出 |
| **Smooth Scale** | per-expert `[expert_num, hidden]` | 共享 `[hidden]` |
| **Bias** | `gmm1_bias` / `gmm2_bias` (TensorList, DYNAMIC) | `share_gmm1_bias` / `share_gmm2_bias` (optional Tensor) |
| **Dispatch** | 参与 all-to-all token 分发 | 仅做 smooth quant，不参与分发 |

---

## 相关文件索引

| 文件 | 内容 |
|------|------|
| [fused_deep_moe.h](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe.h) | 顶层 `Process()` 调度、工作空间布局、`GmmDeqSwigluQuant` / `GmmDeq` 包装函数 |
| [fused_deep_moe.cpp](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe.cpp) | kernel 入口函数、`CALL_FUSED_DEEP_MOE` 宏 |
| [cam_moe_distribute_dispatch.h](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe/raw_distributed/cam_moe_distribute_dispatch.h) | Dispatch 实现，含 `QuantForShareExpert()` |
| [cam_moe_distribute_combine.h](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe/raw_distributed/cam_moe_distribute_combine.h) | Combine 实现 |
| [grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe/gemm/kernel/grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h) | GMM1 + Dequant + SwiGLU + Quant kernel |
| [grouped_matmul_slice_m_per_token_dequant_multistage_workspace.h](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe/gemm/kernel/grouped_matmul_slice_m_per_token_dequant_multistage_workspace.h) | GMM2 + Dequant + Combine kernel |
| [fused_deep_moe_base.h](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe_base.h) | `EXEC_FLAG_SHARED_EXPERT` 等编译期标志定义 |
| [fused_deep_moe_tiling.h](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe_tiling.h) | Tiling 数据结构，含 `shareGmm1HLen` |
| [op_host/fused_deep_moe.cpp](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_host/fused_deep_moe.cpp) | 算子定义，注册 share 相关输入 |
| [fused_deep_moe_sample.py](src/cam/examples/fused_deep_moe_sample.py) | Python 示例，`FusionOp` 和 `SmallOps` 中的共享专家调用 |
