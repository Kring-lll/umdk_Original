# Close-read: 53 Ascend950 MXFP8 Matmul

Status: **passed**. Case Card: `cases/compute/gemm/ascend950-fp8-mx-matmul/`. This file stays the evidence dump.

- Candidate: `catlass-53-ascend950-fp8-mx-matmul`
- Commit: `a8b4fffbd9cabc021dfd47253e907a1cde59de25`
- Golden sample: `cases/compute/gemm/ascend950-basic-matmul/`
- Scope: live `fp8_mx_matmul.cpp` → `MxMatmulTla` + `MmadMx` + UnitFlag + e4m3 + Identity swizzle. ASWT binary, example 54/58/59, Bias, `MmadMxPreload`, `enableL1Resident` are non-active on this executable.

Evidence kinds: `source-code` | `design-doc` | `test` | `benchmark` | `inference`.

No `benchmark` evidence in this example tree. README has no us / TFLOPS table.

---

## What this example actually instantiates

Shipped `examples/53_ascend950_fp8_mx_matmul/fp8_mx_matmul.cpp`. CMake: **cube**.

| Item | Live value | Evidence |
| --- | --- | --- |
| Arch | `Arch::Ascend950` | source-code example |
| Executable | **cube** | `CMakeLists.txt`: `catlass_example_add_executable(... cube ...)` |
| A/B | `float8_e4m3_t` | `using ElementA/B` |
| Scale | `float8_e8m0_t` | `ElementMxScale` |
| C | `float` (fp32) | `ElementC` |
| Bias | `void` | example |
| Layout | A RowMajor, **B ColumnMajor**, C RowMajor | `trans_a=0, trans_b=1` |
| DispatchPolicy | `MmadMx<Ascend950, UnitFlag=true, L1_SCALE_FACTOR_K=16>` | 3-arg form; L0C stages default 1; L1 resident default false; L1A/B 2/2, L0A/B 2/2 |
| L1 tile | `256 × 256 × 256` | M, N, K |
| L0 tile | `256 × 256 × 128` | |
| Block | `BlockMmadTla<MmadMx, ...>` | **not** 43 `MmadPingpong` |
| Tile copy | `PackedMxTileCopyTla` | GM/L1/L0 + MX scale copies |
| Scheduler | `GemmIdentityBlockSwizzle<3, 0>` | dir **fixed 0** |
| Epilogue | `void` | writeback inside BlockMmad |
| Workspace | 0 | `GetWorkspaceSize` |
| AIV | empty | `operator()<AIV>` |
| Host cores | `min(aicCoreNum, taskNum)` | like 43 |
| `CanImplement` | `true` | this kernel |

Same directory also builds `53_ascend950_fp8_mx_matmul_aswt` (`BlockSchedulerAswt`). That is a **second executable**, not the default `fp8_mx_matmul.cpp` path.

CLI (`design-doc` README): `./53_... 256 512 1024 [device_id]` after `gen_data.py 256 512 1024 0 1`.

`test` `test_53_ascend950_fp8_mx_matmul_aswt.py`: 3510-only, **ASWT** JIT, all four transpose pairs, `256×512×1024`, rtol/atol 1e-2. There is **no** optest for the Identity-swizzle binary.

---

## Vs 43: what actually changed

43 is fp32 pingpong with empty AIV and no scales. 53 keeps Kernel/Block/Tile layering but **replaces the Block policy** and adds scale tensors.

```text
Host DeviceGemm  (cube, min(nAic, mnTiles), workspace 0)
  Kernel MxMatmulTla + GemmIdentityBlockSwizzle<3,0>
    AIC: BlockMmadTla<MmadMx>
         GM A,B + MxScaleA,B → L1 (A/B pingpong; scale fetched every L1_SCALE_FACTOR_K L1-K strips)
         L1 → L0 (L0 element is mx_fp8_e4m3_t, not the GM fp8_e4m3_t)
         Mmad → L0C (fp32) → Fixpipe → GM C
    AIV: empty
```

Do not describe this as “43 plus a scale epilogue.” Scale is in the Cube Block, not `BlockEpilogue`.

---

## MX contract (live)

`source-code` `catlass.hpp` / example / `gen_data.py`:

