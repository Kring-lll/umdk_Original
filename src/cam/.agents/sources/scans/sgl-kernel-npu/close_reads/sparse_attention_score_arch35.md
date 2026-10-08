# Sparse attention score (arch35) — first close-read

New family. Not 43, not FIA, not CATLASS 49. Lightning indexer is **out** (950 CMake off); `select_idx` is an **external contract**.

This file answers the first dataflow questions. Live Host→kernel pin: [`sparse_attention_score_arch35_live_path.md`](sparse_attention_score_arch35_live_path.md). Case Card: `cases/compute/attention/ascend950-block-sparse-paged-attention/` (`status: accepted`).

```yaml
source_identity:
  registry_ref: sgl-kernel-npu
  scanned_commit: 8d76a8da75315ae9add5cfd2458b8028289f1ada
  live_950:
    kernel: csrc/sparse_attention_score/op_kernel/arch35/sparse_attention_score_kernel_arch35.h
    host: csrc/sparse_attention_score/op_host/sparse_attention_score.cpp
    doc: csrc/sparse_attention_score/README.md
  tiling_key_950: 10002 bf16 Regular (teaching); 10001 fp16 Regular; 10003 fp8 FullQuant
  sibling_not_950:
    - arch22 SasaInferIntfRegularArch22
    - attn_infra/.../block_mmad_qk.hpp  (A2 gSelectIdx-in-BlockMmad)
  cmake: built on ascend950 (not SGL_KERNEL_ENABLE_A3_ONLY_OPS)
  review: first-read-passed
  live_path: pinned
  case: accepted
  case_card: cases/compute/attention/ascend950-block-sparse-paged-attention/
  upstream_presence:
    verified: true
    repo: sgl-project/sgl-kernel-npu
    branch: main
    commit: ee334193e5278d7b3a64afcc2e3ee5243133c4a0
    scanned_fork_commit: 8d76a8da75315ae9add5cfd2458b8028289f1ada
    paths_verified:
      - csrc/sparse_attention_score/op_host/tiling/sparse_attention_score_tiling.cpp
      - csrc/sparse_attention_score/op_kernel/arch35/sparse_attention_score_kernel_arch35.h
      - csrc/sparse_attention_score/op_kernel/attn_infra/gemm/block/block_mmad_qk_arch35_ABf16_C_to_UB.hpp
```

`execution.task_type: mix`. `engines: [cube, vector]`.

---

## Graph (this op)

```text
select_idx [kvHead, maxQSeqlen, topK]     # logical block id; -1 skip
        ↓  skip logicalId < 0
block_table[batch, logicalId]             # logical → physical page
        ↓
paged K/V  key/value[physicalId, blockSize, kvHead, D]
        ↓  Cube QK   (BlockMmadQK)
S  (UB)
        ↓  Vector online softmax  (EpilogueOnlineSoftmax, first-block vs rest)
P  (L1)
        ↓  Cube PV   (BlockMmadPV)   delayed by PRE_LAUNCH=2
O_tmp
        ↓  Vector rescale-O  (EpilogueRescaleO)  across sparse blocks
attentionOut [T, N, D]
```

Indexer is **not** in this kernel.

---

## Where sparsity happens

**Outer KV-block loop**, before Cube MMA.

For each task `(qToken, kvHead)` the A5 kernel (`source-code` arch35):

1. Reads `validTopK` from `select_num_idx` if present.
2. For `i in 0..validTopK`: `logicalId = select_idx[kvHead, qToken, i]`; skip if `< 0`.
3. `physicalId = block_table[batchIdx, logicalId]`.
4. Last logical page of the sequence may be a **partial** tile (`lastBlockTileSize`).
5. Loop Cube QK / softmax / Cube PV **only** over `actualLoopNum` surviving physical pages.

Not: dense FA with a mask inside MMA. Not: 43 MN GEMM.

`block_mmad_qk.hpp` indexes K by `gSelectIdx` **inside BlockMmad**. That file is included only for `__CCE_AICORE__==220`. Live 950 uses `block_mmad_qk_arch35_ABf16_C_to_UB.hpp`; physical ids are resolved **before** Cube (`validPhysicalIds[]`). See live-path wrap-up.

---

## `select_idx` vs `block_table`

| tensor | meaning |
| --- | --- |
| `select_idx` | **logical** KV-block id per `(kvHead, query token, topk slot)`. `0-based`. `-1` = unused. **Not** a token range, **not** already a physical page. |
| `block_table` | paged map: **logical block → physical block** for that batch. |
| `key` / `value` | `[blockNum, blockSize, kvHead, D]` addressed by **physical** id. |

Address:

```text
physicalId = block_table[batchIdx * maxBlocksPerBatch + logicalId]
gmOffsetK/V = physicalId * (blockSize * kvHeads * D) + kvHeadIdx * D
```

`historyLen + qTokenInBatch` decides which logical id is the tail page (partial `blockSize`).

---

## Cube / Vector pipeline

MIX. Cube: QK then (after softmax) PV. Vector: online softmax → L1 `P`, then rescale-O.

`PRE_LAUNCH = 2`: PV lags QK by two sparse blocks. `CrossCoreFlag` `mm1ToSm` / `smToMm2` / `mm2ToRe`. UB S / O_tmp ping-pong (`UB_S_OTMP_BUF_STAGES = 2`).

Online softmax **state stays on Vector** across the sparse-block loop: first block `kvBlockIdx == 0` initializes; later blocks go through `EpilogueRescaleO`. Host also allocates internal `softmaxLse [T,N,1]` fp32; it is **not** returned (`op_host` comment).

`inner_precise=4` is the production precision path (README). `0` is a known trap.

---

## Not these Cases

| existing | why not |
| --- | --- |
| 43 | Cube tiles exist; teaching is sparse paged FA, not MN GEMM |
| 60 / 53 / A–D | no expert grouping / EP |
| CATLASS 49 / ops-transformer FIA | alternative attention stacks; do not equate until a later contrast |

---

## Case status

Card: `cases/compute/attention/ascend950-block-sparse-paged-attention/` (`accepted`). Canonical Regular bf16 `10002`; fp16/fp8 are siblings. content_review passed. Upstream SoT `sgl-project/sgl-kernel-npu` `main` @ `ee334193e5278d7b3a64afcc2e3ee5243133c4a0`. No Knowledge (single Attention Case).
