# Close-read: 66 Ascend950 StreamK Matmul

Status: **reviewed (passed)**. Case: `cases/compute/gemm/ascend950-streamk-matmul/`. Index: `knowledge/engineering-experience/gemm-k-partition-strategies.md`.

- Candidate: `catlass-66-ascend950-streamk-matmul`
- Commit: `a8b4fffbd9cabc021dfd47253e907a1cde59de25`
- Golden sample: `cases/compute/gemm/ascend950-basic-matmul/`
- Pair: `68_ascend950_multi_core_splitk_matmul.md` + `66_vs_68_k_partition_comparison.md`
- Scope: live `StreamkMatmulTla` + `MmadPingpong` + UnitFlag + fp32. Non-TLA `streamk_matmul.hpp`, `block_mmad_streamk.hpp`, example 37/69, bias, Relu Fixpipe are non-active.

Evidence kinds: `source-code` | `design-doc` | `test` | `benchmark` | `inference`.

No `benchmark` evidence exists in this example tree. README “较优性能” is **design-doc only**.

---

## What this example actually instantiates

Shipped `examples/66_ascend950_streamk_matmul/streamk_matmul.cpp`.

| Item | Live value | Evidence |
| --- | --- | --- |
| Arch | `Arch::Ascend950` | source-code example |
| Executable | **mix** | `CMakeLists.txt`: `catlass_example_add_executable(... mix ...)` |
| A/B/C dtype | `float` (fp32) | `using ElementA/B/C = float` |
| Accumulator | `float` | same as C on this path |
| Bias | `void` | example `ElementBias = void` |
| Layout A/B/C | all `RowMajor` | source-code example |
| DispatchPolicy | `MmadPingpong<Ascend950, UnitFlag=true, HF32=false>` | 3-arg form; defaults L1A/B 2/2, L0A/B 2/2, L0C 1 |
| L1 tile | `256 × 256 × 128` | M, N, K |
| L0 tile | `256 × 256 × 32` | same pack as 43 fp32 |
| Block | `BlockMmadTla<...>` | **same Block as 43** |
| Scheduler | `StreamkGemmIdentityBlockSwizzle<3, 0>` | dir **fixed 0**, not 43’s `M>N` test |
| Epilogue template | `void` | writeback still inside BlockMmad |
| Workspace | `max(2 MiB, L1_M×L1_N×4×aicCoreNum×2)` | `GetWorkspaceSize` |
| AIV | **used**: reduce streamk partials | `operator()<AIV>` is not empty |
| Host cores | **all** `GetCoreNumAic()` | like 60, unlike 43 `min(aic, taskNum)` |
| `CanImplement` | `true` | this kernel only |

Catalog currently lists `dtypes: fp16` and `compute_engine: cube`. Live example is **fp32 mix**. Record, do not resolve in this file.

CLI (`design-doc` README): `./66_... 256 512 1024 [device_id]`. Same numbers in the README body.

`test` optest default: `256×512×3079`, fp32, vs `torch.matmul`, rtol/atol 1e-3. Extra test instantiates fp16; that is **not** the shipped example.

JIT (`source-code` `ascend950_streamk_matmul_impl.cpp`) applies `TileShapeScalerTLA<ElementA, half, BaseL1=256/256/128>`. For fp32, `sizeof(float)/sizeof(half)=2` → L1 K becomes **64**, L0 K **16**. Shipped example does **not** scale. Example vs optest tile K: **conflict, do not resolve**.

---

## Vs 43: what actually changed

Cube data path inside one BlockMmad call is the 43 pingpong (GM→L1→L0→Mmad→Fixpipe). 66 does not introduce a new Cube policy.

New layers:

```text
Host DeviceGemm  (mix, all AIC, workspace ≠ 0)
  Kernel StreamkMatmulTla
    AIC: StreamkGemmIdentityBlockSwizzle
         · full-K MN tiles → BlockMmad → GM C     (normal)
         · K-sliced tail MN tiles → BlockMmad → workspace (streamk / cross)
    AIV: wait flag, add workspace slices, write GM C
```

`block_mmad_streamk.hpp` is **not** in this example’s type stack.

---

## K partition (live)

Let

