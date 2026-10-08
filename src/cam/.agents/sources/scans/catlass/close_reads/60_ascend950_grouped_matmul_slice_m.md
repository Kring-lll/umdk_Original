# Close-read: 60 Ascend950 Grouped Matmul Slice-M

Status: **Case Card written** at `cases/compute/grouped-gemm/ascend950-grouped-matmul-slice-m/`. This file remains the close-read.

- Candidate: `catlass-60-ascend950-grouped-matmul-slice-m`
- Commit: `a8b4fffbd9cabc021dfd47253e907a1cde59de25`
- Golden sample: `cases/compute/gemm/ascend950-basic-matmul/`
- Scope: live `GroupedMatmulSliceMTla` + `MmadPingpong` + UnitFlag + fp16. Slice-K / dequant GMM / segmented groupList are non-active.

Evidence kinds used below: `source-code` | `design-doc` | `test` | `inference`.

---

## What this example actually instantiates

Shipped `examples/60_ascend950_grouped_matmul_slice_m/grouped_matmul_slice_m.cpp`.

| Item | Live value | Evidence |
| --- | --- | --- |
| Arch | `Arch::Ascend950` | source-code example |
| Executable | **cube** (not MIX) | `CMakeLists.txt`: `catlass_example_add_executable(... cube ...)` |
| A/B/C dtype | `half` (fp16) | `using ElementA/B/C = half` |
| Accumulator | `float` | `ElementAccumulatorSelector<half,half>` → `float` |
| Bias | `void` | `BlockMmadTla<..., void, TileCopy>` |
| Layout A/B/C | all `RowMajor` | source-code example |
| `groupList` element | `int64_t` | kernel template arg; host `GenerateGroupList<int64_t>` |
| `groupList` form | **prefix-sum** | kernel: `currentM = groupList[0]` or `groupList[i]-groupList[i-1]`; optest `cumsum` |
| DispatchPolicy | `MmadPingpong<Ascend950, UnitFlag=true, HF32=false>` | defaults: L1A/B 2/2, L0A/B 2/2, L0C 1 |
| L1 tile | `256 × 256 × 256` | M, N, K |
| L0 tile | `256 × 256 × 64` | M, N, K |
| Epilogue | `void` | writeback inside BlockMmad (`CopyL0CToDst`), same as 43 |
| Workspace | 0 | `GetWorkspaceSize` returns 0 |
| AIV entry | empty | `operator()<AIV>` is `{}` |
| Host cores | **all** `GetCoreNumAic()` | unlike 43, no `min(aicCoreNum, taskNum)` |

README (`design-doc`): A is split on M, then multiplied with per-group B. Matches the live buffers:

- A: `m × k` (groups concatenated on M)
- B: `k × n × problemCount` (one B per group)
- C: `m × n` (groups concatenated on M)

CLI: `./60_... <group_count> <m> <n> <k> [device_id]`. README example: `128 512 1024 2048`.

Catalog `dtypes` already match this shipped example (fp16 in/out, fp32 acc). Do **not** add other template dtypes.

---

## `groupList` contract (live path)

`source-code` kernel:

```text
groupIdx = 0 .. problemCount-1
currentM = (groupIdx == 0) ? groupList[0]
                           : groupList[groupIdx] - groupList[groupIdx - 1]
inGroupProblemShape = (currentM, N, K)
```

`test` optest: `GROUP_SIZES = (128,128,128,128)` then `cumsum` → prefix-sum on device.

`source-code` host generator `golden::GenerateGroupList`: random values in `[0, m]`, then sort. That is prefix-sum-shaped. Last value is the **max of those randoms**, not forced to equal CLI `m`. Host still allocates A as `m*k`. Compare uses `groupList[problemCount-1] * n`.

If `CATLASS_EXPERIMENTAL_GROUPLIST_SEGMENTED` is defined, the generator converts the sorted list into **per-group sizes**. This example’s CMake does **not** define that macro. Kernel always subtracts adjacent entries. Segmented form is **not** the live path.

Do not write: “groupList is a list of expert token counts.” Live encoding is prefix-sum boundaries.

---

