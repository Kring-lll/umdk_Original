# Verdict: indexer Case gate + Sparse consumer

Scout + P1 close-reads. **Not a Case. Not Knowledge.**

```yaml
status: verdict-written
review: passed
indexer_case: yes-candidate
mechanism_gate: passed
canonical_instance_gate: pending
sparse_consumer_case: no
sparse_consumer_review: passed
merge_indexer_and_sparse: no
knowledge: none
p1_next: compress-epilog paused
```

Evidence:

- [indexer_a5_runtime_paths.md](indexer_a5_runtime_paths.md) — review passed
- [indexer_selection_contract.md](indexer_selection_contract.md) — review passed
- [indexer_c_ascend_a5_registration.md](indexer_c_ascend_a5_registration.md) — registration chain; live `.so` load still pending
- live Sparse Case: `cases/compute/attention/ascend950-block-sparse-paged-attention/` (consumer teaching; do not edit)

---

## Indexer — new mechanism?

**Yes.** `mechanism_gate: passed`. Do **not** write the Case yet (`canonical_instance_gate: pending`).

Proven (do not reopen):

1. **Production is inside the indexer.** Sparse Case leaves `select_idx` as an external contract. vLLM scores `Q_index @ K_index`, ReLU-filters, weighted Top-k, writes `sparse_indices`.
2. **Output coordinate is not Sparse `select_idx`.** Values are S_k **token** indices (`-1` pad). 950 SFA consumer is token-wise (`sparse_block_size=1`) and runs `block_table` *after* that token. Existing card is logical-**page** → `block_table` → physical page.
3. **`block_table` in the indexer is K-cache addressing, not the taught selection map.**

```text
existing Sparse Case                 vLLM indexer P1
logical page id                      Q_index / K_index
        ↓ block_table                      ↓ score + filter + Top-k
physical KV page                     S_k token index
        ↓                                    ↓ consumer: token / page_block_size
Sparse FA                                    ↓ block_table
                                       physical KV element
```

New teaching is **selection production + token-coordinate contract**, not “another Sparse kernel that accepts `select_idx`”.

| | existing Sparse Case | vLLM indexer |
| --- | --- | --- |
| selection unit | logical page | token / compressed token |
| `-1` | unused page slot | unused token slot |
| `block_table` | selection → physical page | producer reads K-cache; consumer token → physical |
| 950 sparsity grain | page | token |

---

## Canonical instance — still the only blocker

```text
                  runtime live     mechanism source
SFA torch_npu          ✅                ?
DSA _C_ascend          ?                 ✅
```

No row is two ✅ yet. Do not graft in-tree kernel math onto a `torch_npu` Python call. Do not declare DSA canonical while `_C_ascend` load on A5 is unobserved.

Case write when **either** closes (not both):

```text
A. SFA path
   Prove torch_npu.npu_lightning_indexer* public/source semantics
   cover this Case’s selection mechanism.

or

B. DSA path   (preferred)
   Prove _C_ascend.npu_vllm_quant_lightning_indexer
   is registered/executable on the target A5 runtime.
```

Prefer B: mechanism source is already in-tree. Closing B is a **registration/load** question, not a new kernel read.

If B lands:

```text
Canonical:
  DeepSeek-V4 / DSA
    → _C_ascend.npu_vllm_quant_lightning_indexer
    → in-tree Ascend950 implementation

Sibling:
  SFA
    → torch_npu.npu_lightning_indexer*
    → same selection role
    → implementation identity not assumed
```

If B is proven **dead** on that A5, take A and gather SFA’s own mechanism evidence. Do not hunt a global “A5 indexer”.

Registration close-read already closed:

- `enable_custom_op()==False` is **not** proof Path 2 is dead.
- `_C_ascend` comes from loading `vllm_ascend_C`, not from vendor OPP.
- A5 serving **attempts** that load via `camem.py` (worker import), not via `enable_custom_op`.
- Remaining: observe the import on the target A5 process.

---

## Sparse consumer — second Case?

**No.** `sparse_consumer_review: passed`. Stop reading consumers.

| consumer | vs Sparse Case | action |
| --- | --- | --- |
| SFA `npu_*sparse_flash_attention` | token → logical page + offset → `block_table` → physical; QK/softmax/PV not a new Case | contrast only |
| DSA `npu_kv_quant_sparse_attn_sharedkv` | SWA + compressed sparse | contrast-pending; still no second Sparse Case |
| MiniMax `npu_sparse_attention_score` | SASA-shaped; A3 ACLNN | filtered |

`new consumer Case = no`. Do **not** merge indexer + Sparse.

---

## Paused

```text
P1 indexer Case write          ⏸ canonical-instance not pursued
P1-next compress epilog        ⏸
P3 custom-op surfaces          🔜 after P2 accepted
Knowledge                      ❌ now
```

---

## Status snapshot

```text
P1 runtime path matrix             ✅ PASSED
P1 selection contract              ✅ PASSED
P1 Sparse consumer verdict         ✅ PASSED

Indexer new-mechanism gate         ✅ PASSED
Indexer canonical-instance gate    ⏸ STOPPED
Indexer Case                       ⏸

Sparse second Case                 ❌ NO
Merge producer + consumer          ❌ NO
Knowledge                          ❌ NOW
Compress epilog                    ⏸
P2 MoE / MC2                       ✅ PASSED / FREEZE
P3                                 ✅ close-read；Knowledge 🔜
```
