# sgl-kernel-npu Repository Inventory

Scout only. Does not modify `cases/` / `knowledge/` / `skills/`.

## Identity

| Field | Value |
| --- | --- |
| Official repo | `sgl-project/sgl-kernel-npu` (`https://github.com/sgl-project/sgl-kernel-npu.git`) |
| Scan path | `/home/wangyibo/sgl-kernel-npu` (**not** identity) |
| Origin on this checkout | `https://github.com/wangyibo1005/sgl-kernel-npu.git` (personal fork) |
| HEAD | `8d76a8da75315ae9add5cfd2458b8028289f1ada` |
| Branch | `main` |
| Date | 2026-08-21 |
| License | MIT — **clear** (copyright notice required if source is copied into AscendWeaver) |
| Package version (`config.ini`) | `2026.6.1` |

This Scout describes **this checkout**. Official tree was not cloned as the scan root. Sparse Attention’s three Case evidence paths were later verified on `sgl-project/sgl-kernel-npu` `main` @ `ee334193e5278d7b3a64afcc2e3ee5243133c4a0` (byte-identical to this HEAD for those files).

Submodules (`third_party/catlass`, `third_party/pto-isa`) are **not initialized** on this tree (`git submodule status` shows `-`). DeepEP A5 CMake **requires** `third_party/catlass/include` at build time; Scout did not clone them.

## What it solves

sgl-kernel-npu is SGLang’s **production inference kernel library** for Ascend. Delivery unit is a **PyTorch custom op** (`torch.ops.npu.*` / `deep_ep` Python API), not a CATLASS example and not an aclnn-first CANN op catalog.

Two packages:

| Package | Role |
| --- | --- |
| `python/deep_ep/` | DeepEP-Ascend: EP dispatch/combine, fused MoE, normal vs low-latency |
| `python/sgl_kernel_npu/` | Attention, LoRA, MLA preprocess, FLA/Mamba, KV cache, speculative tree, CATLASS wrappers |

It is **not** a Cube template zoo. Cube appears inside fused MoE, sparse attention, CATLASS wrappers, and some attention ops.

## Contains real AscendC kernels?

Yes. Typical layout: `csrc/${op}/op_host/` + `op_kernel/` with `__aicore__`. DeepEP uses CANN custom-op packaging under `csrc/deepep/ops/` (and A2 twin `ops2/`). SGLang kernels compile via `csrc/CMakeLists.txt` `ascendc_library`.

## Target hardware

README and CMake **do not treat 950 as a third “A3 kernels” product**. Mapping used in this Scout:

| Product | Support |
| --- | --- |
| Atlas A5 / Ascend 950 (`ascend950*`, `__NPU_ARCH__==3510`, `__DAV_C310__`) | DeepEP fused MoE A5 + dispatch/combine `*_a5.h`; `sparse_attention_score` **arch35**; 950 CMake **turns off** `SGL_KERNEL_ENABLE_A3_ONLY_OPS` |
| Atlas A3 (`ascend910_93`) | First-class for DeepEP (normal + fused) and the bulk of `sgl_kernel_npu` (MLA, lightning indexer, FLA, laser/ada attn) |
| Atlas A2 (`ascend910b` / arch22) | DeepEP `ops2` + A2 guides; sparse attention **arch22** |

**950 CMake fact:** `csrc/CMakeLists.txt` sets `SGL_KERNEL_ENABLE_A3_ONLY_OPS=OFF` when `SOC_VERSION` matches `^ascend950`. That **drops** host/kernel for MLA preprocess, lightning indexer, sparse_attn_sharedkv, tri_inv, FLA/GDN, batch_matmul_transpose, CATLASS fp8 GMM/matmul. Still built on 950: cache/speculative helpers, LoRA, compressor, causal_conv1d(_update), sparse_attention_score, CATLASS **basic** matmul (if `BUILD_CATLASS_MODULE`).

DeepEP is a **separate** `-a deepep Ascend950` target. On 950 it sets `DEEPEP_SKIP_DISPATCH_FFN_COMBINE=1` and includes **system** `ops-transformer` `moe_distribute_base.h`.

AscendWeaver hardware priority remains **950 first**. A3-only SGLang kernels are inventory + deferred, not the first close-read set.

## Kernel / test / doc locations

