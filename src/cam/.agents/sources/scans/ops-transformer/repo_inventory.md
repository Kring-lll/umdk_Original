# ops-transformer Repository Inventory

Scout only. Does not modify `cases/` / `knowledge/` / `skills/`.

## Identity

| Field | Value |
| --- | --- |
| Official repo | `cann/ops-transformer` (`https://gitcode.com/cann/ops-transformer.git`) |
| Scan path | `/home/wangyibo/ops-transformer_bak` (local checkout; **not** identity) |
| Origin on this checkout | `git@gitcode.com:wang_yibo/ops-transformer.git` (personal fork) |
| HEAD | `64e78dead43e0025aa714abcf4543af1e7a19cad` |
| Branch | `Add-4-op` (master on this remote is `9311b63bc`) |
| Date | 2026-08-21 |
| License | CANN Open Software License Agreement Version 2.0 — **clear** |

This Scout describes **this checkout**. `Add-4-op` adds DeepEP A3 dirs (`dispatch_layout` / `notify_dispatch` / `dispatch_normal` / `combine_normal`) that may not be on a CANN-tag tarball. Do not treat fork HEAD as `cann/ops-transformer` master.

## What it solves

ops-transformer is the CANN **production operator library** for Transformer / MoE / 通算融合. Delivery unit is an **aclnn op** (`op_host` tiling + `op_kernel` AscendC + optional `op_api` / `op_graph` / `examples` / `tests`), not a CATLASS example.

It is not a Cube template zoo. Cube GEMM appears **inside** GMM / MC2 / FA, often via:

- `gmm/common/cgmct/` — production CATLASS-derived layer (DAV3510 / DAV2201)
- `gmm/grouped_matmul/op_kernel/gmm_infra/` — older Catlass-namespace 910B stack
- vendored Catlass under `mc2/mega_moe/op_kernel/arch22/`

## Contains real AscendC kernels?

Yes. Typical op: `op_kernel/${op}.cpp` with `__aicore__`, `KERNEL_TASK_TYPE_*` (AIC / AIV / MIX). Host: `${op}_tiling.cpp` + `*_def.cpp` + `*_infershape.cpp`.

## Target hardware

| Product | Support (from `docs/zh/ascend950_op_list.md` + per-op README) |
| --- | --- |
| Ascend 950PR / 950DT (`arch35` / `__NPU_ARCH__==3510`) | first-class for MoE routing, GMM, MC2 EP, FIA, MLA prolog, MHC |
| Atlas A3 (`ascend910_93`) | many ops; DeepEP stack on this branch is **A3-only** |
| Atlas A2 (`ascend910b` / arch22) | legacy FA/GMM/MC2; some 950-only ops absent |
| Kirin X90 / 9030 | listed on some READMEs; **out of AscendWeaver v0.1 HW priority** |

Compile: `build.sh --ops=<name> --soc=ascend950` (or `ascend910b`). Missing `--soc` skips that arch (`design-doc` skill notes + `docs/zh`).

## Kernel / test / doc locations (repo-wide)

```text
${class}/${op}/
  op_host/          # def, infershape, tiling, config/${soc}/
  op_kernel/        # entry + arch22/ arch35/ templates
  op_api/           # aclnn (optional)
  op_graph/         # GE fusion (optional)
  tests/{ut,st}/    # host/kernel UT; ST often CSV / ATK JSON
  docs/ examples/ README.md
common/             # shared host/kernel helpers
gmm/common/cgmct/   # shared Cube/MIX GMM infra
mc2/common/         # HCCL window, pipeline templates, “one calc two comm” tiling
tests/              # repo CI wiring (tests/test_config.yaml)
docs/zh/            # 950 list, dir_structure, develop guide
examples/           # add_example, fast_kernel_launch, mc2/all_gather_add
experimental/       # prototypes; not default Case sources
torch_extension/    # PyTorch API surface
```

