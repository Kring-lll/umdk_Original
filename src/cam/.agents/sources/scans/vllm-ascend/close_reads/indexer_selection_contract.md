# P1 close-read: indexer selection contract

Scout 之后最关键的一份。**Not a Case. Not Knowledge.**

```yaml
status: close-read-written
review: passed
```

Cross-source map only established:

```text
producer → sparse_indices / topk_indices → consumer
```

It did **not** prove the coordinate equals Sparse Case `select_idx`.

Evidence: same checkout as [indexer_a5_runtime_paths.md](indexer_a5_runtime_paths.md). Kernel cites are **contract pins**, not a full indexer walkthrough.

Existing Sparse Case (do not reopen consumer teaching):

```text
select_idx logical block
        ↓
block_table
        ↓
physical KV page
        ↓
Sparse FA
```

`select_idx` is already a **logical page id**. Indexer is outside that card.

---

## Producer input coordinate

Pinned from `npu_vllm_quant_lightning_indexer` README + DSA/SFA call sites. SFA `torch_npu.npu_lightning_indexer*` uses the same tensor names without `cmp_ratio` / `metadata`.

```text
Q_index:
  query = Q_index^Quant     TND: [T, N_index, D]
  query_dequant_scale       per-token-head (quant_mode=0)
  D = indexer head dim (SFA: 128)

K_index:
  key = K_index^Quant       PA_BSND: [block_num, block_size, N_k, D]
  key_dequant_scale
  README: “压缩后的输入 Index Key”
  DSA Path 2: cmp_ratio=4 → S_k is the compressed indexer-K sequence
  SFA Path 1: no cmp_ratio on the torch_npu call

block_table:
  [B, max_blocks] int32
  used **inside the indexer** to **read** paged K_index
  not applied to the output tensor

other metadata:
  actual_seq_lengths_query / key
  sparse_mode=3  (right-down causal)
  weights W
  Path 2: qli_metadata (aicpu split), pre/next_tokens = 2^63-1
```

Indexer `block_table[batch, logical]` → physical page of **indexer K cache**.  
That is producer **storage addressing**, not “output is already a physical page”.

---

## Selection math

README formula (do not re-derive the whole kernel):

```text
score:
  (Scale_Q @ Scale_K^T) ⊙ (Q_index^Quant @ (K_index^Quant)^T)
  then ReLU          # drop negative correlation
  then ⊙ (W @ 1_{S_k})
  then reduce over indexer heads g

filter:
  ReLU + sparse_mode=3 causal mask on K sequence

top-k:
  Top-k over that S_k score vector
  keep sparse_count slots
```

SFA Path 1: `sparse_count=2048`.  
DSA Path 2: `sparse_count=config.index_topk`, `cmp_ratio=4`.

Padding of unused slots: kernel `INVALID_IDX = -1` (`quant_lightning_indexer_common.h`). Host README: valid values in the front of the row, invalid in the tail.

---

## Producer output

```text
sparse_indices.shape:
  TND: [T, N2, sparse_count]     # N2 follows query’s index-head / KV-N packing
  last dim == attr sparse_count
  dtype int32

index coordinate:
  **S_k token index** in the key sequence the indexer scored
  README: “参与稀疏attention计算的token索引值”
  not logical page id
  not physical page id
  Path 2 with cmp_ratio=4: S_k is the **compressed** indexer-K sequence,
  so the integer is a token (or compressed-token) in that shorter axis

value semantics:
  0-based position along K sequence
  -1 = unused / padding

ordering:
  Top-k slots; host requires valid prefix, invalid suffix
  not proven sorted by score vs by position this round (not needed for Case gate)

invalid/padding semantics:
  -1
  SFA host mask: (topk_indices >= 0) & (topk_indices < seq_len)
    vllm_ascend/attention/utils.py::build_valid_topk_mask

sparse_count lifetime:
  chosen at the indexer call (2048 vs index_topk)
  then copied into topk_indices_buffer when IndexCache / skip_topk
  consumer last-dim is that same K
  not a per-token select_num_idx like SASA (SFA 950 does not pass select_num_idx)
```

