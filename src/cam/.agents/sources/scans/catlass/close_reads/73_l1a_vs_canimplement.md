# Mini-read: 73 L1A allocation vs CanImplement

Status: **addendum to** `73_ascend950_matmul_full_loadA.md`. Not a Case Card by itself.

Question: does `MatmulFullLoadATla::CanImplement` cover the buffers the Block actually uses?

Evidence: `source-code` only. Live tiles: fp16, `L1_M=N=256`, `L1_K=128`, `L1B_STAGES=2`. `Arch::Ascend950::L1_SIZE = 512 KiB`.

---

## Three different numbers

| What | Formula | Live fp16, this example’s tiles |
| --- | --- | --- |
| README “一半 L1 放下 A” (`design-doc`) | `L1_M × K × sizeof(A) <= L1_SIZE/2` | `K <= 512` |
| Named A buffer | `L1A_TILE_SIZE = L1_SIZE / 2` | **256 KiB**, compile-time constant |
| `CanImplement` | `L1_M×K×sizeof(A) + L1_K×L1_N×L1B_STAGES×sizeof(B) <= L1_SIZE` | `K <= 768` |
| Physical copy | B at L1 offset 0; A at `L1B_STAGES * L1B_TILE_SIZE`; size `L1_M × K × sizeof(A)` | B occupies **128 KiB**; A may start at 128 KiB and run toward L1 end |

`kActual` from both Identity and FullLoad swizzles is `problemShape.k()` (`source-code` `GetActualBlockShape`). Kernel does not slice K. Block copies that full K into L1A.

---

## What CanImplement does **not** check

1. **Not** `A_bytes <= L1A_TILE_SIZE`. Named half-L1 is not in the predicate.
2. **Not** the README half-L1 sentence. `CanImplement` allows A up to `L1_SIZE - B_pingpong` (384 KiB here), which is **larger** than half L1.
3. Block `static_assert` on L1 for the **no-bias** live path: **none**. The `L1A_TILE_SIZE + L1B*stages <= L1` assert exists only inside `if constexpr (HAS_BIAS)`.
4. `GetBufferByByte<ElementA>(l1AOffset)` does **not** pass `L1A_TILE_SIZE` as a length. `L1A_TILE_SIZE` is not a copy bound.

What `CanImplement` **does** match: total bytes `A_copy + B_pingpong <= L1_SIZE`, which is the physical packing “B then A in one L1,” provided A does not need to stay inside the named 256 KiB window.

---

## Worked bounds (this example’s tiles, fp16)

```text
B_pingpong = 256 × 128 × 2 × 2 = 128 KiB
A_copy     = 256 × K × 2

CanImplement true  iff  A_copy + 128 KiB <= 512 KiB  iff  K <= 768
README half-L1     iff  A_copy <= 256 KiB            iff  K <= 512
Named L1A window   = 256 KiB  (not applied to the copy)
```

Shipped CLI / optest `K=256` → `A_copy = 128 KiB`. Both caps pass. **This shape does not demonstrate the gap.** The gap is in the source formulas, not in the golden compare.

A shape with `512 < K <= 768` (fp16, these tiles): `CanImplement` returns true; A copy **exceeds** `L1A_TILE_SIZE` and the README half-L1 story. Whether that run is safe depends on leftover L1 after B (384 KiB here), not on the named half. Do not upgrade that to “it works in silicon”; it is a **predicate mismatch**, recorded not resolved.

---

## Agent rule

`CanImplement` 是 **partial** guard: one arithmetic inequality on A+B vs `L1_SIZE`. It is not a proof that every named buffer (`L1A_TILE_SIZE`), every README sentence (“一半 L1”), or every `static_assert` has been checked.

The Host cout `MatA tile cannot be full loaded to L1.` is **not** that inequality. Do not treat the error string as the predicate.

Do not treat a non-trivial `CanImplement` as “all on-chip constraints are encoded.”

---

## Sources

| File | What it shows |
| --- | --- |
| `include/catlass/gemm/kernel/matmul_full_loadA_tla.hpp` | `CanImplement` sum |
| `include/catlass/gemm/block/block_mmad_pingpong_full_loadA_ascend950_tla.hpp` | `L1A_TILE_SIZE = L1/2`; B at 0; A at `l1AOffset`; copy `(L1_M, actualShape.k())`; no-bias path has no L1 `static_assert` |
| `include/catlass/arch/arch.hpp` | `Ascend950::L1_SIZE = 512 KiB` |
| `examples/73_ascend950_matmul_full_loadA/README.md` | half-L1 design-doc |