Correctness: per-op `tests/ut` + often `tests/st`. Benchmark: ST `"perf":"not_key"` is a **gate flag**, not a measured table. README “产品支持 / 功能说明” is **design-doc**. Do not upgrade to `benchmark`.

---

## Operator families

Each family is an engineering cluster, not an op count.

### 1. `moe/` — on-device routing (no HCCL)

| | |
| --- | --- |
| Host / tiling / kernel | `moe/${op}/op_host/`, `op_kernel/` (`arch35/` on v2/v3) |
| Test / doc | `moe/${op}/tests/{ut,st}/`, `docs/`, README; repo CI in `tests/test_config.yaml` |
| HW | 950 listed for gating, init_routing{,_v2,_v3,_quant_v2}, finalize_v2, re_routing, routing-map permute/unpermute, compute_expert_tokens. `*_with_ep` permute/unpermute: **no 950** |
| Engine | Mostly **AIV** / `MIX_AIV_1_0`. Not Cube |
| dtype / quant | fp16/bf16/fp32; v3 / quant_v2 add INT8 and MX-class paths (`design-doc` + headers) |
| Comm | **None in-kernel.** EP AllToAll is `mc2/`. `moe_re_routing` is local shuffle **after** AllToAll |
| Variants | v1/v2/v3; quant; grad; `moe_token_permute*` **aliases** v2/v3 on RegBase/950 |

**Patterns (aggregated):** gating+topk → init-routing/permute (sort+expand) → (expert compute elsewhere) → finalize/unpermute. Routing-map is a parallel input encoding. EP range-slice is A2/A3 adjunct.

### 2. `gmm/` — grouped expert compute

| | |
| --- | --- |
| Host / tiling / kernel | `gmm/${op}/op_host/` (+ `op_tiling/arch35/`), `op_kernel/` (`gmm_infra/`, `arch35/`, `*_apt.cpp` for 310) |
| Shared infra | `gmm/common/cgmct/{block,kernel,epilogue,tile,policy,prologue}` |
| Test / doc | UT CSV + ST on major ops; README product/dtype matrices |
| HW | 950: `grouped_matmul`, `_add`, `_finalize_routing`, `_swiglu_quant_v2`, `quant_grouped_matmul_inplace_add`. v1 swiglu_quant: **not 950**. dequant: **310P stub** |
| Engine | Cube / MIX / AIC→AIV MX builders |
| dtype / quant | fp16/bf16; INT8 T-C/T-T; MX fp8/fp4 + e8m0; antiquant A8W4 |
| Comm | None. “Routing” in finalize_routing = **token scatter-add**, not NCCL |
| Variants | groupType M vs K; ND vs NZ weight; huge tiling-key matrix |

**Patterns:** plain GMM; fused SwiGLU+quant; fused Gelu+MX; fused finalize-routing; K-group accumulate (`grouped_matmul_add`); inplace-add MX.

Host tiling contains `TryFullLoadA` / kernel `FullLoadA` (A full-K vs L1 **sum** with B). cgmct headers name StreamK schedulers. **Live path not verified this Scout** — see scan_notes.

`examples/fast_kernel_launch_example/csrc/grouped_matmul`: CATLASS-style `<<<>>>` demo, **bf16 910b**, not cgmct.

### 3. `mc2/` — compute–communication fusion

| | |
| --- | --- |
| Host / tiling / kernel | `mc2/${op}/op_{host,kernel,api}`; shared `mc2/common/` (window, HCCL, pipeline templates, `one_calc_two_comm_tiling`) |
| Test / doc | UT on production EP/matmul-comm; ST on a subset; `tools/dump_analysis/`, `tools/bandwidth_test/` |
| HW | EP v2: A2+A3+950; v3 / DeepEP: **A3**; mega_moe: **950-only** |
| Engine | MIX for matmul+comm; AIV_ONLY setup/teardown; AICPU/CCU comm engines on some paths |
| Comm | AllToAllV EP; AllGather/AllReduce/ReduceScatter; MTE window / peermem; P2P ATT↔FFN |
| Variants | v1→v2→v3; setup/teardown split; combine+RMSNorm; quantMode 0–4 incl MX |

