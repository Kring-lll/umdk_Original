# Mini close-read: 46 UB footprint (SPLIT_M / dualDstCtl)

Status: **source + installed AscendC adaptor**. Does not open a 46 Case Card. `cases/` not modified for 46. Q6 answers `dualDstCtl=1` dst-address semantics; the 4 KiB stage vs payload gap remains.

Scope is these symbols in CATLASS `a8b4fffbd9cabc021dfd47253e907a1cde59de25`:

- `CopyL0CToUBTla` / `CopyL0CToUBMode::SPLIT_M` / `dualDstCtl`
- `PackedTileCopyTlaToUB`
- `GetSubBlockIdx` / `GetTaskRation`
- `UB_TILE_SIZE`
- `EpilogueAscend950Fixpipe`

Live 46 path: `ElementC == float == ElementAccumulator` ⇒ `SPLIT_M` ⇒ this specialization.

Q1–Q5 stay CATLASS-only. Q6 adds the installed CANN Fixpipe adaptor (`copy_cube_out_*`) plus the Tensor-API / CATLASS v1.6 dual-dst table. The `dav_c310` instruction wrapper is **not** in CANN 8.5.0 on this machine.

---

## Call chain (live 46)

```text
KernelMatmulMixFixpipeOpti
  cLocalTensor = MakeTensor(ubTensorList_[stage], layout(blockM, alignN), UB)
  AIC: BlockMmad(..., cLocalTensor, blockShape)
         └── after K: copyL0CToDst(tensorC=cLocalTensor, tensorL0C)
               └── PackedTileCopyTlaToUB remaps CopyL0CToDst
                     → CopyL0CToUBTla<..., SPLIT_M>
                       → one AscendC::Fixpipe(..., dualDstCtl=1)
  AIV: epilogueOp(cTileTensor GM, cLocalTensor UB)
         └── BlockEpilogue<EpilogueAscend950Fixpipe<true>>
               └── CopyUb2GmTla → one DataCopyPad
```

`PackedTileCopyTlaToUB` only replaces the `CopyL0CToDst` alias. It does not add a loop or a second destination pointer.

---

## Q1. After SPLIT_M, what logical M/N does each AIV own?

Answer is in `block_epilogue_fixpipe.hpp`, not in `CopyL0CToUBTla`.

Inputs to epilogue:

- `tensorOut` = GM tile `(blockM, blockN)` from the kernel (`GetTile` on C).
- `tensorIn` = the same `cLocalTensor` as AIC used: layout `(blockM, alignN)`, **not** GetTile’d on AIV.

Then (`SPLIT_M == true`):

```text
R      = GetTaskRation()          // runtime; CATLASS does not assign a constant
halfM  = CeilDiv(blockM, R)
thisM  = (blockM odd) ? (halfM - GetSubBlockIdx())
                      : halfM
thisN  = blockN                   // not alignN
```

GM tile this AIV writes:

| | Value |
| --- | --- |
| offset | `(halfM * (GetSubBlockIdx() & 0x1), 0)` |
| shape | `(thisM, thisN)` |
| early-out | `thisM * thisN <= 0` |

`CopyUb2GmTla` copy length (DataCopyPad):

| Param | Source |
| --- | --- |
| `blockCount` | `dst.originShape[0]` → `thisM` (GetTile crops originShape) |
| `blockLen` | `dst.originShape[1] * sizeof(ElementSrc)` → `thisN * 4` for fp32 |
| `srcOffset` | `tensorIn.layout()(tensorIn.coord())` → **start of this AIV’s UB tensor** |
| `dstOffset` | GM coord of that half-tile |

N is **not** split in this path. `SPLIT_N` / `dualDstCtl = 2` is a different specialization; 46 does not use it.

CATLASS never writes `GetTaskRation() == 2`. If a later Case plugs in R=2 for a full 256×256 block: `halfM=128`, even `blockM` ⇒ both AIVs `thisM=128`, offsets `(0,0)` and `(128,0)`, copy `(128, 256)`. That arithmetic is conditional on R. Do not treat R=2 as a source-code constant.

---

## Q2. Does one `CopyL0CToUBTla` move the whole 256×256 fp32 tile?

**At CATLASS level: one Fixpipe, no chunk/subtile loop.**

Evidence:

1. Unittest `L0CToRowMajorTestSplitM`: `logs.size() == 1` (one `Fixpipe` call). Shapes include `{128,256}` and `{65,127}`.
2. `CopyL0CToUBTla<SPLIT_M>` `operator()` is a single `AscendC::Fixpipe<... CFG_ROW_MAJOR_UB>(dst[dstOffset], src[srcOffset], params)`.
3. `BlockMmadTla` pingpong, UnitFlag path: after the K/L0 loops, **one** `copyL0CToDst(tensorC, tensorL0C, 0b11)`. L0C layout is `MakeLayoutL0C(mL1Actual, nL1Actual)` for the whole MN block (`L1_M == L0_M`, `L1_N == L0_N`).

Fixpipe sizes for SPLIT_M, from `dstTensor.originShape()` of `cLocalTensor`:

```text
nSize = RoundUp(originN, ELE_NUM_PER_BLK)   // fp32: ELE_NUM_PER_BLK = 32/4 = 8
mSize = RoundUp(originM, 2)                 // comment: "m must be even when split m"
```

Kernel sets `cLocalTensor` origin to `(blockM, alignN)`. For a full L1 tile `blockM=blockN=256`:

```text
alignN = RoundUp(256, 8) = 256
Fixpipe mSize = 256, nSize = 256
```

So CATLASS **requests** a Fixpipe whose `mSize×nSize` is the full block (M rounded up to even). It does **not** pass `thisM`. It does **not** loop over 128-row chunks.

What the hardware actually deposits into each Vector UB is **not** in these files.

---

## Q3. What is 124 KiB?

```text
UB_STAGES     = 2
UB_TILE_SIZE  = Arch::Ascend950::UB_SIZE / 2
              = 248 KiB / 2
              = 124 KiB     (126976 bytes)
```

`resource.ubBuf` is `LocalTensorBuffer<..., VECCALC>`, `InitBuffer(..., ArchTag::UB_SIZE)` — the **whole** 248 KiB UB of **that core**.

Constructor:

```text
ubTensorList_[i] = ubBuf.GetBufferByByte<ElementC>(UB_TILE_SIZE * i)
  i = 0 → offset 0
  i = 1 → offset 124 KiB
```

124 KiB is **one ping-pong stage** (`LocalTensor` region), not the entire AIV UB.

Kernel picks the stage with `ubListId = (loopIdx / blockNum) & 1`.

AIC and AIV each construct their own `Resource` / `ubBuf`. CATLASS does not show a second UB pointer in the Fixpipe call.

---

## Q4. Alignment / padding vs a naive 128×256

Proven in source:

| Site | Padding |
| --- | --- |
| Kernel UB layout N | `alignN = RoundUp(blockN, BYTE_PER_BLK / sizeof(ElementC))` → fp32 **8-element (32 B)** |
| Fixpipe `nSize` | `RoundUp(originN, 8)` for fp32 RowMajor SPLIT_M |
| Fixpipe `mSize` | `RoundUp(originM, 2)` — even M for SPLIT_M |
| AIV GM copy N | **`blockN`**, not `alignN` |
| AIV GM copy M | **`thisM`**, not `RoundUp(blockM, 2)` |
| UB source | **not** GetTile’d; DataCopyPad `srcStride` uses full UB row stride (`alignN`) minus `thisN` |

There is **no** CATLASS reserved region, halo, or extra K pad on this path.

Declared UB tensor shape is `(blockM, alignN)`, not `(thisM, alignN)` and not `(CeilDiv(blockM,R), alignN)`.

Three different byte counts for a full 256×256 fp32 block (R left symbolic):

| Quantity | Formula | Bytes if R=2 |
| --- | --- | --- |
| Allocated stage | `UB_SIZE/2` | 126976 |
| UB tensor layout | `blockM × alignN × 4` | 262144 |
| AIV DataCopyPad | `thisM × blockN × 4` | 131072 |

CATLASS source **does not reconcile** these three numbers. dualDst (Q6) explains layout 256 KiB vs per-AIV copy 128 KiB. It does **not** explain 128 KiB vs the 124 KiB stage.

---

## Q5. What does `dualDstCtl = 1` mean **in CATLASS**?

Observed encoding in `copy_l0c_to_ub.hpp`:

| Mode | `dualDstCtl` set to | Extra |
| --- | --- | --- |
| `NO_SPLIT` (3-arg) | left at default **0** | one dst |
| `NO_SPLIT` (4-arg) | **0** | also sets `subBlockId` |
| `SPLIT_M` | **1** | `mSize` forced even |
| `SPLIT_N` | **2** | `nSize` rounded to 32 |

SPLIT_M Fixpipe signature: **one** destination `LocalTensor` + **one** source L0C. Unittest: one call. `subBlockId` is **not** set on the SPLIT_M path.

---

## Q6. When `dualDstCtl=1`, how is that one `dst` address interpreted on SUB BLOCK0 / SUB BLOCK1?

### What was searched in the install env

Container `cam_wangyibo_dev_85`, CANN **8.5.0** (`ASCEND_TOOLKIT_HOME=/usr/local/Ascend/cann-8.5.0`).

| Artifact | Result |
| --- | --- |
| `kernel_struct_fixpipe.h` | `FixpipeTrait::dualDstCtl` exists. **No** `FixpipeParamsArch3510` / `FixpipeParamsC310` body in this header. |
| `kernel_operator_fixpipe_intf_impl.h` | `__NPU_ARCH__==3101` → `#include "dav_c310/kernel_operator_fixpipe_impl.h"`; LocalTensor dst is `dst.GetPhyAddr()` as a single `__ubuf__` pointer. |
| `dav_c310/` / `fixpipe_arch3510*` | **Not shipped** in this 8.5.0 tree (`basic_api` only has `dav_c100/c220/m200/m300/m310`). |
| `copy_cube_out_utils.h` / `copy_cube_out_fixpipe.h` | Shipped. This is the installed C310/3510-aware Fixpipe **adaptor**, not the missing `dav_c310` instruction wrapper. |