## Buffer math for the selected tiles

`Arch::Ascend950`: L1 512KiB, L0A/L0B 64KiB, L0C 256KiB.

Formulas from `block_mmad_pingpong_tla.hpp`, **fp16** A/B, **fp32** acc, 2/2/1 stages:

| Buffer | Formula | This example |
| --- | --- | --- |
| L1A | `L1_M × L1_K × 2 × 2` | `256×256×2×2 = 256 KiB` |
| L1B | `L1_N × L1_K × 2 × 2` | `256×256×2×2 = 256 KiB` |
| L0A | `L0_M × L0_K × 2 × 2` | `256×64×2×2 = 64 KiB` |
| L0B | `L0_K × L0_N × 2 × 2` | `64×256×2×2 = 64 KiB` |
| L0C | `L1_M × L1_N × 4 × 1` | `256×256×4×1 = 256 KiB` |

With fp16 and these stage counts, these tiles fully consume listed L1 / L0A / L0B / L0C.

This is **not** 43’s fp32 256/128/32 pack. Same pingpong policy, different dtype → different K tiles (`L1_K=256`, `L0_K=64` → still 4 L0-K steps per L1-K).

（**inference**：fp16 下把 K 加倍以贴满同一套容量。不要写成“作者就是为了填满才选这些数”。）

UB is unused on the live cube path.

---

## Layering vs 43 golden sample

```text
Host example
  DeviceGemm<GroupedMatmulSliceMTla<BlockMmad, void, Swizzle, int64_t>>
    Kernel: walk groups, map groupList → MN tiles, call BlockMmad
      BlockMmadTla<MmadPingpong, ...>     ← same Block as 43
        TileCopy / TileMmad               ← same pingpong primitives
```

The new teaching is at **Kernel + Host `groupList`**, not a new Cube pipeline.

Live compute is AIC. AIV entry exists but is empty. `compute_engine: cube`. Same wording rule as 43: not “AIC-only architecture”, not MIX.

---

## Data flow

Per group:

```text
GM  A[totalM : totalM+currentM, :]     fp16 RowMajor
GM  B[group]                            fp16 RowMajor, offset += K*N
        │  MTE2  GM → L1  (pingpong, same BlockMmad as 43)
        ▼
L1
        │  MTE1  L1 → L0A / L0B
        ▼
L0A, L0B
        │  Mmad → L0C (fp32 acc, UnitFlag, 1 stage)
        ▼
L0C
        │  CopyL0CToDst (3510) → GM C as fp16
        ▼
GM  C[totalM : totalM+currentM, :]
```

K is consumed **inside** BlockMmad for that group’s MN tile. No cross-group K split.

---

## Parallel strategy (the actual GMM part)

For each group, Kernel:

1. Reads `currentM` from `groupList` (`GetValue` on GM).
2. `blockScheduler.Update(inGroupProblemShape, (L1_M, L1_N))`.
3. `coreLoops = scheduler.GetCoreLoops()` = MN L1 tiles for **this group**.
4. Assigns this group’s loops to cores with a **rotating start**:

```text
startLoopIdx = (coreIdx - startCoreIdx)  mod coreNum
for loopIdx = startLoopIdx; loopIdx < coreLoops; loopIdx += coreNum
    BlockMmad on that MN tile
startCoreIdx = (startCoreIdx + coreLoops) % coreNum
```

`source-code`. This is how leftover cores from a small group pick up the next group. Irregular `currentM` becomes regular 256×256 Cube tiles plus `GetActualBlockShape` tails.

Host launches **all** AIC cores even if one group has few tiles. Occupancy depends on this cross-group rotation, not on 43’s `min(cores, taskNum)`.

Swizzle (shipped example, `source-code`):

- `m / problemCount >= n` → `GemmIdentityBlockSwizzle<3,0>`
- else → `<3,1>`

That uses **CLI `m` / group count**, not each group’s `currentM`, and not 43’s `M > N`.

**Conflict / divergence — do not resolve:**

| Source | Swizzle direction heuristic |
| --- | --- |
| shipped example | `m / problemCount >= n` |
| Python docstring for the op | same as example (`m/problemCount` vs `n`) |
| optest JIT wrapper | `params.k > params.n` → dir 0 else dir 1 |