**Two MoE EP stacks (do not merge):**

1. Production `moe_distribute_dispatch/combine` (+ v2/v3, setup/teardown)
2. DeepEP on this branch: `dispatch_layout → notify_dispatch → dispatch_normal → combine_normal` (A3, MTE)

`mega_moe`: one op `Dispatch + GMM1 + SwiGLU + GMM2 + Combine` (950). Compute uses CATLASS/cgmct-class GEMM; comm is window/RDMA — **not** “CATLASS has comm”.

Matmul–comm (`all_gather_matmul_v2`, `matmul_all_reduce`, `matmul_reduce_scatter_v2`, `allto_all_matmul`, …): **new vs CATLASS**. 950 Scout: `sources/scans/ops-transformer/mc2/`. **不要**用 `examples/mc2/all_gather_add` 当 950 SoT。

### 4. `attention/` — FA / MLA / indexer / conv

| Pattern | 950 signal | Engine | vs CATLASS 43–73 |
| --- | --- | --- | --- |
| Train FA (`flash_attention_score` + grad) | yes | MIX | unrelated to the six Cases |
| Infer FA: **FIA** (`fused_infer_attention_score`) | **950 primary** Host `TilingFusedInferAttentionScoreV4`；kernel 走 PFA/IFA arch35 | MIX | FA-pipeline **peer** of CATLASS 49 / Sparse Case；contrast passed；**no Case** |
| MLA pipeline (prolog v1–v3, sparse FA, kv_quant_sparse) | split; mixed_quant SFMLA 950-only | MIX cube+vector | unrelated to the six; alternative to CATLASS **19** |
| Lightning indexer (+quant/grad) | yes | MIX | unrelated — sparse **index** before SFA |
| Worker scheduler/combine | yes (AICPU) | AICPU | orchestration, not Cube GEMM |

Deferred leftovers: NSA, Floyd, delta-rule, BSA train, PA cache plumbing, stub `flash_attn`.

### 5. `ffn/` — FFN / Swin

`ffn/ffn` README **×950**. Swin fused ops A2-centric. **Not** the MoE expert path (that is `gmm/`). Deferred for 950-first.

### 6. `posembedding/` — RoPE / fused RMSNorm+RoPE+KV cache

950: `kv_rms_norm_rope_cache` (V1), several RoPE/quant-cache ops. Vector fusion. Complements MLA prolog; not Cube Case material. Knowledge-first / deferred.

### 7. `mamba/` and fused causal conv

`mamba/causal_conv1d` 950-only MIX_AIV; `attention/fused_causal_conv1d*` also on 950 list. Unrelated to GEMM Cases.

### 8. `mhc/` — Manifold Hyper-Connections

950-first tensor contraction + Sinkhorn. Unrelated to the six. Experimental `experimental/mhc` is older 910B reference with README speedup claims (`design-doc` / experimental bench — not production `benchmark`).

### 9. `experimental/` and `examples/`

Prototypes and teaching ops. `experimental/attention/typhoon_mla` wraps CATLASS (`inference` from scout). Not default close-read until promoted.

---

## Compile-time variants (cross-cutting)

- **SoC:** `*_apt.cpp` / `arch35/` vs arch22 `*.cpp`; CMake `CONDITION_UNIT=ascend950`
- **KERNEL_TASK_TYPE:** AIC / AIV / MIX ratios
- **Tiling keys:** `ASCENDC_TPL_*` dtype × trans × groupList × quant template
- **MoE API aliases:** permute/unpermute → init/finalize on 950
- **MC2 API gens:** multiple `aclnn_*_vN.cpp` inside v2 directories
- **Quant mode:** none / static / pertoken / pertile / MX

## What this repo is *not*

- Not CATLASS examples (except mega_moe vendored compute + fast_kernel_launch demo)
- Not a communication-free Cube tutorial
- Not ops-nn / elementwise zoo