```text
C      = blockDim = GetBlockNum()          // launched AIC count
m1,n1,k1 = 256, 256, 128
B      = CeilDiv(M,m1) * CeilDiv(N,n1)     // MN tiles
kTiles = CeilDiv(K,k1)
streamkBlocks = B % C
normalBlocks  = B - streamkBlocks
kTileNumPerCore = (streamkBlocks * kTiles) / C
kTileRemain     = (streamkBlocks * kTiles) % C
```

`source-code` constructor of `StreamkGemmIdentityBlockSwizzle`.

### Which K is split

**Only the last incomplete wave of MN tiles** (`streamkBlocks = B % C`). Each of those MN tiles still has its full K, but that K is cut into L1-K pieces and **strewn across AIC cores** so the tail wave uses up to `min(streamkBlocks * kTiles, C)` core-loops.

Complete waves (`normalBlocks`) keep **full K on one core**, same as 43.

If `streamkBlocks == 0`, no K is split. AIV reduce loop runs zero times.

### Who gets the pieces

`GetStreamkBlockDec(loopIdx)` (`source-code`):

- `loopIdx < normalBlocks`: `isStreamkBlock=false`, `actualBlockShape.k() = K`.
- else: assign a contiguous run of L1-K tiles. First `kTileRemain` streamk cores get `kTileNumPerCore+1` tiles; the rest get `kTileNumPerCore`. `blockCoord.k` is the starting L1-K index.

If a core’s K-run **crosses an MN-tile boundary** (`kTileIdx % kTiles + curKTileNum > kTiles`): `isCrossBlock=true`. The overflow is a second BlockMmad onto the **next** streamk MN tile (`streamkBlockCoord`), written to workspace slot 1.

Kernel also **swaps** one loop index per streamk core when `normalBlocks > 0`: that core’s last-wave normal slot is exchanged with its first streamk slot (`source-code` AIC loop). Effect: streamk K-work is issued before that core’s leftover full-K MN tile. Practice doc for example 37 calls this “尾轮提前计算” (`design-doc` `docs/zh/1_Practice/others/fused_kernel_optimization.md`). 66’s README does **not** mention it. The swap is in 66 source; calling it “overlap with AIV reduce” is **inference**.

### Where partials live

| Block kind | Destination | Layout |
| --- | --- | --- |
| normal | GM C | problem C, RowMajor |
| streamk (same MN tile) | workspace slot `2 * blockIdx` | `actualM × actualN`, row stride `L1_N` |
| streamk cross (next MN tile) | workspace slot `2 * blockIdx + 1` | same |

Workspace size (`source-code`): two `L1_M×L1_N` accumulator tiles **per AIC**, floored at 2 MiB. Not a full `M×N×slices` cube.

AIV reads (`source-code`): for streamk MN tile `skBlockId`, contributing AIC range `[startCoreIdx, endCoreIdx)` from `GetCoreIdx`. If `IsCross(skBlockId)` (head of this tile sits in the previous core’s odd slot), first copy comes from slot `2*startCoreIdx+1`, then remaining slices from each core’s even slot `2*(startCoreIdx+sliceIdx)`.

### Who reduces

**AIV**, inline in `StreamkMatmulTla::operator()<AIV>` (`source-code`). Not `SplitkReduceAdd`, not `StreamkReduceAdd` from example 37.

Only streamk MN tiles are reduced. Normal tiles already sit in GM C.

Per tile: `DataCopyPad` slices into UB → `Add` fold → optional `Cast` if `ElementC != ElementAccumulator` → `DataCopyPad` to GM C. AIV comment: `layoutC can only be RowMajor`. Live example is RowMajor.

Row split among AIVs of the contributing AICs: `laborCoreNum = (endCoreIdx-startCoreIdx)*GetSubBlockNum()`, clipped by `UB_SIZE / sizeof(acc) / laborCoreNum / RoundUp(N, ELE_NUM_ALIGN)`.

### Sync

| Event | Pipe / API | When |
| --- | --- | --- |
| AIC → AIV “workspace ready” | `CrossCoreSetFlag<0x2, PIPE_FIX>` | cores with `blockIdx >= streamkCores` at AIC **entry**; streamk cores after the swapped streamk iteration (or every loop if `normalBlocks==0`) |
| AIV wait | `CrossCoreWaitFlag<0x2, PIPE_MTE2>` then `CrossCoreBarrier<0x0, PIPE_MTE2>` | AIV entry, **before** any reduce |
| Intra-AIV | `MTE3_MTE2` / `MTE2_V` / `V_MTE3` | copy / add / store |

