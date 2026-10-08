# Cross-source map: ops-transformer ↔ CATLASS Cases

Scout only. Existing Cases: **43, 60, 66, 68, 53, 73** plus MoE **A / B / C / MegaMoe (D)**.  
CATLASS catalog IDs 49/65/70/71/19 are **not Cases**; they are noted only so Agents do not invent a sixth GEMM Case out of FA/MLA.

**MoE line frozen 2026-08-21.** Attention freeze same day. MC2 first phase frozen. GMM finalize-routing contrast **passed** — 60/A/C/53 fused composition，**no Case**。**ops-transformer source frozen.** 950 DeepEP is **not** on this checkout; see `sources/scans/sgl-kernel-npu/`。

Rule used here: **same Block / same CATLASS header ≠ new Case**. Production wrapping, tiling keys, and fused epilogues are extensions until a close-read proves a new mechanism.

---

## Against the six live Cases

| ops-transformer pattern | 43 Cube MN | 60 GMM slice-M | 66 StreamK | 68 SplitK | 53 MX block-scale | 73 full-K A + cross-N reuse |
| --- | --- | --- | --- | --- | --- | --- |
| `moe` gating / init-routing / finalize | unrelated | unrelated | unrelated | unrelated | unrelated (except v3 MX **dtype** on routing, not GEMM) | unrelated |
| `grouped_matmul` (plain, M-group) | baseline Cube path inside cgmct | **extension** (production groupList + ASWT + quant branches) | headers exist; **live unknown** | K-group / `SplitKSingleX*` is **capacity K-split**, not 68 | **extension** on MX branches (Blaze/cgmct `block_mmad_mx`) | Host `TryFullLoadA` / kernel `FullLoadA` is **sum vs L1** tiling admission — **extension of the idea**, not 73’s example kernel |
| `grouped_matmul_swiglu_quant_v2` | Cube still GEMM | **extension** (grouped M + fused AIV epilogue) | not the teaching | not the teaching | **extension** (950 MX + e8m0 group-32 in README) | tiling `TryFullLoadA` present — same caveat |
| `grouped_matmul_finalize_routing` | Cube GEMM + AIV scatter-add | **extension** | no | no | MX path on 950 | no |
| `grouped_matmul_add` K-group | Cube | grouping on **K**, not 60’s M-concat | **not** 66 remainder-tile | **not** 68 every-MN + workspace planes | no | no |
| `moe_distribute_*` EP | no | no | no | no | quantMode may carry MX tokens | no |
| `mega_moe` | Cube GEMM inside | grouped expert MM | no | no | **yes** (MXFP8 gs=32 in README) | unknown without close-read |
| FIA / MLA / indexer | no | no | no | no | sparse/quant FA is not 53 | no |
| MC2 AG/AR/RS/A2A+Matmul | Cube matmul templates (`mc2_mat_mul_asw_*`), **not** 43 Case | GMM+A2A is grouped+comm | no | no | some quant/MX modes | no |

### Shared vs new (short)

**43.** Production Cube still GM→L1→L0→Mmad→Fixpipe. Do **not** open a second Basic Matmul Case. cgmct / `gmm_infra` / `mc2_mat_mul_asw_*` are the place to learn how 43’s layering shows up in aclnn ops — that is **knowledge / close-read of GMM**, not a 43 clone.

**60.** `grouped_matmul` arch35 non-quant + ASWT + prefix-sum `groupList` is the production counterpart. Extra: quant/antiquant/weight-NZ, K-group, fused epilogues. Close-read GMM as **extension**, only split a new Case if the fused epilogue (SwiGLU/MX, finalize-routing) is the teaching.

**66 / 68.** Do not treat GMM `groupType=2`, `SplitKSingleX*`, or cgmct `KernelMultiBlockStreamK` / `BuiltInStreamKScheduler` as StreamK/SplitK Cases. Those names are **tiling/capacity** tools. 66 = remainder MN tiles’ K + workspace slots + AIV reduce. 68 = every MN tile’s K + full output-C planes. Unverified whether production GMM ever launches those CATLASS kernel shapes. **Deferred until a live-path close-read.**