README wording collision (do not silently pick one): `sparse_count` is documented as “保留的 **block** 数量”, while `sparse_indices` values are documented as **token** indices. Kernel + 950 SFA consumer (`sparse_block_size=1`) treat the **values** as S2 token ids. The last dimension is a slot count, not a page-size.

---

## Consumer interpretation

### SFA (`npu_sparse_flash_attention` / `npu_kv_quant_sparse_flash_attention`)

Call kwargs: `sparse_indices=topk_indices`, `sparse_block_size=1`, `block_table=attn_metadata.block_table`.

950 constraint: `sparse_block_size` **only 1** = Token-wise sparsity (SFA README). Block-wise (`>1`) is A2/A3, not this A5 row.

```text
sparse_indices[..., j]
        ↓ represents what?
logical KV block?     no
physical page?        no
token?                yes — S2 token index in the KV sequence
cache slot?           no — not a pre-resolved physical slot
```

Inside the consumer (`sparse_flash_attention_service_vector_mla.h`):

```text
tokenIdx = sparseIndices[j] + s2StartIdx     # -1 stays skip
        ↓
GetkeyOffset(tokenIdx):
  blkTableIdx    = tokenIdx / page_block_size
  blkTableOffset = tokenIdx % page_block_size
  physical       = block_table[batch, blkTableIdx] * page_block_size
                   + blkTableOffset
```

So:

```text
indexer writes token
        ↓
SFA consumer divides by page block_size
        ↓
block_table
        ↓
physical KV element
```

That is **not** Sparse Case’s `select_idx` (already logical page). Same `block_table` tensor appears on both sides, but it answers a different question.

### DSA (`npu_kv_quant_sparse_attn_sharedkv`) — contrast only

```text
cmp_sparse_indices[..., j]
        ↓
S2 index in **cmp_kv** sequence (+ s2StartIdx)
        ↓
GetkeyOffset via cmp_block_table   # same token / page_block_size split

ori_sparse_indices  (optional, not Path 2 indexer output):
  comment: absolute physical slot ids, host already applied block_table
  do not treat as lightning-indexer contract
```

Host README says K2 is “一次离散选取的 **block** 数”. Kernel still loads `cmpSparseIndices` as S2 indices. Same wording collision as the indexer README. Coordinate pin follows the kernel: **token-in-cmp-sequence**, then PA inside the consumer.

---

## Where `block_table` translation happens

```text
before indexer?   no as a rewrite of the map
                  (framework may DCP-remap tokens after indexer; that is not PA)

inside indexer?   yes — to **read K_index** from PA_BSND
                  output remains an S_k index, not a physical page

inside consumer?  yes — token → logical page → physical KV
                  SFA and DSA cmp path both do this in GetkeyOffset
```

Sparse Case does the logical→physical step **before** Cube, from `select_idx` that is already a page id.  
vLLM 950 SFA does it **from a token id**, because sparsity grain is one token (`sparse_block_size=1`).

---

## Coordinate identity vs Sparse Case

| | Sparse Case `select_idx` | vLLM indexer `sparse_indices` |
| --- | --- | --- |
| unit | logical **page** | **token** (S_k / cmp S_k) |
| `-1` | unused page slot | unused token slot |
| `block_table` | applied to the selection | applied to indexer-K **load**; applied again in consumer to selected **tokens** |
| 950 sparsity grain | page (blockSize 128 in the live instance) | token (`sparse_block_size=1`) |

**Not the same selection/storage contract.**  
The missing teaching is Indexer → token map. The existing card is token-map-already-assumed-as-pages → Sparse FA.

DCP remap (`sfa_cp._remap_sparse_indices`) is extra evidence that the producer emits **token coordinates**, not page ids (`floor(token / interleave_size)` would be nonsense if the values were already logical pages of size 128 unless interleave were also 128 — the comment says “global token coordinates”).
