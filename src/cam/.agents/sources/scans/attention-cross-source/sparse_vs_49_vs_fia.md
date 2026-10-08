# Sparse Attention ↔ CATLASS 49 ↔ ops-transformer FIA

Focused contrast. **Not a Case.** Knowledge is a separate file. Baseline Sparse Case stays accepted.

Question this file must answer:

```text
Sparse / 49 / FIA
到底只是三种 attention implementation，
还是在 selection / paged addressing /
online-softmax state / Cube–Vector pipeline 上
有可以共用的更高层 pattern？
```

```yaml
status: contrast-accepted
review: passed
new_case: none
new_knowledge: accepted   # knowledge/engineering-experience/fa-pipeline-online-softmax-and-paged.md
sources:
  sparse:
    registry_ref: sgl-kernel-npu
    case: cases/compute/attention/ascend950-block-sparse-paged-attention/
    scanned_commit: 8d76a8da75315ae9add5cfd2458b8028289f1ada
    verified_upstream: sgl-project/sgl-kernel-npu main ee334193e5278d7b3a64afcc2e3ee5243133c4a0
  catlass_49:
    registry_ref: catlass
    commit: a8b4fffbd9cabc021dfd47253e907a1cde59de25
    live: examples/49_ascend950_flash_attention_infer/fai_kernel.h
    qk_block: include/catlass/gemm/block/block_mmad_fai_qk_tla.hpp
  fia:
    registry_ref: ops-transformer
    commit: 9311b63bcd065b9779eb08a7d634e9463ee41ef1
    production: attention/fused_infer_attention_score/
    host_950: op_host/arch35/  # TilingFusedInferAttentionScoreV4
    kernel_950: op_kernel/fused_infer_attention_score_apt.cpp
    sibling_not_950_host: op_kernel/flash_attention_regular.h  # SplitFuse / AtlasA2
```

Do **not** reopen GEMM / MoE. Do **not** start MLA / indexer. Do **not** treat `experimental/attention/fused_infer_attention_score/` demo README as 950 SoT（它声明不支持 PA）。

---

## Live paths

### Sparse (already frozen)

Canonical: bf16 `10002` / `SasaRegularKernelArch35`.  
`select_idx` → `block_table` → physical page → outer **selected-page** loop → QK / online softmax / PV(`PRE_LAUNCH=2`) / `EpilogueRescaleO`.

### CATLASS 49

Teaching kernel: `FAInferKernel<BlockMmadQK, BlockMmadPV, EpilogueOnlineSoftmax, EpilogueRescaleO, PAGED_CACHE_FLAG>` in `fai_kernel.h`.  
QK Block: `MmadFAIQK<Arch::Ascend950, …>` (`block_mmad_fai_qk_tla.hpp`).  
Host example: `cacheMode>0` instantiates `enablePaFlag=true`（README 默认 `cacheMode=1`）。

KV 循环是 **连续 `kvSeq` tile**，不是 `select_idx` 列表。Paged 时：

```text
curKvSeqAxisIdx = kvSeqLoopCount * kvSeqlenBase
blockIdOffset   = curKvSeqAxisIdx / blockSize
blockTableOffset = batch * maxBlockNumPerBatch + blockIdOffset
```

Cube QK 在 `taskId`；Vector softmax 在 `taskId>0`；Cube PV 在 `taskId>1`；Vector rescale-O 在 `taskId>2`。这是 **49 自己的** QK/PV 重叠调度，与 Sparse `PRE_LAUNCH=2` **同类实现选择**，**不是** 三源共同 invariant。不要外推到 FIA 950。

`isFirstLoop` 初始化 online softmax / rescale；后续 tile `!isFirstLoop` 更新 running max/sum 并 rescale O。

### FIA 950

Host：`DAV_3510` → `TilingFusedInferAttentionScoreV4`（`fused_infer_attention_score_tiling.cpp`）。  
**不是** `TilingProcess4SplitFuse`。SplitFuse（`flash_attention_regular.h`，`Arch::AtlasA2`，`PRE_LAUNCH=2`）是非 3510 的 `IsUsingFAI` 路径。不要把 A2 SplitFuse 写成 950 SoT。

950 kernel 入口：`fused_infer_attention_score_apt.cpp` → PFA/IFA **arch35**（`prompt_flash_attention_entry_regbase.h` → `flash_attention_score_kernel_infer.h` / `fia_kernel_noquant_gqa.h` 等）。MLA fullquant 是 sibling，本对照不展开。

API 有 `blocktable`。`sparse_mode` 编的是 **mask 几何**（no-mask / all-mask / causal left-up / right-down / band）→ Host `ComputeSparseType`。生产 FIA **没有** `select_idx`。

Paged：`KvStorageMode::PAGE_ATTENTION` + `blockTableDim2`。寻址是序列位置对应的 logical page，不是 top-k 选择。

Vector 侧有 SoftMaxFlashV2 的 **no_update / update** 两套 VF（`vf_basic_block_*_{no_update,update}_sfa.h`）：第一块初始化，后续块更新 running softmax。这是同一套跨 KV-tile 状态，实现族不是 CATLASS `BlockEpilogue`。

---

## 四轴对照

