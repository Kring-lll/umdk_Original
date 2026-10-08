# vllm-ascend Scout notes

Status: **scout passed** (2026-08-21). Source 4. Do not write a Case or Knowledge this round.

## Identity

- Official SoT: `vllm-project/vllm-ascend` `main`.
- This checkout: **same** (`origin` is official). HEAD `d8268faea90dd9b999a0a82f6d133751d57b5770`.
- License Apache-2.0.

No fork-identity tail item.

## Method

- Inventory = **Integration Surface** first, then kernel families.
- Candidates = **engineering pattern**, not op count.
- Filter with 13 Cases + 6 Knowledge **before** proposing a Case.
- `build_aclnn.sh` A5 `CUSTOM_OPS_ARRAY` is the 950 ACLNN SoT — not A3’s list, not a lone `AddConfig("ascend950")`.
- Catalog display order = work line **P1 → P2 → P3**. `scout_rank` guides close-read inside a line; it is not a file sort key.

## Live gates to keep in front of Agents

1. **`enable_custom_op()` is False on A5** (`utils.py`, issue `#7157`). Kernels in `csrc/` can still compile. Do not claim in-tree ops are default A5 inference until close-read. **Which of the two custom-op surfaces this gates, and whether `_C_ascend` calls bypass it, is unresolved.**
2. **A5 ACLNN list does not include `sparse_attention_score`.** That op is on the **A3** list. MiniMax still binds `npu_sparse_attention_score`. A5 sparse consumer in the ACLNN package is `kv_quant_sparse_attn_sharedkv`.
3. **`kv_quant_sparse_attn_sharedkv` is a likely Sparse-family consumer, not a frozen alternative.** Confirmed: A5-built, sparse-selection-style input. Unresolved until P1: selection coordinate, page lifetime, QK/softmax/PV, full absorb.
4. **SFA vs DSA indexer impls differ:** SFA A5 adaptor uses `torch_npu.npu_lightning_indexer*`; DeepSeek-V4 `select_topk` uses `_C_ascend.npu_vllm_quant_lightning_indexer`. Do **not** seek one global “A5 uses X indexer”. Path matrix is in `close_reads/indexer_a5_runtime_paths.md`.
5. vLLM `MoECommType.MC2` = EP distribute (Case B), **not** MC2 Knowledge’s matmul-collective family.
6. `AscendSFAIndexerBackend` is cache-only. It does not produce `select_idx`.

## Priority (this source)

| pri | pattern | expected |
| --- | --- | --- |
| P1 | lightning indexer / selection producer | maybe Case (**indexer only**) |
| P1 | sparse FA consumer | contrast first; **do not freeze alternative** |
| P1-next | compress epilog | after indexer; KV-storage adjacent, not “who produces the map” |
| P2 | MoE dispatch / GMM / MegaMoe | absorb B / C / D |
| P2 | `npu_mm_reduce_scatter_base` | fill MC2 四问；**no Case** |
| P3 | two custom-op surfaces (vendor ACLNN vs `_C_ascend`) | maybe Knowledge / workflow；**not** `cases/compute/`；**not** one serial chain |

## Do not

- Open a second Sparse Case because `sparse_attention_score` exists.
- Freeze `kv_quant_sparse_attn_sharedkv` as Sparse Case `alternative` before P1 close-read.
- Merge indexer + sparse FA into one Case.
- Expand P1 into the whole DSA KV-cache pipeline via compress epilog.
- Collapse vendor ACLNN and `_C_ascend` into one lifecycle.
- Open a third MC2 Case.
- Reopen ops-transformer / M3 / CATLASS 71.
- Preset `framework-to-custom-op-contracts.md` as if it already exists.
- Scout LoRA, sampling, model patches, 310P, A2-only `dispatch_ffn_combine` first.

## Next

**Source 4 FREEZE.** Do not keep scanning this repo.

Leftover only: P1 indexer canonical runtime-evidence（`.so` load / OPP resolve）— explicit pending, not a probe-now item.

Compress epilog **PASSED / result A**. Do not bind it into Indexer Case. Do not expand `compressor` / `store_kv_block`.
