# Cross-source map: sgl-kernel-npu ↔ existing Cases

**Frozen** SoT for this sgl-kernel-npu stage (2026-08-21). Live Cases: CATLASS **43, 60, 66, 68, 53, 73** and ops-transformer MoE **A, B, C, D (MegaMoe)**.  
Layer 1/2 MoE Knowledge is **frozen baseline**. This map does not reopen them.

Rule: **same Block / same CATLASS header / same EP pair names ≠ new Case**. “新仓库中的新实现”不等于“新的工程机制”。

---

## Against GEMM Cases (43 / 60 / 66 / 68 / 53 / 73)

| sgl-kernel-npu pattern | 43 | 60 | 66 / 68 | 53 | 73 |
| --- | --- | --- | --- | --- | --- |
| `catlass_matmul_basic` | **duplicate** wrapper | no | no | no | no |
| `catlass_gmm_fp8` (A3-only) | Cube inside | **sibling** name | no | dtype cousin | no |
| `fused_deep_moe` A5 GMM stages | Cube still GEMM | **extension** (grouped expert M) | not the teaching | **extension** if MX/w8/w4 | unknown |
| `sparse_attention_score` | Cube tiles ≠ 43 Case | no | no | fp8 attention ≠ 53 GEMM | no |
| DeepEP dispatch/combine | no | no | no | quant on tokens | no |

Do **not** open a second Basic Matmul, GMM, StreamK, SplitK, MX Identity, or FullLoadA Case from this repo.

---

## Against frozen MoE Cases (A / B / C / D)

| sgl pattern | A local permute | B EP dispatch/combine | C GMM SwiGLU MX | D MegaMoe |
| --- | --- | --- | --- | --- |
| DeepEP `Buffer.dispatch/combine` | **not** predecessor | **alternative** (same routing loop) | consumer of local expert compute | split form of the fused op |
| `fused_deep_moe` A5 | A5 public path does not wire shared-expert; InitRouting **not** in FuseMode=1 | dispatch+combine **internalized** | GMM1+SwiGLU+GMM2 **internalized** | **alternative** fused EP+FFN |
| `DISPATCH_FFN_COMBINE` | InitRoutingQuantV2 **inside** kernel | EP inside | FFN inside | sibling fusion; **950 skipped** |
| MoE gating | upstream contract only (same as ops-transformer) | same | same | same |

**Do not write:** InitRouting → Dispatch as a production chain. DeepEP dispatch still consumes token-major `x` + expert ids.

**A5 CMake fact:** fused 950 includes **system** `ops-transformer` `moe_distribute_base.h`. Contrast verdict: SGLang host / fused packaging over the **same EP loop as B** (dest-push), not a new protocol class. Inner morphology is B-push, not MegaMoe dest-pull.

---

## Against ops-transformer backlog (not Cases)

| ops-transformer backlog | sgl counterpart | Relation |
| --- | --- | --- |
| FIA | `sparse_attention_score` | **FA-pipeline peers**（对照 passed）；FIA `sparse_mode` ≠ Sparse `select_idx`；不开 FIA Case |
| MLA / lightning indexer | `sgl-mla-preprocess`, `sgl-lightning-indexer`, `sgl-attn-laser-ada` | **deferred**；950 CMake off / 未对照 |
| `opsxf-deepep-a3` | this repo’s `csrc/deepep/` | **this is the live DeepEP**; ops-transformer DeepEP dirs were fork/A3-only |
| MC2 matmul-collective | none obvious in this Scout | stay ops-transformer backlog |
| GMM finalize-routing | none as a named op | stay ops-transformer backlog |

---

## Cross-source outcome

### Existing mechanisms, new production evidence

1. **SGLang DeepEP Buffer + strategy switch**  
   — Case B 的 serving / packaging alternative；  
   MTE dest-push routing loop 没变，不新建 Case。

2. **A5 fused MoE**  
   — Case D 的 fused EP+FFN alternative；  
   使用 B-style dest-push，而非 MegaMoe dest-pull；  
   Layer 2 taxonomy 得到第二个 source 的验证，不新建 Knowledge。

### New mechanism (Case)

3. **Block-sparse paged attention**  
   — 950-built；`select_idx` 仍是当前唯一没被 FA Knowledge 吸收的 Attention 机制。

Not a Case (do not open): CATLASS wrappers, LoRA/cache/tree, helloworld.

---

## Status (this stage, frozen)

**DeepEP contrast — accepted.** No new Case / no new Knowledge. `close_reads/deepep_a5_cross_source_verdict.md`.

**Sparse attention — Case `accepted`.** Canonical Regular bf16 `10002`. `cases/compute/attention/ascend950-block-sparse-paged-attention/`. Upstream: `sgl-project/sgl-kernel-npu` `main` @ `ee334193e5278d7b3a64afcc2e3ee5243133c4a0`. Selection stays Case-only.

**Attention contrast — `review: passed`.** CATLASS 49 / FIA 950 are FA-pipeline **peers**, not Sparse alternatives. No 49/FIA Case. Knowledge: `knowledge/engineering-experience/fa-pipeline-online-softmax-and-paged.md` (`accepted`).

Out of this source’s Case queue: `DISPATCH_FFN_COMBINE` (950 skip), A2 `ops2`, MLA/indexer/FLA (950 CMake off), CATLASS wrappers (43 clone).