CATLASS 950 uses `FixpipeParamsC310` and `__NPU_ARCH__==3510`. CANN 8.5 gates the same fields on `3101`/`5102`. Numbering differs; the adaptor below is still the installed dual-dst software.

### Sourced address semantics

**One physical dst pointer, used as the start of a compact half-tile in each sub-block’s own UB.** Hardware does not take a second LocalTensor. `subBlockId` selects a UB only in single-dst mode (`dualDstCtl=0`).

Evidence:

1. **Adaptor call** (`copy_cube_out_utils.h`): `Fixpipe<..., CFG_ROW_MAJOR_UB>(dst, colLocal, params_)` with `params_.dualDstCtl = mode`. No second dst, no `dst + M/2` in software.

2. **Software offsets are halved when dual-dst is on** (`copy_cube_out_fixpipe.h` `GetDstOffset`):

```text
L0C→UB and ibShare / DUAL_DST_SPLIT_M:
  dstOffset = (curRow * baseM * stride + curCol * baseN) >> 1
DUAL_DST_SPLIT_N:
  dstOffset = (full ND offset) >> 1
NZ dual-dst: dstOffset >>= 1
```

That only makes sense if each sub-block UB holds a **packed** half (`M/2 × N` or `M × N/2`), indexed from the **same** base the caller passed. If both halves lived in one UB at `base` and `base + halfBytes`, the adaptor would add `halfBytes` for SUB1, not right-shift every tile offset.

3. **Tensor-API Fixpipe doc** (asc-devkit copy in cannbot `Fixpipe.md`): with dual-dst enabled, the L0C `M×N` matrix is split in two and written to the two sub-block UBs at once; **first half → SUB BLOCK0, second half → SUB BLOCK1**. `DUAL_DST_SPLIT_M` requires even M; `DUAL_DST_SPLIT_N` requires N multiple of 32.

4. **CATLASS v1.6 encoding table** (`docs/zh/0x_new_versions/catlass_v1.6.md`), matching the hardware field:

| `dualDstCtrl` | Meaning |
| --- | --- |
| `2'b00` | single dst; whole matrix into the UB selected by `subBlockId` |
| `2'b01` | split on M; `M/2 * N` written to AIV; M even; no on-the-fly quant |
| `2'b10` | split on N; `M * N/2` written to AIV; N even in that table; no on-the-fly quant |
| `2'b11` | reserved |

`subBlockId`: “ID of the destination UB when single-destination mode is enabled.”

5. **CATLASS 46 consumer** agrees: AIV `DataCopyPad` starts at the UB tensor origin (no `halfM` row offset). GM offset is `(halfM * (subIdx & 1), 0)`. That is a compact local view, not a window into a shared 256-row buffer.

### What this does to the 256×256 fp32 numbers (if `GetTaskRation()==2`)

| View | Bytes | After Q6 |
| --- | --- | --- |
| CATLASS `MakeTensor` layout `(blockM, alignN)` | 262144 | Type-level full tile. **Not** what each AIV stores. |
| Per-AIV compact half `128 × 256 × 4` | 131072 | What dual-dst SPLIT_M deposits at `dst` in **that** sub-block UB. |
| Allocated ping-pong stage `UB_SIZE/2` | 126976 | Still **4096 B short** of 131072. |

`126976 = 124 × 256 × 4`. The leftover gap is **exactly four rows** of a 256-wide fp32 tile, not a fractal-sized dual-dst tax.

Do **not** claim hardware writes 124 rows. dualDst explains 256 KiB vs 128 KiB. It does not explain 128 KiB vs 124 KiB.

`GetTaskRation()` is still not assigned a constant in CATLASS. The 128-row arithmetic above is conditional on R=2.

---

## What this does / does not settle for the 46 Case

Settled:

- Distinctive path is L0C → one SPLIT_M Fixpipe (`dualDstCtl=1`) → AIV DataCopyPad of `(thisM, blockN)` from the start of a 124 KiB UB stage.
- 124 KiB = one of two stages on that core’s 248 KiB UB.
- CATLASS does not chunk the 256×256 writeback in software.
- `dualDstCtl=1`: one `dst` LocalTensor address is the **base of a compact M/2×N view in each sub-block UB**; SUB0 gets the first half, SUB1 the second; `subBlockId` is unused on this path.

Still **not** settled (still blocks a Case Card that needs a single occupancy diagram):

- Why the compact half (`131072` B if R=2 and 256×256 fp32) is larger than the allocated stage (`126976` B) by **4 KiB**.
- Runtime value of `GetTaskRation()`.
- The missing `dav_c310` / `fixpipe_arch3510` instruction body (not in CANN 8.5.0 on this machine).

Do not create `case.md` until the 4 KiB allocation vs payload mismatch is either sourced or the Case explicitly leaves it as unknown.
