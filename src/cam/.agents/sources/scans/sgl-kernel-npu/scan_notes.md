# sgl-kernel-npu Scout notes

Status board (2026-08-21):

| item | status |
| --- | --- |
| sgl DeepEP split contrast | **accepted** |
| sgl fused A5 contrast | **accepted** |
| DeepEP cross-source verdict | **accepted** → no Case / no Knowledge |
| SparseAttention first read | **passed** |
| SparseAttention live-path | **pinned** (`close_reads/sparse_attention_score_arch35_live_path.md`) |
| SparseAttention Case | **accepted** `cases/compute/attention/ascend950-block-sparse-paged-attention/` |
| sgl-project upstream verify | **done** for Sparse Attention 3 evidence paths (`main` @ `ee334193e5278d7b3a64afcc2e3ee5243133c4a0`) |

SGLang MoE did **not** inflate the knowledge base. The only new family on this source is Sparse Attention. That is the third-source test passing: A/B/C/D + two MoE Knowledge layers were the right grain.

## Method

- Inventory by **family**. Candidates by **engineering pattern**.
- Catalog file order = `bucket` then `scout_rank` desc. Work order is not scout_rank.
- Communication: **job** ≠ **mechanism** ≠ serving **packaging**.

## Identity

- Official SoT: `sgl-project/sgl-kernel-npu` `main`.
- This checkout: `wangyibo1005/sgl-kernel-npu` `main` @ `8d76a8da75315ae9add5cfd2458b8028289f1ada`.
- Sparse Attention 3 evidence paths: `upstream_presence.verified = true` against official main @ `ee334193e5278d7b3a64afcc2e3ee5243133c4a0` (byte-identical to scan). Whole-tree fetch still not done.

## DeepEP A5 (done)

`close_reads/deepep_a5_split_vs_case_b.md` — Case B alternative.  
`close_reads/fused_deep_moe_a5_vs_megamoe.md` — Case D alternative; Layer 2 survives.  
`close_reads/deepep_a5_cross_source_verdict.md` — no Case, no Knowledge.

## Sparse Attention (Case accepted)

First read: `close_reads/sparse_attention_score_arch35.md`  
Live path: Regular bf16 `10002` / `SasaRegularKernelArch35`; Cube Block is arch35 `BlockMmadTla`, **not** A2 `block_mmad_qk.hpp`; fp8 FullQuant is a dtype variant; GQA is `groupSize = numHeads/kvHeads`.  
Card: `cases/compute/attention/ascend950-block-sparse-paged-attention/`. No Knowledge.

## Still deferred

CATLASS wrappers. A3 MLA / indexer / FLA. DeepEP A2. `DISPATCH_FFN_COMBINE`.
