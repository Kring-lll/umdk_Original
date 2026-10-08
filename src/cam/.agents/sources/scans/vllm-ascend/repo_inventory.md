# vllm-ascend Repository Inventory

Scout only. Does not modify `cases/` / `knowledge/` / `skills/`.

## Identity

| Field | Value |
| --- | --- |
| Official repo | `vllm-project/vllm-ascend` (`https://github.com/vllm-project/vllm-ascend.git`) |
| Scan path | `/home/wangyibo/vllm-ascend` |
| Origin on this checkout | `https://github.com/vllm-project/vllm-ascend.git` (**official**, not a fork) |
| HEAD | `d8268faea90dd9b999a0a82f6d133751d57b5770` |
| Branch | `main` tracking `origin/main` |
| Date | 2026-08-21 |
| License | Apache-2.0 — **clear** |

This Scout describes **this official checkout**. Do not re-introduce a fork-identity tail item.

vLLM community recommended Ascend backend. Delivery unit is a **plugin**: Python model runner + `torch_npu` + optional in-tree ACLNN/AscendC custom ops packaged into the wheel (`vllm_ascend_C` + `_cann_ops_custom` vendor OPP).

It is **not** a Cube template zoo and **not** a replacement for ops-transformer / CATLASS Cases.

## What it solves

Run vLLM on Ascend NPUs. Engineering question for AscendWeaver:

```text
vLLM semantic op
    → Ascend backend chooses implementation
    → Python / torch custom op boundary
    → ACLNN / custom op registration
    → Host tiling
    → AscendC kernel
```

Kernels in `csrc/` exist to **short-circuit** ATB → torch_npu → CANN upgrade. That does **not** mean every named op is a new mechanism.

## Contains real AscendC kernels?

Yes, under `csrc/{attention,moe,gmm,mc2}/` (`op_host` + `op_kernel`, often `arch35/`). Bound in `csrc/torch_binding.cpp` as `torch.ops._C_ascend.*`.

Two compile surfaces (do not collapse):

| Surface | What |
| --- | --- |
| `csrc/build_aclnn.sh` | SOC-filtered ACLNN vendor package → `vllm_ascend/_cann_ops_custom` |
| `setup.py` `vllm_ascend.vllm_ascend_C` | CMake torch extension; registers adapters in `torch_binding.cpp` |

A5 (`SOC_VERSION` matching `^ascend950`) sets `SOC_ARG=ascend950` and a **fixed** `CUSTOM_OPS_ARRAY`. That list is the 950 ACLNN SoT, not `AddConfig("ascend950")` on a def.cpp, and not the A3 list.

## Target hardware

| Product | Support |
| --- | --- |
| Ascend 950 / A5 (`ascend950*`, `AscendDeviceType.A5`) | first-class for this Scout; `A5DeviceAdaptor` |
| Atlas A3 (`ascend910_93`) | first-class plugin; larger custom-op list (includes `sparse_attention_score`) |
| Atlas A2 (`ascend910b`) | historical / still live; **not** 950-first |
| 310P | `vllm_ascend/_310p/`；**deferred** |

**950 runtime gate (must pin):** `enable_custom_op()` currently returns **False** when `get_ascend_device_type() == A5` (issue `#7157`, “temporarily disable all custom ops”). In-tree 950 kernels **exist**; this Scout does **not** claim they are the default A5 inference path until a close-read proves a bypass. `A5DeviceAdaptor` and DeepSeek-V4 indexer still **call** `torch.ops._C_ascend.*` — that tension is P1/P3, not a Case.

## Kernel / test / doc locations

```text
vllm_ascend/platform.py          # NPUPlatform
vllm_ascend/worker/              # V1 runner; worker/v2/
vllm_ascend/attention/           # backends: attention_v1, mla, sfa, dsa, fa3
vllm_ascend/ops/fused_moe/       # EP / GMM / comm method
vllm_ascend/device/device_op.py  # DeviceOperator A2/A3 vs A5 vs 310P
vllm_ascend/compilation/         # ACL graph capture
csrc/attention|moe|gmm|mc2/      # AscendC + ACLNN
csrc/torch_binding.cpp           # torch.ops._C_ascend
csrc/build_aclnn.sh              # SOC → CUSTOM_OPS_ARRAY
setup.py                         # COMPILE_CUSTOM_KERNELS (default 1)
tests/{ut,e2e}/
docs/source/
```

