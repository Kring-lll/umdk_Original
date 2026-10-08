# Verdict: AG v2 ↔ RS v2 native（M1）

Scout passed 之后的 AG/RS close-read 结论。**Not a Case.**  
AR 形态见 [AR verdict](mc2_m1_ar_verdict.md)。M3 不展开。

证据：

- [mc2_mm_reduce_scatter_v2_native.md](mc2_mm_reduce_scatter_v2_native.md)
- [mc2_all_gather_mm_v2.md](mc2_all_gather_mm_v2.md)

```yaml
status: verdict-written
review: passed
new_case: cases/compute-communication/mc2/ascend950-matmul-native-reduce-scatter/
canonical_candidate: matmul_reduce_scatter_v2 native ReduceScatter
sibling: all_gather_matmul_v2 fp16/bf16
ar: member                    # native AR；repeat-AllReduce morphology；不开 Case
knowledge: knowledge/engineering-experience/mc2-compute-collective-coupling.md  # accepted
```

---

## AG / RS 是否同属更高层 “chunked MM-collective coupling”？

**是。** 当时钉死的是这两条 live path 的 **handle-per-chunk**。AR 后来证明同属 M1，但 request morphology 不同（见 [AR verdict](mc2_m1_ar_verdict.md)）。

共用：

```text
ASW Cube（mc2_mat_mul_asw_kernel.h）
  + Host 按 M 切 tileCnt/tailCnt
  + 每 chunk 一个 HCCL handle
  + Wait(handle) 才是 cross-rank consumer gate
  + CrossCoreFlag 只是 same-device
```

方向相反，所以是 **一正一反**，不是两套 family：

```text
AG:  comm → MM
     producer = AllGather
     consumer = Cube（远端 tile）
     本卡 A 可 local MM，不 Wait

RS:  MM → comm
     producer = Cube
     consumer = ReduceScatter
     无 local shortcut；PostProcess 统一 Wait
```

Ownership 也对称：AG 把 shard A 收成 **C full**；RS 把满切片 C 收成 **rank-local shard**。

**不要**把 handle-per-chunk 写成 M1 通例。AR 的 repeat-AllReduce 形态见 [AR verdict](mc2_m1_ar_verdict.md)。

---

## 哪一个更适合 canonical Case？

**RS v2 native `ReduceScatter`（fp16/bf16，`TPL_COMMALG=TPL_CCU_REDUCESUM`）。**

它直接教：

```text
Cube produces C tile
    → same-device FIXPIPE 就绪（flag 3）
    → ReduceScatter consumes tile（handle 留下）
    → Cube continues next tile
    → PostProcess Wait
    → rank-local C shard
```

计算产出的 chunk **何时**成为 communication 的合法输入，以及 **per-tile Wait deferred → RSᵢ overlaps MMᵢ₊₁**，都在一条 AIC 循环里。

AG 是 **reverse-coupling sibling evidence**，可以被同一张 Case 的 contrast 吸收：

- 同一套 Cube + handle
- producer/consumer 对调
- 多一个 local shortcut（本卡不 Wait）

不构成 “无法被第一张卡吸收的第二种机制”。不要为 AG 再开一张 Case。

---

## Case 必须 pin 的 live variant（写卡时）

| 钉 | 值 |
| --- | --- |
| Op | `matmul_reduce_scatter_v2` |
| Kernel class | `MatmulReduceScatterFP16BF16` |
| `TPL_COMMALG` | `TPL_CCU_REDUCESUM`（**不是** `TPL_CCU_ALL2ALL_VEC_REDUCE`） |
| Host 事实 | 950 **4P/8P CCU 默认**走 A2A+vec；native 出现在非 4/8 rankDim，或 `commMode=ai_cpu` |
| Contrast | AG v2 fp16/bf16 `AllGatherMatmulFP16BF16` |

Agent trap（进 Case 的 What Not To Copy）：

- job 名 ReduceScatter ≠ 一定 native RS
- Host 模板槽 `OUTPUT_TYPE_IS_FP8` ≠ 输出 FP8
- flag 3 ≠ 跨 rank
- 内部 ASW ≠ 新的 43/66/68

---

## 明确不在本 verdict

- AR 的 Wait 形态 → 见 [AR verdict](mc2_m1_ar_verdict.md)（`review: passed`）
- M2 pipeline_builder A2A
- M3 grouped A2Av
- M4 window

Knowledge：[mc2-compute-collective-coupling](../../../../knowledge/engineering-experience/mc2-compute-collective-coupling.md)（`accepted`）。

---

## 已执行

1. 一张 MC2 Case：RS native + 短 AG reverse contrast，`status: accepted`。  
   [`ascend950-matmul-native-reduce-scatter`](../../../../../cases/compute-communication/mc2/ascend950-matmul-native-reduce-scatter/case.md)
2. 不开 AG Case、不开 AR Case。
3. Knowledge：[mc2-compute-collective-coupling](../../../../knowledge/engineering-experience/mc2-compute-collective-coupling.md)（`accepted`）。
