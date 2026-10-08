# Sparse attention arch35 — live-path wrap-up

Pins which kernel Host actually launches on 950. First dataflow: `sparse_attention_score_arch35.md`. Case: `cases/compute/attention/ascend950-block-sparse-paged-attention/` (`accepted`).

```yaml
status: live-path-pinned
source_identity:
  registry_ref: sgl-kernel-npu
  scanned_commit: 8d76a8da75315ae9add5cfd2458b8028289f1ada
  host_tiling: csrc/sparse_attention_score/op_host/tiling/sparse_attention_score_tiling.cpp
  kernel_entry: csrc/sparse_attention_score/op_kernel/sparse_attention_score.cpp
  live_qk: csrc/sparse_attention_score/op_kernel/attn_infra/gemm/block/block_mmad_qk_arch35_ABf16_C_to_UB.hpp
  sibling_not_950: csrc/sparse_attention_score/op_kernel/attn_infra/gemm/block/block_mmad_qk.hpp  # A2 BlockMmad only
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

Same rule as MegaMoe: Host selected path is SoT. Compiled-but-unselected files are siblings.

---

## Host → kernel (950)

`SOC_VER_950_CODE = 4` (`tiling.h` comment: **ascend910_93 and ascend950**). `GenerateTilingKey` when `socVer == 4`:

| dtype | tilingKey | kernel (`__CCE_AICORE__==310`) |
| --- | --- | --- |
| bf16, D=128, blockSize=128 | `10002` | `SasaInferIntfRegular` → `SasaRegularKernelArch35` |
| fp16, same | `10001` | same Regular |
| fp8 e4m3, same | `10003` | `SasaInferInterfaceFullQuant` → `SasaFullQuantKernelArch35` |
| anything else | **falls through to `10001`** | Regular fp16 template |

`10004` / `10005` (D=64) exist in `tilingkey.h`. Host **never returns them**. Dead keys.

Entry: `KERNEL_TYPE_MIX_AIC_1_2`. Task split: `totalTaskNum = totalQTokens * kvHeads`, `blockDim = min(totalTaskNum, aicNum)` — one `(qToken, kvHead)` per task.

**950 Case teaching path:** Regular **bf16 `10002`** (sglang MiniMax-M3). fp16 is the same class. fp8 FullQuant is a **dtype variant** of the same outer sparse loop, not a second family.

---

## Cube Block is not `block_mmad_qk.hpp`

`attn_infra/gemm/block/block_mmad.hpp`:

- `__CCE_AICORE__==310` includes `block_mmad_qk_arch35_ABf16_C_to_UB.hpp` + PV sibling
- `__CCE_AICORE__==220` includes `block_mmad_qk.hpp` / `block_mmad_pv.hpp` (`gSelectIdx` gather **inside** BlockMmad)

950 Regular QK is `BlockMmadTla<MmadAtlasA5BsaQK, Shape<128,128,128>, …>`. Physical-id gather is **already done** in `SasaRegularKernelArch35` (`validPhysicalIds[]` → `gmOffsetK`). Do not teach A2 in-block `gSelectIdx` as the 950 mechanism.

Interface L1/L0 tiles are hardcoded `128³`, matching Host `qBaseTile=128` when `embed<=128` and product `blockSize=128`.

Host also sets `pL1BufNum = 3` (`PRE_LAUNCH+1`). Matches kernel `MAX_CROSS_CORE_BUF_STAGES`.

---

## GQA is `groupSize = numHeads / kvHeads`

Host: `tilingData.groupSize = numHeads / kvHeads`. Kernel `rowNum = groupSize`: one task’s Cube QK/PV covers that many Q heads sharing the KV head. Regular does them as **one M tile**. FullQuant loops `headInGroup` (same map, finer M). Not a new sparsity mechanism.

---

## `inner_precise`

Copied into the tiling blob (`innerPrecise`). **No kernel read** in this tree (only the struct field). README still says sglang must pass `4`; treat that as a **wrapper/golden contract**, not a 950 tiling-key branch. Do not invent a second live kernel from `inner_precise`.

---

## FP8 vs Regular

Same: logical `select_idx` → `block_table` → physical page → outer KV-block loop → QK / online softmax / PV / rescale-O.

Different: FullQuant takes `q/k/v_dequant_scale`, output element is `SMDtype` (fp16), and walks GQA heads one-by-one. **Variant**, not a second Case.

---

## Case gate (closed)

Live path pinned. Case **accepted**. Canonical Regular bf16 `10002`. Three evidence paths exist on `sgl-project/sgl-kernel-npu` `main` @ `ee334193e5278d7b3a64afcc2e3ee5243133c4a0` and are byte-identical to scanned fork `8d76a8da75315ae9add5cfd2458b8028289f1ada`.
