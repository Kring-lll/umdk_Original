# Close-read: 46 Ascend950 Matmul Fixpipe Opti

Status: **understanding report for human review**. Not a Case Card. `cases/` not modified.

- Candidate: `catlass-46-ascend950-matmul-fixpipe-opti`
- Commit: `a8b4fffbd9cabc021dfd47253e907a1cde59de25`
- Golden sample: `cases/compute/gemm/ascend950-basic-matmul/`
- Scope: live MIX path with `MmadPingpong` + `PackedTileCopyTlaToUB` + `EpilogueAscend950Fixpipe<SPLIT_M>` + `BlockSchedulerAswt`. Quant/dequant dualDst and `NO_SPLIT` are non-active.

Evidence kinds: `source-code` | `design-doc` | `benchmark` | `test` | `inference`.

---

## What this example actually instantiates

Shipped `examples/46_ascend950_matmul_fixpipe_opti/matmul_fixpipe_opti.cpp`.

| Item | Live value | Evidence |
| --- | --- | --- |
| Arch | `Arch::Ascend950` | source-code example |
| Executable | **mix** | `CMakeLists.txt`: `... mix matmul_fixpipe_opti.cpp` |
| A/B | `half` | example |
| C | `float` | example |
| Accumulator | `float` | `ElementAccumulatorSelector<half,half>` |
| Bias | `void` | example |
| Layout A | `RowMajor` | example |
| Layout B | `ColumnMajor` | example (`LayoutTagB = layout::ColumnMajor`) |
| Layout C | `RowMajor` | example |
| DispatchPolicy | `MmadPingpong<Ascend950, UnitFlag=true, HF32=false>` | L1 2/2, L0 2/2, L0C 1 |
| L1 tile | `256 × 256 × 128` | design-doc + example |
| L0 tile | `256 × 256 × 64` | design-doc + example |
| TileCopy | `PackedTileCopyTlaToUB` | overrides `CopyL0CToDst` → `CopyL0CToUBTla` |
| `copyMode` | `CopyL0CToUBMode::SPLIT_M` | `splitM = is_same<ElementC, ElementAccumulator>` → true |
| Epilogue | `BlockEpilogue<EpilogueAscend950Fixpipe<true>, ...>` | **not** `void` |
| Scheduler | `BlockSchedulerAswt<L1, L0>` | `isGmm_=false` |
| Workspace | 0 | kernel `GetWorkspaceSize` |
| UB stages | 2 | `UB_STAGES = 2`, `UB_TILE_SIZE = UB_SIZE / 2` |
| Host cores | all `GetCoreNumAic()` | example |

Design-doc prototype table: A half `[m,k]` ND, B half `[n,k]` ND, C float `[m,n]` ND.

`source-code` uses logical B as ColumnMajor `(k, n)`. That is consistent with “B stored as N×K ND” **if** ColumnMajor means that; do not flatten the two wordings into one fact beyond “example instantiates `layout::ColumnMajor` for B.”

**Divergence — do not resolve:** optest default `transB=False` JIT-generates `CATLASS_JIT_LAYOUT_B=RowMajor` (`LayoutToStr(false, false)`). The Python test uses `torch.randn(k, n)` and `transB=False`. Shipped C++ example is ColumnMajor B. Case metadata, if written, should follow the **shipped example**, and record that optest can compile a RowMajor-B variant.

---

## Why the writeback path changed

Design-doc (`46_ascend950_matmul_fixpipe_opti.md`):

> Fixpipe N-unaligned GM write is slow; small-K / large-MN tends to be Fixpipe-bound. 950 adds L0C→UB. Enable dualDstCtrl, split one Cube L0C to two Vector UBs, then DataCopyPad UB→GM. Each Vector UB can double-buffer.

This is **design-doc**, not independently re-measured here. The same file **does** contain a benchmark table (`benchmark`).

Live `source-code` matches that story at the call level:

1. AIC: wait AIV, `blockMmad(..., cLocalTensor on UB, ...)`, set AIV flag.  
   Inside BlockMmad, `CopyL0CToDst` is actually `CopyL0CToUBTla<..., SPLIT_M>` with `intriParams.dualDstCtl = 1`.
2. AIV: wait AIC, `BlockEpilogue` → `CopyUb2GmTla` → `AscendC::DataCopyPad` UB→GM, set AIC flag.
3. UB list pingpongs with `ubListId = (loopIdx / blockNum) & 1` when `UB_STAGES > 1`.

Comment in example and kernel: “dualDstCtrl is not supported in quant/dequant scenarios.” This example is not quant. Do not generalize that comment into a hardware manual.

When `ElementC != ElementAccumulator`, code selects `NO_SPLIT` and the second AIV subblock returns early. **Not this example** (`C` is `float`).

---

## Layering vs 43