Evidence: op README = `design-doc`. `tests/ut` / `tests/e2e` = `test`. Do not upgrade to `benchmark`.

---

## Integration Surface

This source’s primary taxonomy. Kernel families below are **how** a surface is implemented, not a second inventory.

### Platform

`NPUPlatform` (`platform.py`). Device family via `AscendDeviceType` / `_build_info.py` from `SOC_VERSION`. Worker class, scheduler, `custom_ops=["all"]` except 310P. Force-disables vLLM breakable cudagraph.

### Model Runner

`worker/model_runner_v1.py` and `worker/v2/`. Builds attention metadata, chooses `MoECommType`, ACL-graph capture sizes (`MAX_CAPTURE_SIZES_FOR_950 = 4`). xlite (`vllm_ascend/xlite/`) is a **sibling serving runtime**, not P1.

### Attention Backend

| Backend | File | Role |
| --- | --- | --- |
| `ASCEND` | `attention_v1.py` | dense / paged FA |
| `AscendMLABackend` | `mla_v1.py` | MLA；FIA `npu_fused_infer_attention_score` |
| `AscendSFABackend` | `sfa_v1.py` | sparse FA + lightning indexer (DeepSeek-V3.2 / GLM-DSA) |
| `AscendDSABackend` (+ C4/C128/SWA) | `dsa_v1.py` | DeepSeek-V4 DSA |
| `AscendSFAIndexerBackend` | `indexer.py` | **cache-only** placeholder；不跑 indexer kernel |
| `AscendFAImpl` | `fa3_v1.py` | FA3 variant |
| 310P | `_310p/attention/` | **deferred** |

Context parallel twins: `attention/context_parallel/{mla,sfa,dsa,attention}_cp.py`.

### MoE / EP Backend

`ops/fused_moe/`: `MoECommType` ∈ {ALLGATHER, ALLTOALL, MC2, FUSED_MC2}.

- MC2 split: `torch_npu.npu_moe_distribute_dispatch(_v2)` / `combine(_v2)` — Case **B** job names.
- Expert compute: `torch_npu.npu_grouped_matmul` / `npu_grouped_matmul_swiglu_quant_v2` via `DeviceOperator` — Case **C**.
- FUSED_MC2 + `cann_ops_transformer` (`_MEGA_MOE_SUPPORTED`): Case **D** MegaMoe packaging.
- In-tree `csrc/mc2/dispatch_ffn_combine*`：`AddConfig` **A2/A3 only** — not 950 SoT (same skip as sgl `DISPATCH_FFN_COMBINE`).
- `eplb.py` / `worker/v2/eplb.py`：framework expert load collection — **not** a new EP window protocol by default.

### Device Communication

TP: `npu_mm_reduce_scatter_base` (`torch_npu`; A5 adaptor default `comm_mode="ai_cpu"`). Sequence-parallel FX passes. DP/EP groups. **Fill MC2 Knowledge 四问；不开第三张 MC2 Case.**

### Custom Ops

`COMPILE_CUSTOM_KERNELS` (default on) → `build_aclnn.sh` + `vllm_ascend_C`. `bootstrap_custom_op_env` prepends `ASCEND_CUSTOM_OPP_PATH`. Python gate: `enable_custom_op()`. A5 currently **off**.

### Graph / Capture

`compilation/acl_graph.py`, encoder ACL graph, spec-decode graph params. 950 capture-size cap. Not a kernel Case.

### KV Cache

`block_table.py`, `reshape_and_cache`, DSA/SFA compressor caches, `store_kv_block*`, `kv_compress_epilog` / `indexer_compress_epilog_v2` (A5 fused scatter+quant). Paged `block_table` is FA Knowledge **logical→physical**，不是 `select_idx`.

