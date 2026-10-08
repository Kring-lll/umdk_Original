# Close-read: 43 Ascend950 Basic Matmul

Status: **accepted**. First Case Card is `cases/compute/gemm/ascend950-basic-matmul/`.

- Candidate: `catlass-43-ascend950-basic-matmul`
- Commit: `a8b4fffbd9cabc021dfd47253e907a1cde59de25`
- Scope: live `MmadPingpong` + UnitFlag + fp32 only. Mutex / Preload / ShuffleK are non-active.

## What this example actually instantiates

The shipped `examples/43_ascend950_basic_matmul/basic_matmul_tla.cpp` is **not** a generic fp16 kernel.

| Item | Value | Evidence |
| --- | --- | --- |
| Arch | `Arch::Ascend950` | source-code example |
| A/B/C dtype | `float` (fp32) | `using ElementA/B/C = float` |
| Bias | `void` (disabled) | `using ElementBias = void` |
| Layout A/B/C | all `RowMajor` | source-code |
| DispatchPolicy | `MmadPingpong` | default; Mutex/Preload are commented |
| UnitFlag | `true` | and therefore `l0CStages = 1` |
| HF32 | `false` | |
| L1 resident | `false` | |
| L1 A/B stages | 2 / 2 | pingpong GM→L1 |
| L0 A/B stages | 2 / 2 | pingpong L1→L0 |
| L1 tile | `256 × 256 × 128` | M, N, K |
| L0 tile | `256 × 256 × 32` | M, N, K |
| Epilogue | `void` | no independent fused epilogue; L0C→GM is inside BlockMmad via `CopyL0CToDst` on 3510 |
| Workspace | 0 | `GetWorkspaceSize` returns 0 |
| AIV entry | empty | `operator()<AIV>` is `{}`; live compute is AIC |

Case `dtypes` are **fp32 in/out/acc** only. Template-supported dtypes are a different concept and are **not** in this Case’s metadata.

## Buffer math for the selected tiles

`Arch::Ascend950` (`include/catlass/arch/arch.hpp`): L1 512KiB, L0A 64KiB, L0B 64KiB, L0C 256KiB.

BlockMmad allocation formulas (`block_mmad_pingpong_tla.hpp`), fp32:

- L1A: `L1_M × L1_K × 4 × 2 = 256×128×4×2 = 256KiB`
- L1B: `L1_N × L1_K × 4 × 2 = 256×128×4×2 = 256KiB`
- L0A: `L0_M × L0_K × 4 × 2 = 256×32×4×2 = 64KiB`
- L0B: `L0_K × L0_N × 4 × 2 = 32×256×4×2 = 64KiB`
- L0C: `L1_M × L1_N × 4 × 1 = 256×256×4×1 = 256KiB`

With fp32 and the selected stage counts, these tile sizes fully consume the listed L1/L0A/L0B/L0C capacities.

L0A uses `M×K`; L0B uses `K×N`. They match here because `L0_M == L0_N == 256`, not because the formulas are identical. L0B layout tag is `nZ`; the 64KiB figure is `L0B_TILE_SIZE` allocation, not extra fractal padding.

（**inference**：tile 可能是贴着容量边界选的。不要写成“作者就是为了填满才选这些数”。）

Do not treat 256/128/32 as a universal Cube tile.

Static asserts in `block_mmad_pingpong_tla.hpp`:

- `L1_M == L0_M` and `L1_N == L0_N`
- `L0_K <= L1_K` (here 32 vs 128 → 4 L0-K steps per L1-K)
- `UnitFlag` ⇒ `L0C_STAGES == 1`
- HF32 only if A and B are `float`

## Layering I believe is correct

```text
Host example
  DeviceGemm<BasicMatmulTla<BlockMmad, void, Swizzle>>
    Kernel: assign MN blocks, tile GM, call BlockMmad
      BlockMmadTla<MmadPingpong, ...>
        TileCopy: GM→L1, L1→L0, L0C→GM
        TileMmad
```

