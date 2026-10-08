# Close-read: `matmul_reduce_scatter_v2` native ReduceScatter（fp16/bf16）

Not a Case. Pin **one live variant**. Sibling `TPL_CCU_ALL2ALL_VEC_REDUCE` 不在本文。

```yaml
op: matmul_reduce_scatter_v2
commit: 9311b63bcd065b9779eb08a7d634e9463ee41ef1
hw: ascend950
canonical_kernel: MatmulReduceScatterFP16BF16
canonical_comm: Hccl::ReduceScatter<true>
not_this_file: MatmulA2AVecReduceFP16BF16  # job 仍叫 RS
```

入口：`mc2/matmul_reduce_scatter_v2/op_kernel/arch35/matmul_reduce_scatter_v2_apt.cpp`  
实现：`.../arch35/matmul_reduce_scatter_fp16_bf16.h`  
Host：`op_host/op_tiling/matmul_reduce_scatter_tiling_base.cpp`

---

## 1. Exact 950 tiling key（native）

Kernel 分派（fp16/bf16）：

```text
TPL_INPUT == INPUT_TYPE_IS_FP16_BF16
TPL_COMMALG == TPL_CCU_REDUCESUM          → MatmulReduceScatterFP16BF16
TPL_COMMALG == TPL_CCU_ALL2ALL_VEC_REDUCE → MatmulA2AVecReduceFP16BF16   // 不要
```

`TPL_CCU_REDUCESUM = 0`，`TPL_CCU_ALL2ALL_VEC_REDUCE = 1`（`matmul_reduce_scatter_v2_apt_tiling_key.h`）。

Host `GetTilingKey()`：`commAlg = isA2APath_ ? TPL_CCU_ALL2ALL_VEC_REDUCE : TPL_CCU_REDUCESUM`。

`isA2APath_`（`IsUseA2APath`）：**DAV_3510 且 rankDim ∈ {4, 8}**。  
`commMode == ai_cpu` 时强制 `isA2APath_ = false` → native RS。

因此 950 **4P/8P + CCU 默认不是本文路径**。本文是：

```text
TPL_COMMALG = TPL_CCU_REDUCESUM
TPL_COMM_MODE = CCU 或 AICPU
dtype = fp16 | bf16
```

Agent trap：Host 对这条 fp16/bf16 key 仍传 `OUTPUT_TYPE_IS_FP8`（枚举值 1）去填模板槽，**不是**输出真是 FP8。看 `TPL_INPUT` + `TPL_COMMALG`。

`KERNEL_TYPE_MIX_AIC_1_2`。`--cce-auto-sync=off` 是编译条件，不是 sync 原语。

---

## 2. Chunk coordinate

Host formulaic 切 **M**（`DoFormulaticTiling` / HCCL 256MB 再切）：`tileCnt` 个主块 + 可选 `tailCnt` 尾块。`rankM` = 用户原始 M。

Kernel 一个 comm chunk `i`：

```text
headSliceM = (rankM/rankDim - tailM*tailCnt) / tileCnt     # InitForMC2，RS 的 rankM_ = rankM/rankDim
sliceM     = tCubeTiling.M / rankDim                       # ReduceScatter 元素行数
recvCount  = sliceM * tCubeTiling.N
cOffset    = recvCount * sizeof(C)
handles_[i]           i = 0 .. tileCnt-1
handles_[tileCnt + j] j = 0 .. tailCnt-1
```

`UpdateSlice(i, isTail)`：`MC2MatmulAswBlockDerive::UpdateOffset` 按 `idx * headSliceM`（或 tail）给 **每个 rank 一段** A/C 偏移。RS 的 `isGather=false`，`calRankNum = rankDim`：这一 comm tile 的 Cube 覆盖 **所有 rank 在该 M 切片上的 C**，然后 RS 把它们归约散射。

这不是 66/68 的 K 切分。

---

## 3. Producer / consumer timeline

```text
Init
  hccl_.InitV2 + SetCcTilingV2
  gmToFloat_ = workspaceGM          // send buf

Process
  InnerProcess
    Compute(main tiles)
    if tailM: Compute(tail tiles)
  PostProcess
    AIC block0: Wait all handles; Finalize
```

每个 tile 内（`MatMulV3Compute`，仅 AIC used cores）：

```text
mmv3.UpdateSlice(i)
mmv3.Process(...)                  // Cube 写 send buf
CrossCoreSetFlag<0, PIPE_FIX>(3)
CrossCoreWaitFlag(3)               // same-device：FIXPIPE 后再让 comm 读
handles_[i+shift] = ReduceScatter<true>(cWork, recvBuffer, recvCount, SUM, stride, 1)
cWork += cOffset; recvBuffer += cOffset
```

`<true>` = 这次调用交出 handle（comm 已启动，不必在循环里 Wait）。  
**Wait 全部在 PostProcess** → tile i 的 RS 可以和下一块 Cube overlap。

没有单独的 `Commit` API；`ReduceScatter<true>` 即 launch。

---

## 4. Handle lifetime

- 数组 `handles_[MAX_HANDLE]`，`MAX_HANDLE = 64`。
- 一个 comm tile 一个 handle；tail 用 `shift = tileCnt`。
- 写入：每个 **used AIC** 核在 MM 循环里都调用 `ReduceScatter`（源码无 `block_idx==0` 守卫）。Wait/Finalize **只有 AIC `GetBlockIdx()==0`**。HCCL 是否在非 0 核上内部过滤：**未**在本文件证明。
- 真正 Wait：`PostProcess` 对 `tileCnt+tailCnt` 个 handle 依次 `hccl_.Wait`。

---

## 5. Buffer ownership

| 角色 | 正常路径 | `MC2_DEBUG_ONLY_CUBE` |
| --- | --- | --- |
| MM 写出 / RS send | `gmToFloat_` workspace | `cGM` |
| RS recv / 最终 shard | `cGM` | workspace（且 `recvCount=1` 哑调用） |
| A / B | `aGM_` / `bGM_` 本 rank shard | 同左 |

`GetWorkspaceSize`：native 只要一份 `mmResultLen_` send buf；A2A sibling 再加一份 recv。

API 最终：rank-local **C shard**。中间 full-C 在 workspace，是 comm 的 send，不是下一张 API tensor。

---

## 6. AIC / AIV

| | native fp16/bf16 |
| --- | --- |
| AIV | `Compute()` 开头 `if ASCEND_IS_AIV return`。**不参与** RS |
| AIC used | Cube + flag 3 + ReduceScatter |
| AIC idle（`block_idx >= usedCoreNum`） | 只做同样次数的 flag 3 Set/Wait，不发 RS |
| AIC 0 | PostProcess Wait + Finalize |

Flag 3 在 **PIPE_FIX 之后、ReduceScatter 之前**。AIV 不 Wait 这条 flag。把它读成 **Cube 写完 GM 的 same-device 就绪**，不要读成 cross-rank，也不要读成 AIV 消费 C。

`--cce-auto-sync=off` 只说明不能指望编译器代插 sync。

---

## 7. Local shortcut？

**没有** AG 那种 “本卡 C 绕过 Wait”。每一块都是 MM → RS。本 rank 的 shard 是 ReduceScatter 的语义结果，不是单独 local MM 路径。

---

## 不要做

- 把 4P/8P CCU 默认路径当成这篇 native RS。
- 把 `OUTPUT_TYPE_IS_FP8` 读成输出 dtype。
- 把 flag 3 写成跨 rank。
- 展开 quant / A2A+vec。Case 已 `accepted`：`cases/compute-communication/mc2/ascend950-matmul-native-reduce-scatter/`。