### Quantization

`quantization/methods/` (w8a8, MX, w4a*). Method objects call into fused_moe / attention. MX on A5 GMM is 53/C evidence, not a fourth GEMM Case.

---

## 950 ACLNN build list (`build_aclnn.sh` `^ascend950`)

```text
moe_gating_top_k_hash
inplace_partial_rotary_mul
kv_compress_epilog
compressor + compressor_metadata
vllm_quant_lightning_indexer + metadata
kv_quant_sparse_attn_sharedkv + metadata
hc_pre / hc_post
swiglu_group_quant
indexer_compress_epilog_v2
causal_conv1d
recurrent_gated_delta_rule
chunk_fwd_o / chunk_gated_delta_rule_fwd_h
store_kv_block + metadata
k2q_csr
```

**Not on this A5 list:** `sparse_attention_score` (it **is** on the A3 list). MiniMax-M3 still calls `torch.ops._C_ascend.npu_sparse_attention_score` via torch binding — different surface than the A5 ACLNN package.

---

## Kernel families (engineering clusters, not op counts)

### 1. Selection producer (lightning indexer)

`csrc/attention/vllm_quant_lightning_indexer/` (+ metadata AICPU). README: Top-k over quantized Q_index @ K_index → `sparse_indices`. Framework: DeepSeek-V4 `indexer.select_topk` → `_C_ascend.npu_vllm_quant_lightning_indexer`. SFA path more often `torch_npu.npu_lightning_indexer` / `npu_quant_lightning_indexer`.

### 2. Sparse attention consumer

A5 ACLNN: `kv_quant_sparse_attn_sharedkv` — **likely** Sparse-family consumer; contrast first. Binding also: `sparse_flash_attention`, `sparse_attention_score` (`selectIdx` in kernel). Name/use ≠ proven mechanism alternative. Do **not** open a second Sparse Case because the name appears; do **not** freeze `relation: alternative` before P1 close-read.

### 3. Indexer / KV compress epilog

A5: `indexer_compress_epilog_v2`, `kv_compress_epilog`. Fused quant+scatter into paged cache. **Adjacent utility** (selection/KV storage), not P1 “who produces the select map”. Close-read after indexer.

### 4. Dense / MLA FA (torch_npu FIA)

`npu_fused_infer_attention_score(_v2)` — FA Knowledge peer. No Case.

### 5. MoE routing + GMM (torch_npu + optional custom)

Dispatch/combine v2; grouped matmul + SwiGLU quant. Absorb A/B/C/D.

### 6. TP matmul-collective (torch_npu MC2)

`npu_mm_reduce_scatter_base`. M1 RS **job**, not a new Case.

### 7. Custom-op surfaces (P3) — two surfaces, not one chain

```text
Framework semantic op
        ↓
implementation selection
   ┌────┴───────────────┐
torch_npu / ACLNN    _C_ascend torch extension
   │                       │
vendor / CANN op       in-tree C++ binding
   │                       │
Host / kernel          Host / kernel
```

Surface A: `build_aclnn.sh` → vendor ACLNN package → `_cann_ops_custom`.  
Surface B: `setup.py` / CMake → `vllm_ascend_C` → `torch_binding.cpp` → `torch.ops._C_ascend.*`.  
**Do not collapse.** Which surface `enable_custom_op()` gates on A5, and whether `_C_ascend` direct calls bypass it, is P1/P3 close-read. Scout does not conclude.

P3 close-read must keep three layers distinct (do not auto-identify them):

```text
Build / delivery surface     ≠
Python call surface          ≠
runtime selected implementation
```

This diagram is the **candidate** runtime contract, not a Scout fact. Candidate for Knowledge / workflow **after** evidence; do not preset the file.

### Deferred (this source)

Model patches, LoRA glue, sampling, generic cache helpers, 310P, A2-only historical, GDN/Mamba/causal_conv, k2q_csr, xlite internals, `dispatch_ffn_combine` A2/A3.