Same `SwizzleOffset = 3`. Offset 3 is observed, not a measured 950 optimum (`inference` if anyone says it improves locality).

L2 (`source-code` kernel): if `CeilDiv(currentM, L1_M) == 1`, disable L2 for **that group’s B**. No 43-style “single N-tile ⇒ disable L2 for A”. CATLASS-specific; not generic knowledge.

---

## Pipeline

Live pipeline is **the same BlockMmad pingpong as 43**: first L1-K, then current L1-K while prefetching the next onto the other L1 stage, L0 pingpong, Fixpipe to GM.

`pipeline_pattern`: **double-buffer**.

There is no extra GMM pipeline stage. README does not claim a new Cube schedule; it says the example uses `MmadPingpong` + `BlockMmadTla`.

`CanImplement`: `GroupedMatmulSliceMTla` returns `true`. Device forwards. Host ignores the return. Same class of non-guard as 43, **this kernel only**.

---

## What is *not* running

| Item | Status |
| --- | --- |
| Slice-K GMM (`05` / `grouped_matmul_slice_k`) | different example |
| 47/48 dequant GMM, 80 GMM+GELU | different examples |
| `CATLASS_EXPERIMENTAL_GROUPLIST_SEGMENTED` | not in this CMake |
| Mutex / Preload / ShuffleK / Bias / L1 resident | not instantiated |
| AIV compute | empty body |

存在 ≠ 运行 ≠ 本候选教学内容。

---

## Tests

`tests/optest/tests/test_60_ascend950_grouped_matmul_slice_m.py`: 3510-only, four groups of 128, `M=512,N=256,K=256`, fp16, `b` shape `(G,K,N)`, prefix-sum `groupList`, vs per-group `torch.matmul`, rtol/atol 1e-2.

JIT kernel is `JitKernelType::AIC`. Default layouts RowMajor. Swizzle heuristic **differs** from the C++ example (see above).

---

## What NOT To Copy (if this becomes a Case)

1. Do not treat CLI `m` as “every group has `m/G` rows.” Live `currentM` comes from prefix-sum `groupList`.
2. Do not copy 43’s fp32 256/128/32 tiles here, or copy these fp16 256/256/64 tiles to fp32.
3. Do not copy `SwizzleOffset=3` or the `m/problemCount >= n` test as a universal GMM rule. Optest already uses a different test.
4. Do not use this `CanImplement` as a legality guard.
5. Do not describe the empty AIV entry as “no AIV in the architecture.”
6. Do not import Slice-K, dequant, or MoE routing into this Case.

---

## Vs 43 Basic Matmul

| | 43 | 60 |
| --- | --- | --- |
| Change layer | golden Cube pingpong | **Kernel scheduling** over the same Block |
| Dtype | fp32 | fp16 in/out, fp32 acc |
| L1/L0 K | 128 / 32 | 256 / 64 |
| Problem | one GEMM | many GEMMs sharing N,K; A/C concatenated on M |
| Core launch | `min(aic, taskNum)` | all AIC; rotate `startCoreIdx` across groups |
| Epilogue | void / L0C→GM | same |
| Engine | cube, AIV empty | same |

---

## Review questions

1. **Independent Case?** **Yes.** This is the clean “irregular `groupList` → regular Cube tiles” sample. Proposed path: `cases/compute/grouped-gemm/ascend950-grouped-matmul-slice-m/`.
2. **Engineering Knowledge now?** Not required. The reusable idea is GMM scheduling; it is still tied to this CATLASS kernel. Later MoE Cases can cite this Case rather than a separate knowledge file.
3. **Core difference vs 43:** Kernel maps prefix-sum `groupList` onto pingpong BlockMmad; Cube data path is the 43 pattern with fp16 tiles.

Open items (do not resolve in a Case Card without more evidence):

- Example vs optest swizzle condition.
- Generator last prefix vs CLI `m`.
- Whether rotating `startCoreIdx` is load-balancing or just a way to keep all cores in the grid; **inference** if we say “for load balance.”