```text
Host example (MIX)
  DeviceGemm<KernelMatmulMixFixpipeOpti<BlockMmad, BlockEpilogue, Aswt>>
    Kernel (AIC + AIV, CrossCore flags)
      BlockMmadTla<MmadPingpong, PackedTileCopyTlaToUB>   — Cube, dst is UB
      BlockEpilogue Fixpipe                              — Vector, UB → GM
        CopyUb2GmTla / DataCopyPad
```

43: Kernel AIC, `BlockEpilogue = void`, L0C→GM inside BlockMmad.  
46: Kernel MIX, independent epilogue stage, L0C→UB→GM.

Effective compute is Cube + Vector. `compute_engine: vector+cube` is correct. Wording: AIC still does Mmad; AIV does writeback, not a fused activation.

---

## Data flow

```text
GM A  fp16 RowMajor
GM B  fp16 ColumnMajor   (shipped example)
        │  MTE2  GM → L1   (same pingpong BlockMmad)
        ▼
L1
        │  MTE1  L1 → L0A / L0B
        ▼
L0A, L0B
        │  Mmad → L0C (fp32, UnitFlag)
        ▼
L0C
        │  Fixpipe L0C → UB   dualDstCtl=1, SPLIT_M
        ▼
UB  (2 stages on Vector; AIV subblocks split M)
        │  DataCopyPad  UB → GM
        ▼
GM C  fp32 RowMajor
```

Pingpong of L1/L0 is still “prefetch next L1-K onto the other buffer,” **not** `MmadPreloadAsyncWithCallback`.

AIC↔AIV handshake is extra: `CrossCoreWait/SetFlag` mode 4, flag ids 6/8, plus `FLAG_ID_MAX` when dual dst.

---

## Parallel strategy

`BlockSchedulerAswt` (`source-code`):

- MN L1 tiles: `CeilDiv(M,256) * CeilDiv(N,256)`.
- `round_ = CeilDiv(tileNum, blockNum)` then some cores drop a round so the last partial wave is not over-assigned.
- Window length `WINDOW_LEN = 4`; odd window rows reverse N (`nTileIdx_ = nTileNum_ - 1 - nTileIdx_`).
- If `endBlockIdx_ + 1 <= blockNum / 2`, `UpdateTailTile()` further splits the leftover tiles so more cores get work.

Design-doc `docs/zh/2_Design/01_kernel_design/05_aswt.md` describes the same idea (S-window + split leftover tiles). That doc’s **speedup table is ASWT vs Swizzle on basic matmul**, not this Fixpipe opti vs Fixpipe.

46’s own benchmark table says it compared **the same tileShape and the same scheduler**, so those speedups isolate the writeback path, not ASWT.

Do not merge the two tables.

Host launches all AIC. Kernel uses `GetBlockIdx()`; on AIV, `curBlockIdx /= GetTaskRation()` so Vector pairs map to the Cube block.

---

## Buffer math for the selected tiles

fp16 A/B, fp32 C/acc, UnitFlag ⇒ L0C stages 1:

| Buffer | Occupancy |
| --- | --- |
| L1A | `256×128×2×2 = 128 KiB` |
| L1B | `256×128×2×2 = 128 KiB` (L1 total 256 KiB, **not** 512) |
| L0A | `256×64×2×2 = 64 KiB` |
| L0B | `64×256×2×2 = 64 KiB` |
| L0C | `256×256×4×1 = 256 KiB` |
| UB window | `UB_SIZE / 2 = 248 KiB / 2 = 124 KiB` per stage (`source-code`) |

L0A/L0B/L0C match listed capacity. L1 does **not** fill 512 KiB. Do not say these tiles were chosen to pack L1.

**Open (do not claim overflow or fit):** a full 256×256 fp32 C tile is 256 KiB; one UB stage is 124 KiB. SPLIT_M / dualDst is supposed to split M across two Vector UBs (`source-code` epilogue uses `GetTaskRation()` and `GetSubBlockIdx()`). This review did **not** independently prove the per-AIV footprint equals `halfM × N × 4` or that it fits 124 KiB. Flag for Case Card: state the allocation formula, not “UB is packed.”

---

## Pipeline

`pipeline_pattern`: **vector-cube-overlap** is the distinctive tag (AIC Mmad / Fixpipe vs AIV DataCopyPad, double-buffered UB). L1/L0 pingpong is still present; if the schema allows only one distinctive tag, prefer `vector-cube-overlap` over repeating 43’s `double-buffer`. If multiple are allowed: `vector-cube-overlap` + `double-buffer`.

---

## Benchmark (do not over-read)

From the example markdown, Ascend 950, CANN 9.0.0, same L1/L0 tiles and ASWT:

