# Close-read: 68 Ascend950 Multi-Core SplitK Matmul

Status: **reviewed (passed)**. Case: `cases/compute/gemm/ascend950-multi-core-splitk-matmul/`. Index: `knowledge/engineering-experience/gemm-k-partition-strategies.md`.

- Candidate: `catlass-68-ascend950-multi-core-splitk`
- Commit: `a8b4fffbd9cabc021dfd47253e907a1cde59de25`
- Golden sample: `cases/compute/gemm/ascend950-basic-matmul/`
- Pair: `66_ascend950_streamk_matmul.md` + `66_vs_68_k_partition_comparison.md`
- Scope: live `MultiCoreSplitkMatmulTla` + `MmadPingpong` + UnitFlag + fp32. Padding SplitK, single-core SplitK, quant SplitK, example 09/22/34/52/69 are non-active.

Evidence kinds: `source-code` | `design-doc` | `test` | `benchmark` | `inference`.

No `benchmark` evidence in this example tree. README “能获取较优的性能” is **design-doc only**.

---

## What this example actually instantiates

Shipped `examples/68_ascend950_multi_core_splitk_matmul/multi_core_splitk_matmul.cpp`.

| Item | Live value | Evidence |
| --- | --- | --- |
| Arch | `Arch::Ascend950` | source-code example |
| Executable | **mix** | `CMakeLists.txt`: `... mix ...` |
| A/B/C dtype | `float` (fp32) | `using ElementA/B/C = float` |
| Accumulator | `float` | same as C on this path |
| Bias | `void` | example `ElementBias = void` |
| Layout A/B/C | all `RowMajor` | source-code example |
| DispatchPolicy | `MmadPingpong<Ascend950, UnitFlag=true, HF32=false>` | 3-arg form; same defaults as 66 |
| L1 tile | `256 × 256 × 128` | same as 43 fp32 |
| L0 tile | `256 × 256 × 32` | same as 43 fp32 |
| Block | `BlockMmadTla<...>` | **same Block as 43** |
| Scheduler | `SplitkGemmIdentityBlockSwizzle<3, 0>` | dir **fixed 0** |
| Epilogue template | `void` | |
| Workspace | `max(2 MiB, M×N×4×splitkFactor)` | `GetWorkspaceSize` |
| AIV | **used**: `SplitkReduceAdd` over full C | `operator()<AIV>` |
| Host cores | **all** `GetCoreNumAic()` | |
| `CanImplement` | `true` | this kernel only |
| `ReluEnable` | must be false | `static_assert` |

Catalog lists `dtypes: fp16` and `compute_engine: cube`. Live example is **fp32 mix**. Record, do not resolve.

CLI (`design-doc` README): `./68_... 256 512 1024 [device_id]`.

`test` optest: `256×512×1024`, fp32 only, vs `torch.matmul`, rtol/atol 1e-3. 950 skip via device name `Ascend950(PR|DT)`.

JIT (`source-code` `multi_core_splitk_matmul_impl.cpp`): same `TileShapeScalerTLA` as 66. fp32 → L1 K **64**, L0 K **16**. Example does **not** scale. Conflict, do not resolve. JIT also forces `CATLASS_JIT_BLOCK_SCHEDULER = "30"` (offset 3, dir 0), matching the example.

---

## Vs 43: what actually changed

Cube path inside BlockMmad is still 43 pingpong. 68 never writes GM C from AIC.

```text
Host DeviceGemm  (mix, all AIC, workspace ≠ 0)
  Kernel MultiCoreSplitkMatmulTla
    AIC: SplitkGemmIdentityBlockSwizzle
         every MN tile × every K-slice → BlockMmad → workspace plane[slice]
    AIV: wait flag, SplitkReduceAdd(workspace planes → GM C)
```

`splitk_matmul.hpp` is included for **`SplitkReduceAdd` only**. The A2 `SplitkMatmul` kernel class is not the live type.

---

## K partition (live)

Host computes `splitkFactor` once (`source-code` `GetSplitkFactor`):

```text
blockNum = CeilDiv(M, L1_M) * CeilDiv(N, L1_N)
kTileNum = CeilDiv(K, L1_K)
splitkFactor = 2
if aicCoreNum / blockNum > 0:
    splitkFactor = aicCoreNum / blockNum    // integer division
splitkFactor = min(splitkFactor, kTileNum)
```

If `blockNum > aicCoreNum`, the `if` is false and the factor **stays 2** (then capped by `kTileNum`). The README heuristic is not a runtime disable; the kernel still SplitKs.

Scheduler (`source-code`):

```text
coreLoops = blockNum * splitkFactor
splitkSliceIdx(taskIdx) = (taskIdx % coreLoops) / blockNum
kIdx = GetKIdxBySplitkSliceIdx(sliceIdx)
```

K-tiles are split as evenly as possible across `splitkFactor` slices: the first `(kTileNum % splitkFactor)` slices get one extra L1-K. `GetActualBlockShape` uses that slice length; the last slice also clips to remaining `K`.

AIC assignment: `for loopIdx = blockIdx; loopIdx < coreLoops; loopIdx += GetBlockNum()` — same stride as 43, over a **K-extended** task list.

### Which K is split

**Every MN tile’s K**, not only a tail wave. Each MN tile is computed `splitkFactor` times, each time with a disjoint K range.

### Who gets the pieces

