# Close-read: GroupedMatmulSwigluQuantV2

Status: **Case C** `cases/compute/grouped-gemm/ascend950-gmm-swiglu-mx-quant/`. This file remains the evidence dump.

Production **extension** of CATLASS **60 (GMM slice-M / groupList)** + **53 (MX / MmadMx)**. This file does **not** re-explain those. It answers: what is fused on top, who feeds `groupList`, and whether the output is Combine’s `expand_x`.

```yaml
source_identity:
  registry_ref: ops-transformer
  scanned_commit: 9311b63bcd065b9779eb08a7d634e9463ee41ef1
  scanned_branch: master
  scan_path: /home/wangyibo/ops-transformer_bak
  path: gmm/grouped_matmul_swiglu_quant_v2/
  live_950: grouped_matmul_swiglu_quant_v2_apt.cpp (MX / pertoken / MxA8W4)
  upstream_presence:
    verified: false
    upstream_commit: null
```

`execution.task_type: mix` (`KERNEL_TYPE_MIX_AIC_1_2`). `engines: [cube, vector]`. AIC: grouped MMA (cgmct `BlockMmadMx` on MX path). AIV: SwiGLU + output quant (`block_epilogue_swiglu_mx_quant.h` / VF SwiGLU).

---

## What this op is

One grouped GEMM (expert **up**-proj style: `X[M,K] · W[E,K,N]`) → split N into act/gate → **SwiGLU** → **requant** to INT8 / FP8 / FP4.

It is **not** a full expert FFN (no second / down-proj GMM). mega_moe is the compressed Dispatch+GMM1+SwiGLU+GMM2+Combine; this op is the fused **GMM1+SwiGLU+quant** piece.

Row axis `M` is **not reordered**. Groups are contiguous slices of `X` described by `groupList`. Dispatch’s local-expert-major `expand_x` is the intended `X` layout.

---

## groupList (the Part 2 question)

| question | answer from this op |
| --- | --- |
| Who produces? | **Not this op.** Required input. |
| count or cumsum? | Attr `group_list_type`: **0 cumsum (default)**, **1 count**. Kernel uses previous prefix when type 0 (`groupListType == 0 && groupId > 0`). Same dual contract as dispatch `expert_token_nums_type` **and** as init type 0/1. |
| Before or after EP? | This kernel has **no rank**. It only sees local `X` rows. After EP, those rows are **this rank’s recv tokens**. The matching producer is therefore **dispatch `expert_token_nums`**, not init’s pre-shuffle histogram. |
| Local vs global experts? | `groupList` length = `E` = number of **weight groups** = typically **local experts on this rank** (W is `[E,K,N]`). Init counts can be a global `[expertEnd-expertStart]`. Length match fails if init enumerated all experts. |
| Host convert? | Tiling copies the attr through (`set_groupListType`). **No host rewrite of count→cumsum.** Caller must pass the representation they claim. |

Dispatch default `expert_token_nums_type = 1` (count). This op default `group_list_type = 0` (cumsum). **Defaults do not compose.** A production graph must set them equal.

**Verdict:** `dispatch.expert_token_nums → GMM.group_list` is **conditional-direct** (INT64, length = local E, type attr aligned). `init.counts → GMM.group_list` stays **not a direct-data-edge** on the EP chain (wrong time, often wrong length). Single-device (no EP) could still feed init counts — **logical alternative**, unproven here.

---

## 1. API contract (950)

| name | role |
| --- | --- |
| `x` | expert-major activations `[M, K]`. Quant dtypes (INT8 / FP8 / FP4 / HiF8). Match dispatch `expand_x` **if** dispatch quant_mode produced that dtype. |
| `x_scale` | per-token or MX e8m0 scales. Match dispatch `dynamic_scales`. |
| `group_list` | INT64 `[E]` |
| `weight` | dynamic list, often NZ. MX uses e8m0 `weight_scale` group-32 (Case 53 class). |
| `y` | requant expert rows `[M, N/2]` (SwiGLU halves N) |
| `y_scale` | output scales for `y` |

---

## 2. Host / tiling

950 `op_host/op_tiling/arch35/` (basic + weight-quant). Keys: MX vs pertoken vs MxA8W4 NZ (`grouped_matmul_swiglu_quant_v2_tiling_key.h`). `tuning_config` picks templates. **Do not** re-open Case 73 FullLoadA here (Scout already noted production GMM FullLoadA as sum-check; this fused op is a different binary).

---

## 3. Live kernel

- AIC: grouped MMA along M slices from `groupList` (60’s idea) with MX `BlockMmadMx` when e8m0 scales (53’s idea).
- AIV: SwiGLU on the N-split, then quant / MX scale gen (`gmmsq_quant_mx_vf.h`).
- `MIX_AIC_1_2`.

---

## 4. Data transformation

Token/rank: **none**. Expert axis is the **group** axis of W, which should be **local experts** after EP. `M` order = input `X` order = dispatch `expand_x` order if wired that way. Output `M` is the same; **H becomes N/2**.

---

## 5. Metadata

| tensor | producer | consumer |
| --- | --- | --- |
| `group_list` | **dispatch `expert_token_nums` (candidate)** | this kernel’s M split |
| `x_scale` | dispatch `dynamic_scales` | dequant before/with MMA |
| `y_scale` | **this op** | next GMM2 / later quant consumer — **not** combine `expert_scales` (those are gate weights) |

---

## 6. Sync

Cube ↔ Vector epilogue handshake inside the fused kernel (cgmct AIC→AIV builder). No HCCL.

---

## 7. Relation to previous / next

| edge | verdict |
| --- | --- |
| dispatch `expand_x` → `x` | **conditional-direct** if dtype/quant_mode match and `M==A` |
| dispatch `dynamic_scales` → `x_scale` | **conditional-direct** (layout: 1-D pertoken vs 2-D MX) |
| dispatch `expert_token_nums` → `group_list` | **conditional-direct**, attrs must match; defaults do not |
| init counts → `group_list` | **not** on EP chain |
| `y` → combine `expand_x` | **logical-edge**. Down-proj is `grouped_matmul` (`grouped_matmul_downproj_contract.md`). |
| `y` → following `grouped_matmul` (down) | **conditional-direct** (same M/`group_list`; GMM2 `x` = this `y`) |

---

## 8. CATLASS

| mechanism | relation |
| --- | --- |
| prefix-sum / count `groupList` slicing M | **60 extension** |
| MX group-32 / `MmadMx` / e8m0 | **53 extension** (production host + fused epilogue; do not clone Case 53) |
| SwiGLU on AIV + output requant | **new production fusion** |
| StreamK / SplitK / FullLoadA as Cases | **not claimed** from this binary |

---

## 9. Tests

UT tiling/infershape (incl. invalid `group_list_type`), ST A8W8, examples pertoken / MX A8W4. No joint test with dispatch.

---

## 10. Evidence conflicts

1. README example writes `groupListType=cumsum` as a word; attr is 0/1. Kernel 0=cumsum, 1=count.
2. Default type 0 vs dispatch default type 1.
3. Output `y` vs combine `expand_x` same “expert rows” story, **different FFN stage and dtype**.
