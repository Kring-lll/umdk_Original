# P1-next close-read: A5 compress epilog (quant + paged-cache write)

P3 Knowledge **accepted / FREEZE**. This round is **storage materialization**, not Indexer selection.

**Not a Case. Not Knowledge. Not the whole DSA KV-cache pipeline.**  
Do **not** fold this into the paused Indexer Case. Do **not** open `compressor` / `store_kv_block`.

Source: `vllm-project/vllm-ascend` `main` @ `d8268faea90dd9b999a0a82f6d133751d57b5770`.

```yaml
status: close-read-written
review: passed
scope: compress-epilog-only
freeze: source-4
not: indexer-case
not: sparse-consumer-case
not: compressor-algorithm
not: store_kv_block
not: kv-cache-subsystem
```

Companion: [compress_epilog_verdict.md](compress_epilog_verdict.md).

P3 reminder: `_C_ascend` + `EXEC_NPU_CMD(aclnn…)` is **bound / dispatched**. A5 runtime composition / `.so` load remains **unresolved**. This file judges **mechanism from source**, not live A5.

---

## 先问什么

```text
1. 它 materialize 什么？（hidden / K_index / KV → quant? scatter? paged cache?）
2. 写入坐标是什么？block_table 在哪一层？
3. 相对非 A5 split path，融合删了哪些 API handoff？
4. 现有 FA paged-addressing / Indexer / 53·C 能不能吸收？
```

不要先问“这是不是 Indexer 的一部分”。也不要先问“A5 serving 是否跑了这份 kernel”。

```text
Indexer            = 谁被选中？（selection production）
Compress epilog    = 数据怎么量化并写进后续要读的 cache？（storage materialization）
```

---

## Two jobs, two A5 calls

| job | A5 Python call | writes |
| --- | --- | --- |
| Indexer K_index cache | `A5DeviceAdaptor.indexer_quant_scatter_part1` → `_C_ascend.indexer_compress_epilog_v2` | packed `indexer_full_cache` (fp8 values + scales) |
| Attention KV cache (SWA / compressed-KV) | `A5DeviceAdaptor.dsa_kv_compress_scatter` → `_C_ascend.kv_compress_epilog` | fp8 `kv_compress_cache` |

Both are on the A5 `CUSTOM_OPS_ARRAY` (`build_aclnn.sh`). Binding hops:

```text
_C_ascend.indexer_compress_epilog_v2
    → EXEC_NPU_CMD(aclnnIndexerCompressEpilogV2)

_C_ascend.kv_compress_epilog
    → EXEC_NPU_CMD(aclnnKvCompressEpilog)
```

composition-capable ≠ runtime composition observed.

Query quant for lightning indexer is **separate**: A5 `indexer_quantize_query` = `torch_npu.npu_dynamic_quant` (fp8). That is **not** this epilog.

---

## Q1 — What is materialized?

Neither op writes raw hidden. Both take a **token-row tensor `x`** already produced upstream, quantize it, and scatter into a paged cache. Skip `slot == -1`.

### `indexer_compress_epilog_v2`

```text
K_index row  (fp16/bf16 `x`)
    ↓ per-row group FP8-e4m3  (VFProcessDynamicBlockQuant, group 128)
    ↓ scatter value + scatter scale into ONE packed page
indexer_full_cache  (uint8 view)
```

A5 adaptor `layout=2`. Kernel:

```text
blkNumIdx  = slot / cacheBs
blkSizeIdx = slot % cacheBs
valueOffset = blkNumIdx * blockStride + blkSizeIdx * d
scaleOffset = blkNumIdx * blockStride + cacheBs * d + blkSizeIdx * scaleCol
```

Part3 `dsa_indexer_scatter_scale_part3` is a **no-op** on A5 (`kv_scale is None`). Scale lives in the same packed cache.

This is **K_index storage** for a later indexer **read**. It does not produce `select_idx` / top-k.

### `kv_compress_epilog`