```text
MX_SCALE_GROUP_NUM      = 32   // one e8m0 per 32 K elements
MX_SCALE_COPY_GROUP_NUM = 2    // 2-byte align / MakeMxScaleLayout C0
MX_BASEK_FACTOR         = 64   // A/B K alignment; L1_K and L0_K must be multiples of 64
```

Host:

```text
mxScaleK = CeilDiv<32>(K)
layoutMxScaleA = MakeMxScaleLayout<e8m0, RowMajor, false>(M, mxScaleK)
layoutMxScaleB = MakeMxScaleLayout<e8m0, ColumnMajor, true>(mxScaleK, N)
lenMxScaleA    = M * RoundUp<2>(mxScaleK)
lenMxScaleB    = RoundUp<2>(mxScaleK) * N
```

`gen_data.py` block size is **32** (`source-code`). README (`design-doc`) writes scale shape `(m, ceil(k/64), 2)` for RowMajor A. `MakeMxScaleLayout` packs the scale-K axis with `ELE_NUM_PER_C0=2`, so `ceil(mxScaleK/2) = ceil(K/64)` when K is a multiple of 64. Do not treat README’s 64 as a second MX group size.

Math (`design-doc` README): `C = (MxScaleA ⊙ A) @ (MxScaleB ⊙ B)` (+ Bias if not void). Live Bias is void.

Template also names `float8_e5m2`. This executable instantiates **e4m3 only**.

---

## Parallel / memory / pipeline

MN tiling is 43-style Identity swizzle, offset 3 dir 0, `min(nAic, mnTiles)` cores.

`L1_SCALE_FACTOR_K=16`: GM→L1 scale copy covers up to 16 L1-K strips per fetch (`source-code` Block). A/B still pingpong per L1-K (`L1A/B_STAGES=2`).

L1 occupancy (fp8 e4m3, 2/2 stages, factor 16), `source-code` formulas:

```text
L1A/B each: 256×256×1×2 = 128 KiB
L1 scale A/B each: 256×256/32 × 1 × 16 × 2 stages = 64 KiB
sum 384 KiB  (static_assert vs Arch L1)
```

L0 uses `GetL0Element<e4m3, true> = mx_fp8_e4m3_t`. Do not copy 43’s fp32 L0 byte math onto this path.

`pipeline_pattern`: Cube **double-buffer** (A/B). Not mix. ASWT’s `UpdateTailTile` is only on the **aswt** specialization (`source-code`); default Identity path does not call it.

README `enableL1Resident` windows (`design-doc`) are **not** this executable (`ENABLE_L1_RESIDENT` default false).

---

## What is *not* running

| Item | Status |
| --- | --- |
| `fp8_mx_matmul_aswt.cpp` / `BlockSchedulerAswt` | second binary; optest only covers this |
| 54 FP4 / 59 A8W4 / 58 batched MX | different examples |
| Bias / e5m2 / L1 resident / L0C stages 2 | not instantiated |
| 43 `MmadPingpong` | different policy |

存在 ≠ 运行 ≠ 本候选教学内容。

---

## Source conflicts

| 冲突 | 记录 |
| --- | --- |
| Catalog dtypes list e5m2 | live example is e4m3 |
| README scale `(m, ceil(k/64), 2)` vs `MX_SCALE_GROUP_NUM=32` | packing vs group; do not invent a second group size |
| Default binary Identity `<3,0>` vs optest ASWT + four transposes | not the same path |
| README L1-resident recipe vs live `false` | 不调和 |
| `CanImplement` always true | not a dtype/shape guard |

This file does not attempt to resolve the conflicts.

---

## Review questions

1. **Independent Case?** **Yes, as the MX block-scale GEMM** — teaching is `MmadMx` + e8m0 group-32 contract + scale-aware TileCopy, not 43 pingpong. Pick **one** scheduler for the Case (recommend default Identity executable); ASWT as sibling note, not a second Case.
2. **Engineering Knowledge now?** MX scale layout / group-32 vs packed-64 README is Case-local until another MX example is curated. Do not fold into `gemm-k-partition-strategies.md`.
3. **Core difference vs 43:** Block policy and operands (fp8 + scales). Kernel is still MN tiled Cube, AIV empty.

Open items (Case chose):

- Case live path is the **Identity** executable. ASWT remains a sibling binary; optest still only covers ASWT.
- e5m2 stays template_scope, not this Case’s `dtypes`.
- 54/59 share `MmadMx` (`inference` in catalog); not ingested in this batch.
