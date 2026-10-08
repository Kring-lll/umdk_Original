# Close-read: 73 Ascend950 Matmul Full Load A

Status: **passed**, with addendum `73_l1a_vs_canimplement.md`. Case Card: `cases/compute/gemm/ascend950-matmul-full-loadA/`. This file stays the evidence dump.

- Candidate: `catlass-73-ascend950-matmul-full-loadA`
- Commit: `a8b4fffbd9cabc021dfd47253e907a1cde59de25`
- Golden sample: `cases/compute/gemm/ascend950-basic-matmul/`
- Scope: live `matmul_full_loadA_tla.cpp` → `MatmulFullLoadATla` + `MmadAscend950FullLoadA` + UnitFlag + fp16. A2 `25_matmul_full_loadA`, `ENABLE_L1_RESIDENT=true`, Bias, example 44 quant full-loadA are non-active.

Evidence kinds: `source-code` | `design-doc` | `test` | `benchmark` | `inference`.

No `benchmark` evidence in this example tree. README “N 轴越大收益越大 / 小 N 可能劣化” is **design-doc only**.

---

## What this example actually instantiates

Shipped `examples/73_ascend950_matmul_full_loadA/matmul_full_loadA_tla.cpp`. CMake: **cube**.

| Item | Live value | Evidence |
| --- | --- | --- |
| Arch | `Arch::Ascend950` | source-code example |
| Executable | **cube** | `CMakeLists.txt` |
| A/B/C | `half` (fp16) | example |
| Accumulator | fp32 | L0C `ElementAccumulator` |
| Bias | `void` | example |
| Layout | all RowMajor | example |
| DispatchPolicy | `MmadAscend950FullLoadA<..., UnitFlag=true, HF32=false, L0C=1, L1Resident=false, L1A=1, L1B=2, L0A=2, L0B=2>` | **L1A_STAGES must be 1** (`static_assert`) |
| L1 tile | `256 × 256 × 128` | example |
| L0 tile | `256 × 256 × 32` | example |
| Block | `BlockMmadTla<MmadAscend950FullLoadA, ...>` | 950-specific full-loadA header, **not** 43 pingpong |
| Kernel | `MatmulFullLoadATla` | `needLoadL1` skip when A GM offset unchanged |
| Scheduler (Host) | if `M > L1_M`: `GemmIdentityBlockSwizzleL1FullLoad<1, 0>` else `GemmIdentityBlockSwizzle<3, 0>` | **compile-time branch on M vs 256** |
| Epilogue | `void` | |
| Workspace | 0 | |
| AIV | empty | |
| Host cores | `min(aicCoreNum, taskNum)` | like 43 |
| `CanImplement` | **real check** | `L1_M * K * sizeof(A) + L1_K * L1_N * L1B_STAGES * sizeof(B) <= L1_SIZE` |

Host **uses** `CanImplement` and prints `kInvalid` / “MatA tile cannot be full loaded to L1.” Unlike 43, this is a legality guard on this path (`source-code`).

CLI (`design-doc` README): `./73_... 256 512 256 [device_id]`.

`test` optest: 950-only, fp16 `512×512×256` vs `torch.matmul`, rtol/atol 1e-2.

---

## Vs 43: what actually changed

Cube still GM→L1→L0→Mmad→Fixpipe. The Block **stops pingponging A**.

```text
Host DeviceGemm  (cube, min(nAic, mnTiles), workspace 0)
  Kernel MatmulFullLoadATla
    AIC:
      if needLoadL1: copy A tile of shape (L1_M, **full K of this block**) into L1
                     (L1A buffer reserved as L1_SIZE/2)
      pingpong B by L1_K
      L1 → L0 pingpong, Mmad, Fixpipe → GM C
    AIV: empty
```

`needLoadL1` is false when this core’s next MN tile has the **same A GM offset** as the previous (`source-code` Kernel). That is the reuse README describes. `ENABLE_L1_RESIDENT` is **false** here; the FullLoadA block’s resident skip for **B** is a different knob and is off.

README (`design-doc`) says the example is based on `34_ascend950_basic_matmul`. The tree’s 34 is **A2 single-core SplitK**. 950 basic pingpong Case is **43**. Record, do not invent a 34-950 basic.

---

## Full-load A (live)

Block (`source-code`):