| [M,N,K] | baseline Fixpipe→GM | this opti | ratio | note in table |
| --- | --- | --- | --- | --- |
| 567×488×399 | 6.53us | 6.34us | 1.03 | MN unaligned |
| 1226×1557×399 | 15.06us | 12.05us | 1.25 | MN unaligned |
| 2058×2038×256 | 20.69us | 12.64us | 1.64 | MN unaligned |
| 2048×2048×256 | 11.97us | 12.09us | **0.99** | MN aligned |
| 2058×2048×256 | 13.81us | 12.38us | 1.12 | M unaligned |
| 2048×2038×256 | 19.09us | 12.35us | 1.55 | N unaligned |

`benchmark` in-tree. This review did not rerun it.

Do **not** write: “always use L0C→UB.” The same table shows ~1.00× on aligned MN. Design-doc itself points at N-unaligned / Fixpipe-bound.

---

## CanImplement / leftover names

Kernel `CanImplement` returns `true`. Same non-guard class as 43, **this kernel only**.

Kernel also declares `BlockMmad::ML1_` … `KL0_`. Pingpong `BlockMmadTla` names those `L1_TILE_M` / `L0_TILE_M`. The aliases are unused in `operator()`. This close-read did not compile the example; **do not** assert a build break. Record as a naming leftover to re-check if a Case is written.

---

## What is *not* running

| Item | Status |
| --- | --- |
| 43-style L0C→GM Fixpipe | the baseline in the benchmark table, not this binary |
| `CopyL0CToUBMode::NO_SPLIT` / `SPLIT_N` | not selected (`C==acc` ⇒ SPLIT_M) |
| Quant/dequant dualDst | commented unsupported; not this example |
| Identity swizzle | ASWT is used |
| Bias / HF32 / L1 resident | off |
| fp16 output | example C is float |

---

## Tests

`tests/optest/tests/test_46_ascend950_matmul_fixpipe_opti.py`: 3510, `128×128×128`, fp16 inputs, fp32 output, `transB=False` → JIT **RowMajor B**, vs `torch.matmul`. rtol/atol 1e-2. `JitKernelType::MIX`.

This is a smoke shape, not the Fixpipe-bound shapes in the benchmark table.

---

## What NOT To Copy

1. Do not copy L0C→UB as the default 950 Matmul writeback. Benchmark shows no gain on aligned MN.
2. Do not copy 256/128/64 as a universal tile (L1 is half-empty at fp16; 43 used different K/dtype).
3. Do not copy ASWT `WINDOW_LEN=4` as “950 best scheduler.” Separate doc table; not this Case’s Fixpipe comparison.
4. Do not treat this `CanImplement` as a resource check.
5. Do not fold EVG / visitor fusion into this Case. Epilogue here is UB→GM DataCopyPad, not a visitor graph.
6. Do not claim dualDst hardware semantics beyond `dualDstCtl = 1` and the two-subblock M split in epilogue.

---

## Vs 43 Basic Matmul

| | 43 | 46 |
| --- | --- | --- |
| Change layer | golden pingpong + void epilogue | **Kernel MIX + Tile L0C→UB + Epilogue UB→GM + ASWT** |
| Dtype | fp32 all | fp16 A/B, fp32 C |
| B layout | RowMajor | ColumnMajor (example) |
| Writeback | L0C → GM on AIC | L0C → UB → GM with AIV |
| Engine | cube, AIV empty | vector+cube |
| Scheduler | Identity swizzle `<3,dir>` | ASWT |
| Why it exists | teach CATLASS layering / pingpong | teach a **measured Fixpipe bottleneck** workaround |

---

## Review questions

1. **Independent Case?** **Yes.** Proposed: `cases/compute/gemm/ascend950-fixpipe-optimization/`. It is a performance-problem-driven Case, not another GEMM math Case.
2. **Engineering Knowledge?** **Yes, after the Case:** `knowledge/engineering-experience/l0c-writeback-optimization.md`. Content should stay at: when Fixpipe→GM is bound (especially N-unaligned, small K), 950 can Fixpipe to UB and let Vector pad-copy to GM; cite **this** benchmark; do **not** upgrade to “all unaligned stores should bypass L2 / use UB.”
3. **ASWT knowledge?** Not in the first knowledge file. ASWT has its own design-doc table vs Swizzle. Keep it inside the Case’s scheduler section until seen elsewhere.
4. **Core difference vs 43:** 43 stops at L0C→GM on AIC. 46 adds a Vector writeback stage because the authors measured Fixpipe-bound cases.

Open items:

- Example ColumnMajor B vs optest RowMajor B.
- UB stage size vs 256×256 fp32 tile after dual-dst split → **see** `sources/scans/catlass/close_reads/46_ub_footprint.md` (not resolved).
- `BlockMmad::ML1_` name vs `L1_TILE_M`.
- Design-doc B `[n,k]` ND vs code `ColumnMajor(k,n)` wording.