```text
KV row  (bf16 `x` — SWA `kv` after rope, or compressor output `compressed_kv`)
    ↓ A5: quant_mode=2 MXFP8, quant_group_size=64, layout=1
    ↓ scatter into kv_compress_cache (fp8 e5m2 / e4m3)
paged KV / compressed-KV cache
```

A5 comment: input is **unquantized bf16**; the fused op quantizes internally. Non-A5 `dsa_kv_compress_scatter` only `npu_scatter_nd_update_v2` (expects a tensor already in cache dtype).

layout=1 write:

```text
gmOffset = slot * rowStride     # physical slot already
```

layout=2 (kernel can do packed value+scale; **A5 serving call uses layout=1**) is not the A5 adaptor path.

`compressor()` that **produces** `compressed_kv` is a different op. Out of scope.

---

## Q2 — Write coordinate; where is `block_table`?

These kernels take **`slot_mapping`**, not `block_table`.

Host already did FA-style logical→physical:

```text
token position
    → logical block + in-block offset
    → block_table[logical] = physical page
    → slot = physical_page * block_size + offset    # -1 = pad
    → epilog kernel
```

Pinned in `worker/block_table.py` (`block_numbers * block_size + block_offsets`). A5 adaptor keeps slot **1D flat**; Base formats `[block_idx, offset]` for `npu_scatter_nd_update_v2`. Same physical address, different scatter API.

```text
block_table          Host / BlockTable  （FA Knowledge logical→physical）
slot_mapping         kernel input
kernel               physical store; skip -1
```

Indexer layout=2 `slot / cacheBs` is **decoding a packed physical slot**, not looking up `block_table`.

Do not call this Sparse `select_idx` addressing. `select_idx` is a **read-side** logical page list for FA. This is **write-side** paged slot.

---

## Q3 — Which API handoffs did fusion delete?

| path | Non-A5 (Base DeviceOperator) | A5 fused |
| --- | --- | --- |
| Indexer K | `npu_dynamic_quant` → `kv_out` + `kv_scale` → `scatter_nd` k_cache → `scatter_nd` scale_cache | one `indexer_compress_epilog_v2` into `indexer_full_cache`; Part3 gone |
| Indexer Q | int8 `npu_dynamic_quant` (still split) | fp8 `npu_dynamic_quant` (still split; **not fused into epilog**) |
| KV scatter | `npu_scatter_nd_update_v2` of already-quantized `x` | `kv_compress_epilog` quant + scatter |

Deleted **intermediate tensors / second scatter**, not a new producer/consumer protocol. No MTE window, no chunk ownership transfer, no new visibility gate. Pad skip (`-1`) is the same scatter contract as `reshape_and_cache` / `scatter_nd`.

Stream overlap in `indexer.py` Part1 (aux scatter ∥ Cube matmul) is **framework scheduling of existing ops**, not a kernel-family mechanism.

---

## Q4 — Absorb?

| asset | relation |
| --- | --- |
| FA Knowledge `block_table` logical→physical | **Host already did this.** Kernel consumes the resulting `slot_mapping`. Not a new addressing family. |
| P1 indexer | **Different job.** Indexer = who is selected. Epilog = how K_index/KV are quantized and stored. Do not bind. |
| 53 / C | MX / group-scale **pattern** on a Vector cache write. **Not** Cube `MmadMx` / GMM+SwiGLU. Do not clone 53 or C. |
| Sparse Case | Consumer-side `select_idx` → paged FA. Epilog is write-side. Do not open a second Sparse. |

FA Knowledge 吸收 **paged-addressing** 部分（Host `block_table` → physical `slot_mapping`）。quant + physical scatter 是无需单独资产化的融合包装，不是 FA pipeline 全文。

What would have been a Case: a new **ownership / scheduling** of “quantize + indexed paged-cache materialization” as a distinct protocol. Source shows fused **packaging** of quant + physical scatter.

---

## What this file does not do

- Close P1 indexer canonical `.so` probe.
- Walk `compressor` / `store_kv_block` / full DSA cache tuple.
- Claim A5 serving **observed** these kernels.
- Write Case or Knowledge.