```text
csrc/deepep/ops/          # A3/A5 DeepEP custom ops (fused, dispatch/combine, layout)
csrc/deepep/ops2/         # A2 DeepEP twin
csrc/${op}/op_host|kernel # SGLang kernels → libsgl_kernel_npu.so
csrc/attentions/          # laser / ada sparse / sparse_block_estimate (separate tree)
csrc/catlass/             # torch.ops wrappers over third_party/catlass
python/deep_ep/doc/       # NORMAL / LOW_LATENCY / FUSED_DEEP_MOE APIs
tests/python/deepep/      # EP correctness + some perf compare
tests/python/sgl_kernel_npu/
benchmark/                # present; not ingested as us tables this Scout
docs/developer_guide/
```

Evidence: README + `python/deep_ep/doc/` = `design-doc`. `tests/python/**` = `test`. Homepage DeepEP GB/s / us tables = `benchmark` (repo-level, not per-kernel msprof). Do not upgrade ST-style scripts to `benchmark` without numbers in-tree.

---

## Operator families

Each family is an engineering cluster, not an op count.

### 1. DeepEP fused MoE (`fused_deep_moe` / A5)

| | |
| --- | --- |
| Host / kernel | `csrc/deepep/ops/op_host/fused_deep_moe_a5/`, `op_kernel/fused_deep_moe.cpp` (`__DAV_C310__` → `fused_deep_moe_a5/entry.h`; else MIX 1C2V A3 path) |
| Test / doc | `tests/python/deepep/test_fused_deep_moe_a5.py`, `python/deep_ep/doc/FUSED_DEEP_MOE.md` |
| HW | A3 + A5; A5 path **not** identical (weights, capacity, second return) |
| Engine | MIX Cube+Vector+comm (A3 `KERNEL_TYPE_MIX_AIC_1_2`) |
| Relation | **alternative** to ops-transformer MegaMoe (Case D). A5 fused **includes CANN ops-transformer** `moe_distribute_base.h` |

`FuseMode.DISPATCH_FFN_COMBINE` is a second fused op (`aclnnDispatchFFNCombine`) that **embeds InitRoutingQuantV2**. **Skipped on 950** (`DEEPEP_SKIP_DISPATCH_FFN_COMBINE=1`). Do not put it in the 950 close-read set.

### 2. DeepEP split EP (dispatch / combine)

Normal + low-latency Python APIs over `moe_distribute_*_v2` / `cam_moe_*_normal` / `notify_dispatch` / `dispatch_layout`. A5 headers: `moe_distribute_{dispatch,combine}_v2_a5.h`, `cam_moe_*_normal_a5.h`.

| | |
| --- | --- |
| Pair contract | token-major `x` + `topk_idx` → dispatch → local expert compute → combine (Python `Buffer`) |
| vs Case B | same **routing loop** as ops-transformer `moe_distribute_v2`; **not** InitRouting’s predecessor |
| vs opsxf-deepep-a3 | this repo is the **live** DeepEP; ops-transformer DeepEP dirs were fork/A3-only |

### 3. Sparse attention (`sparse_attention_score`)

Paged block-sparse main attention (post-indexer) for MiniMax-M3. arch35 + arch22. Built on 950. `torch.ops.npu.npu_sparse_attention_score`.

### 4. CATLASS wrappers (`csrc/catlass/`)

`catlass_matmul_basic` (950+A3); `catlass_gmm_fp8` / `catlass_matmul_fp8` **A3-only** in this CMake. Teaching value is **how 43/60/53 names appear in SGLang**, not a new GEMM Case.

### 5. A3-first inference kernels (950 CMake off)

MLA preprocess, lightning indexer, sparse_attn_sharedkv, laser/ada attention under `csrc/attentions/`, FLA/GDN, tri_inv, batch_matmul_transpose.

### 6. Always-on helpers (950)

LoRA BGMV/SGMV, compressor, causal conv1d update, KV cache assign/extend, speculative `build_tree`.

### 7. Not in this Scout as Case sources

`csrc/helloworld`, `contrib/torch_memory_saver`, uninitialized `pto-isa` (chunk GDN tests exist).

---

## Deprecated / multi-version notes

- `ops/` vs `ops2/`: A3/A5 vs A2. Do not Case both as two EP mechanisms until close-read proves a 950-specific protocol.
- A3 `fused_deep_moe.h` vs A5 `fused_deep_moe_a5/`: **two live paths**. 950 Scout must pin `__DAV_C310__` / `ascend950`, not the A3 MIX 1C2V file.
- Do not treat `torch.ops.npu.*` names as new kernel families when they wrap CATLASS examples.
