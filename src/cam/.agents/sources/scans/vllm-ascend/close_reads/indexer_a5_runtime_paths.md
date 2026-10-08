# P1 close-read: A5 indexer runtime path matrix

Scout 之后。**Not a Case. Not Knowledge.**  
不回答“A5 到底用哪一个 indexer”。SFA 与 DSA / DeepSeek-V4 已经是两条路径。

证据源：`vllm-project/vllm-ascend` `main` @ `d8268faea90dd9b999a0a82f6d133751d57b5770`。

```yaml
status: close-read-written
review: passed
scope: path-matrix-only
not: one-global-indexer
not: compress-epilog
not: MiniMax-as-producer
```

Companion:

- [indexer_selection_contract.md](indexer_selection_contract.md)
- [indexer_sparse_consumer_verdict.md](indexer_sparse_consumer_verdict.md)

---

## How to read this file

Each row is one **runtime path**, not one chip-wide default.

```text
model / backend
     ↓
call site
     ↓
DeviceOperator / direct call
     ↓
torch_npu | _C_ascend
     ↓
gate?   (enable_custom_op A5=False)
     ↓
actual implementation   # unresolved this round; P3
```

Keep three layers distinct (Scout P3 SHOULD-FIX; do not auto-identify):

```text
Build / delivery surface     ≠
Python call surface          ≠
runtime selected implementation
```

This file pins the **Python call surface** and who sits above it.  
It does **not** prove that `torch_npu.npu_lightning_indexer` and `csrc/.../vllm_quant_lightning_indexer` are the same kernel.

`enable_custom_op()` (`vllm_ascend/utils.py`): A5 → `False` → skip `import vllm_ascend.vllm_ascend_C`. That is **this function’s** load gate, not “`_C_ascend` never registers on A5”. Serving still attempts the `.so` via `camem.py`. See [indexer_c_ascend_a5_registration.md](indexer_c_ascend_a5_registration.md).

---

## Path 1 — SFA (A5 adaptor)

```text
SFA  (AscendSFAImpl)
     ↓
sfa_v1.indexer_select_post_process
     # builds Q_index / weights / optional C8 quant
     ↓
DeviceOperator.indexer_select_post_process
     # A5DeviceAdaptor overrides Base
     ↓
torch_npu.npu_quant_lightning_indexer     if enable_sparse_li_c8 and q_li_scale
torch_npu.npu_lightning_indexer           otherwise
     ↓
gate?  does not sit on this Python call
       (no torch.ops._C_ascend.*)
     ↓
actual impl: vendor / torch_npu   # not pinned to csrc/
```

Pinned call:

- File: `vllm_ascend/device/device_op.py` `A5DeviceAdaptor.indexer_select_post_process`
- `layout_query="TND"`, `layout_key="PA_BSND"`
- `sparse_count=2048`, `sparse_mode=3`
- `block_table=attn_metadata.block_table`

**Do not mix with Path 2.** A5 SFA does **not** call `npu_vllm_quant_lightning_indexer`.

Base adaptor (A2/A3, contrast only) still has `_C_ascend.npu_lightning_indexer_quant` / `_C_ascend.npu_lightning_indexer` plus a `use_torch_npu_lightning_indexer` branch. That is **not** the A5 SFA row.

`AscendSFAIndexerBackend` (`vllm_ascend/attention/indexer.py`) is cache-only. It does not produce `topk_indices`. Skip-topk layers read `topk_indices_buffer`; they are not a third producer.

SFA consumer (same backend, not a second indexer):

```text
sfa_v1._execute_sparse_flash_attention_process
     ↓
DeviceOperator.execute_sparse_flash_attention_process   # not overridden on A5
     ↓
if KV is int8 / fp8:
    A5._execute_kv_quant_sparse_flash_attention
         → torch_npu.npu_kv_quant_sparse_flash_attention
         sparse_indices=topk_indices
         sparse_block_size=1
else:
    Base path
         → torch.ops._C_ascend.npu_sparse_flash_attention
         same kwargs
         gate?  this Python call is _C_ascend
```

C8 SFA on A5 can stay on `torch_npu` for **both** producer and consumer. BF16 SFA consumer still names `_C_ascend`. Do not collapse those two consumer rows.