Live compute is entirely on AIC. The AIV entry exists but is empty. This is **not** AIC+AIV MIX, and it is **not** “a kernel with no AIV entry”.

Kernel (`basic_matmul_tla.hpp`):

1. `BlockScheduler` from problem shape and L1 MN tile
2. Each AIC walks `loopIdx = blockIdx; loopIdx < coreLoops; += blockNum`
3. Builds TLA GM tensors; **L2 bypass**: if `M` fits one L1-M tile, disable L2 for B; if `N` fits one L1-N tile, disable L2 for A. CATLASS-specific; not generic knowledge.
4. Calls `blockMmad` on the MN block; K is consumed inside BlockMmad
5. `PipeBarrier<PIPE_ALL>()` at the end

Host also sets `aicCoreUsed = min(aicCoreNum, taskNum)` so idle cores are not launched when MN tiles < core count.

## Pipeline I believe is correct

BlockMmad K-loop (`block_mmad_pingpong_tla.hpp`):

1. Prologue: load first A/B L1 tiles (GM→L1), MTE1/MTE2 flags
2. For each L1-K:
   - **Preload next L1-K** onto the other L1 stage (ordinary pingpong, not `MmadPreloadAsyncWithCallback`)
   - Inner loops: L0-M × L0-K × L0-N
   - L1→L0A / L1→L0B with M/MTE1 flags
   - Mmad into L0C; `initC` only on first K
3. After K: Fixpipe L0C→GM (`CopyL0CToDst` when `CATLASS_ARCH==3510`)

With `ENABLE_UNIT_FLAG && RowMajor C`, constructor calls `SetMMLayoutTransform(true)`. Not independently verified.

## Swizzle

Host: if `M > N` use `GemmIdentityBlockSwizzle<3, 0>` else `<3, 1>`.

Observed: `SwizzleOffset = 3`.

Interpretation（**inference**）：likely intended to improve traversal locality. No benchmark that 3 is optimal.

## CanImplement (this instance only)

Call chain for this example:

1. Host: `matmulOp.CanImplement(arguments)` on `DeviceGemm` (return value unused)
2. Device: forwards to `GemmKernel::CanImplement`
3. Kernel: `BasicMatmulTla::CanImplement` is `return true`

Do **not** write that all CATLASS BasicMatmul / all CATLASS kernels do this. `sparse_matmul_tla.hpp` has real size/shape checks. Non-TLA `basic_matmul.hpp` also returns true, but that is a sibling kernel, not this Case’s claim.

## What is *not* running in this example

| Commented / unused | Status |
| --- | --- |
| `MmadPingpongMutex` | Commented. `dispatch_policy.hpp` `static_assert` **only `Arch::Ascend950`**. README says “A5 新增 Mutex”. **Unresolved upstream conflict. Do not explain it away.** This Case does not teach Mutex. |
| `MmadPreloadAsyncWithCallback` + `enableShuffleK` | Commented. Not this Case. |
| Bias | Type is `void` |
| L1 resident | Off |
| AIV compute | Empty body |

存在 ≠ 运行 ≠ 本 Case 教学内容。

## Tests

`tests/optest/tests/test_43_ascend950_basic_matmul.py`: 3510-only, `m,n,k = 256,512,1024`, fp32, `torch.matmul`, rtol/atol 1e-3. Same dtype as the example.

## Catalog / Case Card wording (frozen)

1. `dtypes`: shipped fp32 only. No `template_scope` in v1.
2. `pipeline_pattern`: `double-buffer` only.
3. `compute_engine: cube`. Wording: live path is AIC; AIV entry exists but is empty.
4. L2 bypass lives under Interesting Implementation Details, not Key Optimization Pattern.
5. Mutex hardware scope is recorded as a conflict, not resolved.
6. Do not copy L1=256 as default for fp16.