| axis | Sparse Case | CATLASS 49 | FIA 950 |
| --- | --- | --- | --- |
| **1. selection** | MMA **前** `select_idx`（logical block id，`-1` skip）；只对 surviving pages 做 QK/PV | **无** top-k 选择；扫连续 `kvSeq` tiles；可选 **mask** 裁 S2 | **无** `select_idx`；`sparse_mode` = causal/band **mask**，不是 page 选择 |
| **2. paged addressing** | `select_idx` 给出 logicalId → `block_table` → physical | 连续 KV 位置 `/ blockSize` → `block_table` → physical | 同 49：PA 时 sequential logical page → `block_table` |
| **3. online-softmax state** | Vector 状态跨 **selected pages**；`kvBlockIdx==0` init；`EpilogueRescaleO` | 跨 **连续 KV tiles**；`isFirstLoop` init；`EpilogueRescaleO` | 跨 **S2 / sInner tiles**；VF no_update vs update |
| **4. Cube–Vector pipeline** | MIX 1:2；**本卡** `PRE_LAUNCH=2`（调度选择，非 family 规则） | MIX；**本例** QK → softmax(+1) → PV(+2) → rescale(+3) | MIX；950 走 PFA/IFA 流水。**未**把 PV-lag-QK 钉成 950 invariant；SplitFuse `PRE_LAUNCH=2` **不是** 950 SoT |

### 拆开以后

```text
共用（更高层 FA pattern）
  Cube QK
      → Vector online softmax（首 tile init，其后 update max/sum）
      → Cube PV
      → normalization-aware O update / merge
           Sparse / 49 可落成独立 Vector rescale-O
           FIA 950 未证明独立 rescale-O stage
  可选：block_table = logical → physical indirection
           logicalId producer 由 KV-domain policy 决定

不是三源 invariant
  PV 相对 QK 延迟多少拍（Sparse PRE_LAUNCH=2；49 taskId 流水）
  → 实现调度，只在各自 live path 上成立
  独立 Vector rescale-O stage
  → Sparse / 49 实现事实，不是 FIA 950 已证明的 stage

KV-domain policy（三者不同，所以是 peers 不是 alternative）
  Sparse : select_idx 选出的非连续 pages
  49     : 连续 kvSeq tiles（+ optional mask）
  FIA    : 连续 / mask-truncated S2（sparse_mode）

Sparse 独有
  MMA 之前用 select_idx 做非连续 page 选择
  稀疏发生在“扫哪些 KV page”，不是 MMA mask，也不是 causal window
```

`block_table` **不是** Sparse 的新机制。它是 logical → physical **indirection**；logicalId 谁产由 KV-domain policy 决定（`select_idx` vs 序列位置）。`block_table` 存在 ≠ 这块 attention 是 block-sparse。

FIA `sparse_mode` **不是** Sparse 的 sparsity。名字碰撞：mask 几何 vs page 选择。

---

## Verdict

| question | answer |
| --- | --- |
| 三种是同一张 Case 吗？ | **否** |
| 49 / FIA 是 Sparse 的 alternative 吗？ | **否。** 三者是 **common FA-pipeline family 的 peer implementations**；**KV-domain policy 不同**（selected pages vs 连续 kvSeq vs mask-truncated S2） |
| pipeline + paged `block_table` 共用吗？ | **是**（online-softmax 跨 KV-tile 状态 + normalization-aware O merge + 可选 logical→physical `block_table`） |
| 独立 Vector rescale-O 是三源 invariant 吗？ | **否。** Sparse / 49 有 `EpilogueRescaleO`；FIA 950 **未**证明独立 stage |
| PV-lag-QK 是三源 invariant 吗？ | **否。** Sparse / 49 各自有延迟调度；FIA 950 **未**钉这条 |
| 要不要再开 49 Case？ | **否。** 再开会重教跨 tile online-softmax / rescale；KV 扫描策略与 Sparse 不同，也不该并进 Sparse 卡 |
| 要不要再开 FIA Case？ | **否。** 950 生产 FA 是 PFA/IFA 家族 + 大量 tiling key；教学增量不在再写一张 QK/softmax/PV 卡 |
| Sparse Case 还要吗？ | **要。** `select_idx` 选择仍是当前唯一 single-source 新机制 |
| 第一篇 Attention Knowledge？ | **已写、accepted。** FA pipeline + optional paged `block_table`。**不要**写 `select_idx`，**不要**写 PV-lag-QK 通例，**不要**把 FIA 写成独立 rescale-O |

成功标准与 DeepEP 对称、方向相反：

- DeepEP：新实现被已有 MoE Case **吃掉**，不产 Case / Knowledge。
- 本轮：49 / FIA 是 FA-pipeline **peers**（对照证据，不开 Case）；Sparse 的 selection 课不被吸收。

---

## What NOT To Copy / 不要做

- 把 FIA 950 写成已证明存在独立 Vector rescale-O stage。
- 把 `block_table` 写成永远 sequential logical → physical。
- 把 PV 相对 QK 延迟写成 Sparse + 49 + FIA 共同规则。
- 把 49 / FIA 写成 “Sparse 的 alternative”。它们是同一 FA-pipeline family 的 peers，KV-domain policy 不同。
- 把 49 / FIA 认成 Sparse，或把 Sparse 并进 FIA。
- 把 FIA `sparse_mode` 写成 `select_idx`。
- 把 A2 SplitFuse `flash_attention_regular.h` / `Arch::AtlasA2` 当 950 FIA SoT。
- 把 `experimental/.../fused_infer_attention_score/README.md`（不支持 PA、产品表只有 A2/A3）当生产 FIA。
- 用 49 的 `PAGED_CACHE_FLAG` 证明存在 top-k sparse selection。
- 现在开 MLA / lightning indexer / CATLASS 70 / `sparse_flash_attention` 全家桶。
- 再找一个 Sparse Attention 实现来重复 Case。

---

## Status

`review: passed`。49 / FIA **不开 Case**。

Attention Knowledge：[fa-pipeline-online-softmax-and-paged](../../../knowledge/engineering-experience/fa-pipeline-online-softmax-and-paged.md)（`accepted`）。Sparse Case 仍是 selection SoT。