**53.** MX group-32 + e8m0 appears in `grouped_matmul` README, `grouped_matmul_swiglu_quant_v2` 950 path, `mega_moe` phase-1, and some MC2 quantMode. Production adds grouping, MIX AIC→AIV, fused SwiGLU/finalize. Do **not** clone 53; close-read the **fused MX GMM** as extension.

**73.** GMM `FullLoadA`: `curBaseM * AlignUp(k,baseK) + baseN * baseK * depthB1 < L1_SIZE` (`source-code` `grouped_matmul_pre_tiling.h`). That is a **sum** check (A full-K + B), closer to 73’s `CanImplement` than to README half-L1. Cross-source value: production already has the Agent trap (named vs checked). **Do not open a second FullLoadA Case.** Optionally fold into CanImplement knowledge after GMM close-read.

---

## CATLASS catalog (not Cases) — do not confuse with the six

| CATLASS catalog | ops-transformer counterpart | Relation |
| --- | --- | --- |
| 49 FA infer | `fused_infer_attention_score` (950 PFA/IFA) | **FA-pipeline peers**（contrast passed；不开 Case）；KV-domain 不同 |
| 70 prefill | same FIA family | **sibling of 49**；尚未对照；默认不开 Case |
| 65 MX GMM + SwiGLU + MX quant | `grouped_matmul_swiglu_quant_v2` arch35; inside `mega_moe` | **alternative / production extension** |
| 71 MX GMM finalize routing | `grouped_matmul_finalize_routing` | **fused composition of 60/A/C/53**；contrast passed；**no Case**；map = `row_index[slot]→token`，≠ Case A `expanded_row_idx` |
| 19 MLA | `mla_prolog_*` + sparse FA stack | **alternative**; experimental `typhoon_mla` is the only explicit CATLASS wrap |

---

## New mechanisms (not in the six)

1. **MoE token routing** — sort/expand/scatter-add, AIV, `groupList` **consumer** for GMM (`moe/`)
2. **EP AllToAllV + HCCL/MTE window** — `moe_distribute_*`, DeepEP (`mc2/`)
3. **通算融合** — AG/AR/RS/A2A overlapped with matmul (`mc2/`)
4. **MLA / lightning indexer** — still deferred；FIA 本身已是 FA-pipeline peer，不开 Case
5. **API aliasing** — `MoeTokenPermute` → `MoeInitRoutingV3` on 950 (Agent trap: two names, one kernel)

---

## Recommended first MoE close-read **set** — **done / frozen**

Read these together as the **on-device MoE FFN + EP** path on 950. Outcome: Cases A–D + Layer 1/2 Knowledge. **Do not treat this table as the next close-read queue.**

```text
gating_top_k
    → init_routing_v3          # expand tokens, optional quant, expert counts
        → moe_distribute_dispatch_v2   # AllToAllV EP (quantMode 0–4)
            → grouped_matmul_swiglu_quant_v2   # local expert FFN (MX on 950)
        → moe_distribute_combine_v2
    → finalize_routing_v2      # weighted unpermute to token order
```

**Why a set**

| Step | What the Agent learns that the others do not |
| --- | --- |
| Gating | logits → expert ids / weights; **no** Cube |
| Init routing | `expanded_x` + `groupList` / counts that **GMM consumes**; permute aliases |
| EP dispatch/combine | tokens **leave the device view**; window/HCCL; pair APIs; not `moe_re_routing` |
| GMM SwiGLU MX | only Cube step; talks to **60 + 53** without cloning them |
| Finalize | inverse of init-routing; CATLASS 71 / ops GMM-FR is a **fused** cousin of the **loop**, not this op and **not** a direct `expanded_row_idx` wire |

`mega_moe` is the **fused substitute** for dispatch+GMM+SwiGLU+GMM+combine. Case: [`ascend950-mega-moe-fused-ep-ffn`](../../cases/compute-communication/moe/ascend950-mega-moe-fused-ep-ffn/case.md). Close-read: [`mega_moe.md`](close_reads/mega_moe.md).

Out of this set: DeepEP (A3, other stack), `*_with_ep` (no 950), routing-map (parallel encoding), `grouped_matmul_add` (K-group, not expert FFN).