DCP (`sfa_cp.py`): after producer, `_remap_sparse_indices` treats values as **per-request global token coordinates** and remaps into the local KV view. That is a post-producer transform, not a different indexer.

---

## Path 2 — DeepSeek-V4 / DSA

```text
DeepSeek-V4 IndexerOps.select_topk
     and/or
DSA CP  _indexer_select_topk
     ↓
direct torch.ops._C_ascend.npu_vllm_quant_lightning_indexer
     # not DeviceOperator.indexer_select_post_process
     ↓
Python call: _C_ascend
     ↓
enable_custom_op() does not load this
serving attempts vllm_ascend_C via camem.py (worker import)
.op present after that import: pending A5 probe
     ↓
actual impl if loaded: TORCH_LIBRARY → aclnnVllmQuantLightningIndexer
             (in-tree csrc; vendor OPP is ACLNN search path, not this Python name)
```

Pinned kwargs (both call sites agree):

- `layout_query="TND"`, `layout_key="PA_BSND"`
- `sparse_mode=3`, `cmp_ratio=4`, `return_value=False`
- `sparse_count=self.index_topk` (model config, not SFA’s hardcoded 2048)
- `block_table` + `metadata=qli_metadata`

Files:

- `vllm_ascend/models/deepseek_v4/indexer.py` `select_topk`
- `vllm_ascend/attention/context_parallel/dsa_cp.py` `_indexer_select_topk`

DSA A5 consumer (contrast only; not an indexer):

```text
DeviceOperator.get_dsa_sparse_attn_op
     A5 → torch.ops._C_ascend.npu_kv_quant_sparse_attn_sharedkv
     ↓
cmp_sparse_indices = compress_topk_idxs   # Path 2 output
ori_kv / cmp_kv + ori_block_table / cmp_block_table
```

Compress epilog (`indexer_compress_epilog_v2` / `kv_compress_epilog`) sits **before** this indexer as KV-storage write. P1-next. Do not fold it into the path matrix as a fourth producer.

---

## Path 3 — MiniMax: consumer evidence only

Do **not** expand MiniMax into an A5 indexer producer path.

```text
MiniMax-M3
     ↓
msa_m3_npu._minimax_m3_sparse_attn_a3
     ↓
torch.ops._C_ascend.npu_sparse_attention_score
     topk_idx, block_table, select_num_idx
     ↓
this is Sparse-Case-shaped **consumer** wiring
     ↓
A5 ACLNN list does not include sparse_attention_score
     producer of topk_idx is MiniMax Triton / host, not lightning indexer
```

If MiniMax later needs a producer close-read, that is a different object. Not this P1.

---

## Matrix (A5)

| backend | call site | adaptor / direct | Python call | `sparse_count` | `cmp_ratio` | `enable_custom_op` on this call | actual impl this round |
| --- | --- | --- | --- | --- | --- | --- | --- |
| SFA, LI C8 | `sfa_v1` → `A5DeviceAdaptor.indexer_select_post_process` | adaptor | `torch_npu.npu_quant_lightning_indexer` | 2048 | (not passed) | no | unresolved (vendor) |
| SFA, non-C8 LI | same | adaptor | `torch_npu.npu_lightning_indexer` | 2048 | (not passed) | no | unresolved (vendor) |
| DeepSeek-V4 | `IndexerOps.select_topk` | direct | `_C_ascend.npu_vllm_quant_lightning_indexer` | `config.index_topk` | 4 | no (camem may still load `.so`) | TORCH_LIBRARY → aclnn if `.so` loaded |
| DSA CP | `_indexer_select_topk` | direct | same `_C_ascend` op | `self.index_topk` | 4 | no (same) | same |
| MiniMax | `npu_sparse_attention_score` | — | `_C_ascend` **consumer** | — | — | n/a for indexer | not an indexer path |

README trap: `vllm_quant_lightning_indexer` documents 950 `sparse_count` = **512**. SFA A5 still passes **2048** into `torch_npu.npu_lightning_indexer*`. Do not “correct” one number onto the other op.

---

## What this file does not conclude

- A5 default serving uses Path 1 or Path 2.
- `torch_npu.npu_lightning_indexer` ≡ `npu_vllm_quant_lightning_indexer`.
- `vllm_ascend_C` actually imports on the target A5 process (see registration close-read).
- MiniMax `topk_idx` is produced by lightning indexer.
