# Cross-source map: vllm-ascend ↔ 13 Cases / 6 Knowledge

Scout only. Official source: `vllm-project/vllm-ascend` `main` @ `d8268faea90dd9b999a0a82f6d133751d57b5770`.

Rule: **framework-used kernel ≠ new Case**. Filter with live teaching assets first.

```text
13 Cases + 6 Knowledge
    → absorb torch_npu / packaging / FIA / GMM / EP / MC2 jobs
    → leftover = indexer / selection producer (maybe)
                 + framework custom-op **surfaces** (maybe Knowledge)
```

Do **not** reopen GEMM / MoE / existing Sparse **consumer-side** teaching / FA Knowledge / MC2 Knowledge / ops-transformer.

Already taught: `select_idx` → Sparse Attention (consumer).  
P1 is the missing producer: Indexer → `select_idx` / `sparse_indices`. Do not merge them.

---

## Against Attention

| vllm-ascend pattern | Sparse Case | FA Knowledge | 49 / FIA contrast |
| --- | --- | --- | --- |
| `kv_quant_sparse_attn_sharedkv` (A5 ACLNN) | **consumer contrast** first；likely Sparse-family；**not** frozen alternative | FA loop **if** QK→softmax→PV (unresolved) | not FIA |
| `sparse_attention_score` (`selectIdx`) | **same family** (SASA); A3 ACLNN list; MiniMax binding | selection **not** in Knowledge | not FIA |
| `npu_sparse_flash_attention` | consumer sibling | same | not FIA |
| `vllm_quant_lightning_indexer` | **upstream producer** of `sparse_indices` | **not absorbed** (`select_idx` excluded) | not 49 |
| `torch_npu.npu_lightning_indexer` | same producer job, torch_npu impl | not absorbed | not 49 |
| MLA / `npu_fused_infer_attention_score` | no | **peer** | **absorbed** |
| `block_table` | paged addressing | **logical→physical** | FIA PA same idea |
| `AscendSFAIndexerBackend` | no kernel | no | no |

**Do not write:** “又有 Sparse Attention，开 Case。”

```text
Indexer  --produces-->  sparse_indices / select_idx
                            ↓
                    Sparse FA consumer
```

If indexer is a new mechanism, it is a **new Case of its own**. Do not merge with the Sparse Case.

sgl lightning indexer was **950 CMake off**. This tree has `AddConfig("ascend950")` + `arch35` + A5 `build_aclnn.sh` list. That is why indexer is P1, not a frozen Attention leftover.

---

## Against MoE (A / B / C / D + Layer 1/2)

| vllm-ascend pattern | A | B | C | D |
| --- | --- | --- | --- | --- |
| `npu_moe_distribute_dispatch(_v2)` / `combine(_v2)` | not InitRouting chain | **alternative** | consumer of recv rows | split form |
| `npu_grouped_matmul` / `swiglu_quant_v2` | no | no | **alternative** | GMM inside fused |
| `FusedMC2` + `cann_ops_transformer` | no | internalized | internalized | **alternative** |
| in-tree `dispatch_ffn_combine*` | no | no | no | A2/A3 only；950 skip |
| `npu_moe_gating_top_k` | gating contract | same | same | same |
| EPLB Python controller | no | **packaging / scheduling** until a new map appears | no | no |

**Do not write:** InitRouting → Dispatch as vLLM’s production chain. EP still consumes token-major hidden + expert ids (Layer 1).

If the path is `Python Buffer → torch.ops / torch_npu → existing dispatch / GMM`, **Layer 1/2 + B/C/D absorb it**.

---

## Against GEMM (43 / 60 / 66 / 68 / 53 / 73)

Cube inside GMM / indexer / sparse FA is **not** a new GEMM Case. MX A5 GMM = 53/C. `group_list` = 60.

---

## Against MC2 Knowledge (RS / M2 Cases)

| vllm-ascend pattern | Fill | New Case? |
| --- | --- | --- |
| `npu_mm_reduce_scatter_base` | ownership / progress / visibility / transform | **no** (M1 RS job) |
| A5 `comm_mode="ai_cpu"` (CCU_SCHED → `ccu`) | same four questions; job ≠ mechanism | **no** |
| MoE `MoECommType.MC2` | **not** M1 AG/RS/AR；it is EP distribute (B) | **no** |

Name “MC2” in vLLM MoE is **EP fused comm**, not the matmul-collective family. Do not open a third MC2 Case for either.

---

## Against GMM finalize-routing (just frozen)

No `grouped_matmul_finalize_routing` / CATLASS 71 cousin found as a named vLLM path this Scout. Stay absorbed in 60/A/C/53.

---

## What this source might still add

### Maybe Case (P1)

**Selection producer / lightning indexer** — how vLLM produces `sparse_indices` for the Sparse consumer. Contrast sgl indexer (deferred) and Sparse Case **consumer only**.

### Maybe Knowledge / workflow (P3), never `cases/compute/`

```text
vLLM semantic op
    → DeviceOperator chooses torch_npu vs _C_ascend
    → enable_custom_op / vendor OPP / torch extension
    → tiling → kernel
```

Preset path names are **forbidden** at Scout. A5 `enable_custom_op()==False` is load-bearing evidence for this layer.

### Not a Case

FIA/MLA, sparse FA consumer (until contrast fails), MoE B/C/D packaging, TP RS, EPLB Python, LoRA, sampling, 310P, A2-only `dispatch_ffn_combine`.

---

## Work order (frozen for this source)

```text
P1  indexer / selection contract + sparse consumer contrast
P2  MoE path vs A/B/C/D  (expect absorb)
P3  two custom-op surfaces（vendor ACLNN vs `_C_ascend`；expect Knowledge-shaped, not one chain）
```