`streamkCores = coreLoops - normalBlocks`. Flag ID `FLAG_AIC_FINISH = 0`.

There is **no** AIC atomic add into C. Reduce is Vector on GM workspace.

---

## Trigger (documented vs live)

README (`design-doc` only):

```text
B = CeilDiv(M,m1)*CeilDiv(N,n1)
use StreamK when B/C > 1  and  B%C <= 0.8 C
```

Same predicate appears in `fused_kernel_optimization.md` for **example 37** (`design-doc`). No table, no ms, no TFLOPS in 66.

Practice doc (`design-doc`): StreamK is for **tail-round idle**, not for `B < C`. `B < C` should consider SplitK or smaller MN tiles first.

**Live kernel always instantiates StreamK.** There is no host `if (B/C > 1)` switch to 43.

Shipped CLI / optest MN = `256×512` with these tiles → `B = 2`. For any `C > 2`, `B/C` is not `> 1`. That shape sits in the README’s **SplitK** window (`B <= C/2` in 68), not the StreamK window. The scheduler still treats all 2 MN tiles as `streamkBlocks` (`2 % C = 2`, `normalBlocks = 0`). Correctness tests that path; they do **not** demonstrate a multi-wave tail.

Do not write: “66 is faster on 256×512×1024.” No `benchmark` evidence.

---

## Buffer math (Cube, fp32, same as 43)

| Buffer | This example |
| --- | --- |
| L1A / L1B | `256×128×4×2 = 256 KiB` each |
| L0A / L0B | `256×32×4×2 = 64 KiB` each |
| L0C | `256×256×4×1 = 256 KiB` |

UB is used on AIV for reduce (`Arch::Ascend950::UB_SIZE`). AIC BlockMmad does not allocate a C UB on this path.

---

## What is *not* running

| Item | Status |
| --- | --- |
| `examples/37_streamk_matmul` / `StreamkReduceAdd` | different example / different reduce type |
| `block_mmad_streamk.hpp` | not in the type stack |
| README’s `37_ascend950_tail_multi_core_splitk_matmul` | that path string is not this example; 950 tail-SplitK is example **69** |
| Bias / Relu Fixpipe | `void` / `static_assert(!ReluEnable)` |
| 43 Mutex / Preload / ShuffleK | not instantiated |
| Runtime “only if B/C>1” gate | does not exist |

存在 ≠ 运行 ≠ 本候选教学内容。

---

## Tests

`test_66_ascend950_streamk_matmul.py`: 3510-only. fp32 `256×512×3079`; extra fp16 with looser tol. JIT `JitKernelType::MIX`. Scheduler still `<3,0>`.

---

## What NOT To Copy (if this becomes a Case)

1. Do not treat catalog fp16 / cube as the live path.
2. Do not copy 37’s `StreamkReduceAdd` or A2 tiles into 66.
3. Do not copy `SwizzleOffset=3` / dir 0 as a measured 950 optimum.
4. Do not use README `B/C>1 && B%C<=0.8C` as a benchmark result or as something the binary checks.
5. Do not describe StreamK as “always split all K.” Normal waves are full-K 43 tiles.
6. Do not alias this with 68 SplitK (different scheduler, workspace, reduce helper, trigger).
7. Do not use this `CanImplement` as a legality guard.

---

## Review questions

1. **Independent Case?** **Yes, as the StreamK half of a pair** — but only after / together with 68 and a K-partition knowledge file. Cube Block is 43; the teaching is tail-wave K scatter + 2-slot workspace + inline AIV reduce + early flag.
2. **Engineering Knowledge now?** **Yes (proposed, not created this batch):** `knowledge/compute/gemm/gemm-k-partition-strategies.md`. 66 alone cannot teach “when not to StreamK.”
3. **Core difference vs 43:** Kernel + mix AIV. Same pingpong Block and fp32 256/128/32 tiles.

Open items:

- Example vs JIT L1-K (128 vs 64 for fp32).
- Catalog dtype/engine vs live fp32 mix.
- README heuristic vs shipped `256×512` (`B=2`).
- README filename `37_ascend950_tail_multi_core_splitk_matmul` vs tree (37 is A2 StreamK; 69 is 950 tail SplitK). Do not invent a resolution.
- Whether the loop-index swap is “for AIV overlap”: **inference**.