Any AIC that draws that `(MN tile, slice)` task. There is no “this MN tile stays on one core.” Fill is `blockNum * splitkFactor` tasks, not a guarantee of `aicCoreNum` busy cores (capped by `kTileNum`).

### Where partials live

Workspace is `splitkFactor` **full `M×N` accumulator planes**:

```text
tensorC = gmW[ M*N * splitkSliceIdx ]
then GetTile at the MN block origin
```

`source-code` AIC. Layout of that plane is RowMajor `(M, N)` in **accumulator** dtype, even though problem C is the same dtype here.

AIC **does not** touch GM C.

### Who reduces

**AIV** via `SplitkReduceAdd<ArchTag, acc, C, STAGES=1, computeLength>` (`source-code`).

`computeLength = 192 * 1024 / sizeof(ElementAccumulator)` — **192 KiB**, not `Arch::Ascend950::UB_SIZE` (248 KiB). `static_assert` only requires `STAGES * computeLength * sizeof(acc) <= UB_SIZE`. Why 192 KiB on 950 is **not** sourced here (A2 UB is 192 KiB in `Arch::AtlasA2`). Do not invent a reason.

Reduce (`source-code` `SplitkReduceAdd::operator()`):

- Split the **flattened** `M*N` elements across all AIVs (`GetBlockNum()*GetSubBlockNum()`).
- Each AIV copies `splitkFactor` planes of its element range into UB (`DataCopyPad` with `blockCount=splitkFactor` when the span fits the 32-bit length check).
- Fold with `Add`, optional `Cast` if C ≠ acc, `DataCopyPad` to GM C.

This is a **global** reduction of every output element, not per-MN-tile like 66.

### Sync

| Event | API | When |
| --- | --- | --- |
| AIC done | `CrossCoreSetFlag<0x2, PIPE_FIX>` | **after the entire AIC task loop** |
| AIV wait | `CrossCoreWaitFlag<0x2, PIPE_MTE2>` + `CrossCoreBarrier<0x0, PIPE_MTE2>` | AIV entry |
| Intra-AIV | MTE3/MTE2/V flags inside `SplitkReduceAdd` | |

No early flag, no “reduce while Cube finishes leftover MN tiles.” All Cube K-slices finish, then Vector reduces.

---

## Trigger (documented vs live)

README (`design-doc` only):

```text
when CeilDiv(M,m1)*CeilDiv(N,n1) <= C/2
```

Practice doc for A2 09/22 (`design-doc` `matmul_kernels_optimization.md`): SplitK when `taskBlocks < aicCoreNum` and K is large enough; cost is workspace + AIV ReduceAdd. No numbers attached to example 68.

**Live kernel always SplitKs.** `GetSplitkFactor` still returns at least `min(2, kTileNum)` even when `blockNum > C`.

Shipped CLI `256×512×1024`: `B = 2`. For `C >= 4`, `B <= C/2` matches the README predicate. Whether that is faster than 43 is **not measured here**.

---

## Buffer math (Cube, fp32, same as 43)

Identical L1/L0/L0C occupancy to 43 / 66 on this dtype and these tiles.

Workspace (example shape, fp32): `M×N×4×splitkFactor`. For `256×512×1024`, `kTileNum = 8`. If `aicCoreNum / 2 >= 8`, `splitkFactor = 8` → `256×512×4×8 = 4 MiB` (above the 2 MiB floor). If fewer AIC, factor shrinks. Do not hard-code a chip’s `C`.

---

## What is *not* running

| Item | Status |
| --- | --- |
| Example 09 `SplitkMatmul` kernel class | different template; only `SplitkReduceAdd` is reused |
| 22 padding / 34 single-core / 52 quant / 69 tail SplitK | different examples |
| Bias / Relu | `void` / `static_assert` |
| StreamK scheduler / 2-slot workspace | 66, not 68 |
| Runtime `if B <= C/2` else 43 | does not exist |

---

## Tests

`test_68_ascend950_multi_core_splitk_matmul.py`: 950-only, one fp32 shape `256×512×1024`, MIX JIT.

---

## What NOT To Copy (if this becomes a Case)

1. Do not treat catalog fp16 / cube as the live path.
2. Do not assume SplitK fills every AIC — `splitkFactor` is capped by `kTileNum`.
3. Do not copy `192 KiB` reduce UB as “950 UB size.”
4. Do not copy `SwizzleOffset=3` / dir 0 as a measured optimum.
5. Do not use README `B <= C/2` as a benchmark or as a binary gate.
6. Do not alias with 66: 68 splits **all** MN tiles’ K and reduces **full C**.
7. Do not use this `CanImplement` as a legality guard.

---

## Review questions

1. **Independent Case?** **Yes, as the SplitK sibling of 66**, not as a second 43. Teaching is global K factor + `M×N×factor` workspace + `SplitkReduceAdd`. Weaker intrinsic score than 66 is deserved: the Cube story is thinner.
2. **Knowledge?** Same proposed file as 66. 68 without 66 teaches “split K when MN is small” and Agents will then SplitK a tail-round problem.
3. **Core difference vs 43:** AIC destination is workspace planes; AIV owns GM C.

Open items:

- Example vs JIT L1-K for fp32.
- Catalog dtype/engine vs live.
- Hardcoded 192 KiB reduce length vs 950 `UB_SIZE`.
- No `benchmark` for the README heuristic.
