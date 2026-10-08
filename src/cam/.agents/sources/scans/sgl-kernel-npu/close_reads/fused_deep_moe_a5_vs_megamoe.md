# Fused DeepEP A5 vs MegaMoe (Case D) + Layer 2

Contrast only. Status: **accepted** (no new Case, no new Knowledge). Not a FusedMoE operator manual. Not a third MoE Knowledge.

Baseline:

- Case D: `cases/compute-communication/moe/ascend950-mega-moe-fused-ep-ffn/`
- Layer 2: `knowledge/engineering-experience/moe-fusion-materialization-and-sync.md`
- Inner EP loop: Case B / `deepep_a5_split_vs_case_b.md`

Question: does A5 `fused_deep_moe` invent a new fusion mechanism, or can Layer 2’s three questions classify it as a Case D alternative?

```yaml
source_identity:
  registry_ref: sgl-kernel-npu
  scanned_commit: 8d76a8da75315ae9add5cfd2458b8028289f1ada
  live_950:
    entry: csrc/deepep/ops/op_kernel/fused_deep_moe.cpp   # __DAV_C310__ → fused_deep_moe_a5/entry.h
    kernel: csrc/deepep/ops/op_kernel/fused_deep_moe_a5/
    cmake: csrc/deepep/ops/op_kernel/CMakeLists.txt      # CATLASS_ARCH=3510; includes system moe_distribute_base.h
  not_sot:
    - csrc/deepep/ops/op_kernel/fused_deep_moe.h         # A3 MIX 1C2V
    - FuseMode.DISPATCH_FFN_COMBINE                      # DEEPEP_SKIP_DISPATCH_FFN_COMBINE on 950
  upstream_presence:
    verified: false
  review: accepted
  case: none
```

`KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_MIX_AIC_1_2)`. engines: cube + vector + communication.

---

## Design-doc vs kernel (do not copy the docstring)

`python/deep_ep/deep_ep/buffer.py` FuseMode 1 text says “InitRouting + AllToAll + GMM…”. Live A5 `entry.h` Init is:

```text
x, expert_ids, gmm1/2 weights+scales, expert_scales,
optional share_* / smooth / x_active_mask,
→ output, share_output, expertTokenNums
```

That is the **Case B EP loop** (token-major + expert ids), not Case A InitRouting. Treat the InitRouting sentence as **wrong packaging docs**. Kernel wins.

Python still names the second return `ep_recv_count`. Kernel / A5 fused doc use `expertTokenNums`. Name ≠ lifetime.

---

## Inner EP morphology: B-push, not D-pull

Fused A5 does **not** copy MegaMoe’s dest-pull / `[mask|count]` source-window protocol.

`CamMoeDistributeDispatch::Process` (`raw_distributed/cam_moe_distribute_dispatch.h`):

```text
AlltoAllDispatch → SetStatus → WaitDispatch → LocalWindowCopy → UpdataTokenNumsOut
```

Same dest-push as Case B / A5 split. Payload is packed to dest `(rank, local_e, slot)`, not left token-major on the source window for the dest to pull.

Case D already taught pull vs B’s push as **communication morphology inside the same EP loop**, not a third loop. Fused A5 puts **B’s push** inside one MIX kernel. That is packaging of a known pair, not a new rank/window protocol class.

GMM inside the fused op is CATLASS `MmadMxWithCallback` + SwiGLU epilogue (`fused_deep_moe_a5.h`). That is Case C / 53 **inside fusion**, not a fourth GEMM Case.

---

## Layer 2 Q1 — API-visible handoffs

Split graph still: Dispatch `expand_x` / `assist_info` / `ep_recv_count` / GMM `group_list` / GMM1 `y` / GMM2 `y` / Combine `expand_x`.

A5 fused `FusedDeepMoe::Process` (`source-code`): workspace offsets from tiling (`x1Token`, `swiglu`, `x2Token`, `y2Token`, `groupList`, `expandIdx`, `epSendCount`). Then:

1. `DispatchMxGmm1SwigluQuantFunc` (AIV send/recv + AIC GMM1 + AIV SwiGLU/quant when `EXEC_FLAG_DEEP_FUSE`)
2. `PipeBarrier` + `CrossCoreFlag gmm1AivFinished`
3. AIV `CamMoeDistributeCombine.Init(gmGmm2DepOut, …)`
4. `MxGmm2CastCombineFunc` (GMM2 + combine)

| split API handoff | fused A5 replacement / internal state | class |
| --- | --- | --- |
| `x` / `expert_ids` / `y` | keep | keep |
| dispatch `expand_x` | workspace `x1Token` (recv local-expert-major) | **relocated** |
| `assist_info` / `expandIdx` | workspace `expandIdxOffset` | **relocated** |
| `ep_recv_count` | workspace `epSendCount` | **relocated** (still a count vector, not MegaMoe `[mask\|count]`) |
| GMM `group_list` | workspace `groupListOffset` INT64 prefix for Catlass | **relocated** (D **replaced** this with GMM `M`; taxonomy still applies) |
| GMM1 `y` / scale | workspace swiglu / `x2Token` + mx scale | **relocated** |
| GMM2 `y` / combine `expand_x` | workspace `y2Token` (`gmGmm2DepOut`) then combiner | **relocated** (D generic path **deleted** the full expert-major API buffer) |

Different **cells** than MegaMoe. Same **three outcomes**. Do not write a second Layer 2 file to store the table.

---

## Layer 2 Q2 — metadata

Rule 4 / 5 still hold. Map is still the EP triple, still this routing loop. Counts are still post-recv, local-expert scope.

`UpdateAndCleanInfo` writes Catlass groupList **prefix** and a **non-cumsum** `expertTokenNums` copy. API `expertTokenNums` is again a **late output**, not the GMM compose trap on the user graph. Layer 1 Rule 3 stays an API-compose rule; fusion removes it from the user graph.

---

## Layer 2 Q3 — visibility / ordering

Frozen sentence still holds: fusion may replace primitives, not delete producer→consumer visibility.

| dependency | A5 fused primitive (`source-code`) | MegaMoe live |
| --- | --- | --- |
| same-device AIC↔AIV after GMM1 | `PipeBarrier` + `CrossCoreFlag gmm1AivFinished` (not “one flag for everything”) | wave / per-expert workspace flags |
| GMM2 → combine send | `CrossCoreSetFlag/WaitFlag` `SEND_SYNC_EVENT_ID` / `RECV_SYNC_EVENT_ID` | intra-kernel flags |
| cross-rank payload | `SetStatus` / `WaitDispatch` on the **push** window | two `CrossRankSyncInWorldSize` on the **pull** window |

Cross-device **semantics** kept: remote payload visible before consumer reads. Primitive follows B-push, not D-pull.

---

## Shared expert

Kernel has `EXEC_FLAG_SHARED_EXPERT` and share_* workspace. Python public fused path may not wire those tensors. Kernel capability ≠ shipped API. Not a new Case until a close-read of a **live** 950 shared-expert launch.

---

## What this file does **not** do

- Re-teach 60 / 53 / MmadMx.
- Promote A3 `fused_deep_moe.h` or `DISPATCH_FFN_COMBINE`.
- Claim MegaMoe Host `CalTilingKey` MTE hardcode applies here (different op).
