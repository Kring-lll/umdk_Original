# CATLASS Repository Summary

Scout output. Does not modify `cases/`.

## Identity

| Field | Value |
| --- | --- |
| Path | `/home/wangyibo/catlass` |
| Remote intent | official CANN template library |
| HEAD | `a8b4fffbd9cabc021dfd47253e907a1cde59de25` |
| Branch | `master` |
| Date | 2026-08-20 |
| License | CANN Open Software License Agreement Version 2.0 |
| License status | **clear** — can extract Case Cards; copying source into AscendWeaver still needs license notice |

## What it solves

CATLASS (CANN Templates for Linear Algebra Subroutines) is a **layered Cube/GEMM template library**. It decomposes Matmul-class operators into:

- Arch / Resource
- Layout / TLA
- Tile copy / Tile Mmad
- BlockMmad / BlockEpilogue / BlockScheduler
- Kernel
- Host example + optest

It is not a general Vector-op zoo. Almost every high-value kernel is **Cube GEMM or GEMM-derived** (Grouped, StreamK, Quant/MX, Conv-as-GEMM, Flash Attention, MLA, MoE finalize routing).

## Contains real AscendC kernels?

Yes. Examples launch `__aicore__` kernels assembled from `include/catlass/**`. Typical APIs: `GlobalTensor`, `LocalTensor`, `DataCopy`/`Fixpipe`, `SetFlag`/`WaitFlag`, `Mmad`, TLA tensors.

## Target hardware

| Product | Support |
| --- | --- |
| Atlas A2 | yes (examples `00`–`42`, `75`–`77`, GEMV, MLA, sparse) |
| Atlas A3 | yes |
| Ascend 950PR / 950DT (`CATLASS_ARCH=3510`) | yes, first-class from v1.5 onward |

AscendWeaver hardware priority: **950 first**. A2 examples are used only when they fill a pattern 950 does not cover (Vector GEMV, MLA).

## Kernel locations

```text
examples/            # end-to-end operator samples (preferred Case source)
include/catlass/     # reusable templates (the real engineering core)
  gemm/{kernel,block,tile}/
  epilogue/{block,tile,fusion}/
  gemv/
  conv/
  layout/ arch/
experimental/        # not default Case sources
python/tla_dsl/      # CATLASS DSL, out of v0.1 Case scope
tests/optest/        # operator tests wrapping many examples
tests/unittest/      # tile-level C++ tests (A2 + 950)
tools/tuner/         # autotune, not a Case
```

## Build / test / benchmark

- Build: `scripts/build.sh <example> [-DCATLASS_ARCH=3510]`
- Correctness: each example does golden compare (`Compare success.`)
- Operator tests: `tests/optest/`
- Tile tests: `tests/unittest/catlass/gemm/tile/`
- Perf: homepage plots + some example docs (e.g. Fixpipe has measured us tables). Most READMEs say to comment out golden compare for perf.

## Deprecated / multi-version notes

- A2 `00_basic_matmul` and 950 `43_ascend950_basic_matmul` are the **same Common/Pingpong pattern** on different arch. Do not ingest both.
- Many quant/GMM/FA examples are incremental epilogue variants of one pattern.
- `experimental/` (MX FA infer) is not a default Case until promoted to `examples/`.
- CATLASS DSL (`python/tla_dsl`) is a different programming surface; Scout does not treat it as AscendC Case source in this batch.

Covered well: GEMM, GEMV(vector), Batched, Grouped, SplitK, StreamK, FullLoad/L1-resident, Fixpipe MIX, MX quant, Matmul+Epilogue (EVG / SwiGLU / Gelu), Conv2d, Flash Attention infer/prefill, MLA, MoE finalize-routing (scatter-add).

Not in CATLASS: elementwise/reduce/norm as standalone Vector ops, communication, AllGather+Matmul overlap, Host-free SIMT kernels.