```text
L1A_TILE_SIZE = ArchTag::L1_SIZE / 2     // reserved, not “exact A bytes”
L1B_TILE_SIZE = L1_N * L1_K * sizeof(B)
L1A_STAGES    = 1
copy A with layout MakeLayout<A>(L1_M, actualShape.k())   // K is the block’s K, not L1_K
```

Kernel `CanImplement` (`source-code`):

```text
L1Used = L1_M * problemK * sizeof(A) + L1_K * L1_N * L1B_STAGES * sizeof(B)
ok iff L1Used <= L1_SIZE
```

For live fp16 tiles `256/256/128`, CLI `K=256`: A `256×256×2=128 KiB`, B `128×256×2×2=128 KiB`. Optest `K=256` same A/B bytes; **M=512** so two M-tiles.

Do not copy 43’s L1A formula `L1_M×L1_K×stages` onto A here.

---

## Parallel Strategy

| 层级 | 本例 |
| --- | --- |
| 启动核数 | `min(nAic, mnTiles)` |
| `M <= 256` | Identity `<3,0>` — **CLI `256×512×256` is this branch** |
| `M > 256` | FullLoad swizzle `<1,0>` — **optest `512×512×256` is this branch in the example Host** |
| FullLoad swizzle | README (`design-doc`): tiles assigned contiguously per core (`0,0,..,1,1,..`) instead of round-robin |

JIT (`source-code` `ascend950_matmul_full_loadA_impl.cpp`):

- Base L1 `256×256×256`, L0 `256×256×64`, then `TileShapeScalerTLA<ElementA, half, ...>` → for fp16 scale 1, so **L1-K 256 / L0-K 64**, not the example’s 128/32.
- Default `CATLASS_JIT_BLOCK_SCHEDULER==30` → Identity `<3,0>`, **not** the Host `M>L1_M` FullLoad swizzle unless the JIT flag is 10.

Example vs JIT tile K, and example Host swizzle vs default JIT swizzle: **conflicts, do not resolve**.

---

## Pipeline

`pipeline_pattern`: **double-buffer** on B and L0. A is a single L1 buffer (`L1A_STAGES=1`). Not mix.

README N-reuse benefit: **design-doc**, no `benchmark`.

---

## What is *not* running

| Item | Status |
| --- | --- |
| 43 `MmadPingpong` / L1A stages 2 | different policy |
| `ENABLE_L1_RESIDENT=true` | example sets false |
| A2 example 25 | rejected duplicate in catalog |
| 44 quant full-loadA | different example |
| Runtime “only if N is large” gate | does not exist |

---

## Source conflicts

| 冲突 | 记录 |
| --- | --- |
| README `34_ascend950_basic_matmul` vs tree (34=A2 SplitK, 950 basic=43) | 不调和 |
| Example L1-K 128 / L0-K 32 vs JIT base 256 / 64 | 不调和 |
| CLI `M=256` Identity swizzle vs optest `M=512` FullLoad swizzle (example Host) vs JIT default Identity | 不调和 |
| README perf vs N | design-doc only |
| Catalog `enableL1Resident` story | live false; full-K A load ≠ that flag |

This file does not attempt to resolve the conflicts.

---

## Review questions

1. **Independent Case?** **Yes** — teaching is **full-K A load + cross-N A reuse** (`L1A_STAGES=1`, `needLoadL1`) plus **both** Host swizzle branches (`M<=256` Identity, `M>256` FullLoad). Do not call this L1-resident A (`ENABLE_L1_RESIDENT=false`). Cube policy is not 43. The L1-A vs CanImplement gap is a real Agent trap.
2. **Engineering Knowledge?** After a Case: a short **full-K A load + cross-N A reuse** note, plus CanImplement ≠ all buffer constraints. Not `gemm-k-partition-strategies.md`. Not 46.
3. **Core difference vs 43:** **full-K A load + cross-N A reuse**; B still pingpongs; `CanImplement` is real but partial.

Open items (Case chose):

- Case records **both** Host swizzle branches as live (`M ? L1_M`). Do not pick one unique live scheduler. CLI `256×512×256` is Identity; optest `512×512×256` is FullLoad on the example Host.
- Example vs JIT L1-K recorded, not resolved.
- README 34 filename recorded, not resolved.
- Addendum: `CanImplement` vs named `L1A_TILE_SIZE` vs README half-L1 — see `73_l1a_vs_canimplement.md`.
