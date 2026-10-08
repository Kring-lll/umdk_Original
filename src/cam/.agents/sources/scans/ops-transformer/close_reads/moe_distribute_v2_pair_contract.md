# DispatchV2 ↔ CombineV2 pair contract

Status: **Case B** evidence. `cases/compute-communication/moe/ascend950-moe-ep-dispatch-combine/`.

Short file. Kernel detail lives in the two op close-reads. This table is the SoT for **which tensor is reused vs inverted vs renamed**.

```yaml
source_identity:
  registry_ref: ops-transformer
  scanned_commit: 9311b63bcd065b9779eb08a7d634e9463ee41ef1
  paths:
    - mc2/moe_distribute_dispatch_v2/
    - mc2/moe_distribute_combine_v2/
```

README constraint: they **must** be used as a pair (A2: CombineV2; A3/950: CombineV2 or CombineAddRmsNorm).

Communication: same EP group / window resource. Dispatch writes dest windows then status; combine writes **back** to src windows then status. Comment in dispatch Init: **combine uses the front half of the window, dispatch the rear**, so a fast rank cannot clobber in-flight dispatch data (`design-doc` §2.4.3).

---

## Rank layout

| stage | payload axis |
| --- | --- |
| dispatch in `x` | token-major `[BS, H]` |
| dispatch window send | `(dst_rank, local_e, slot)` packed |
| dispatch out `expand_x` | local-expert-major `[A, H]` (src-rank concatenated inside expert) |
| combine in `expand_x` | **same axis as dispatch out** (after GMM: same rows, new values) |
| combine window send | `(src_token, k)` slots in the **origin** rank window |
| combine out `x` | token-major `[BS, H]` |

Dispatch packing is **not** inverted by replaying init `expanded_row_idx`. Combine recovers origin `(src_rank, tokenId, topkId)` from **`assist_info_for_combine`** (dispatch `FillTriple`). **`expert_ids`** is reused for expert/routing identity, not as the reverse slot map.

---

## Producer / consumer

| object | producer | consumer | reuse / invert / rename |
| --- | --- | --- | --- |
| `x` hidden `[BS,H]` | residual (outside) | **dispatch in** | combine **out** is a new token-major tensor, not this buffer |
| `expert_ids [BS,K]` | gating / SoftmaxV2 | **both** in | **reused**, not rewritten |
| `expert_scales [BS,K]` | gating `y` **if fp32**; else converted / sibling | dispatch optional (`expand_scales` path); **combine required fp32** | **conditional reuse** (dtype) |
| `scales` (quant smooth) | caller | dispatch only | not combine |
| window data | dispatch MTE | peer combine wait | invert direction |
| window status / flags | dispatch `SetStatus` | dispatch `WaitDispatch`; later combine packed-token flags | protocol pair; **do not share one flag word across ops without the half-buffer split** |
| `expand_x` | dispatch | GMM in; after GMM, **combine in** | **layout reused; values replaced by expert FFN** |
| `dynamic_scales` | dispatch | GMM `x_scale` (quant) | not a combine input |
| `assist_info_for_combine` `[A*128]` | dispatch | **combine required** | **reused** (V2 map; not init row idx) |
| `expert_token_nums [localE]` | dispatch (post-recv) | **GMM `group_list` candidate**; **not** combine (`group_list` reserved) | not inverted |
| `ep_recv_count` | dispatch | combine **`ep_send_counts`** | **renamed, same buffer** |
| `expand_scales [A]` | dispatch | combine optional | reused |
| `tp_*` | reserved | reserved | dead |
| init `expanded_row_idx` | init-routing | **neither** EP op | **not in this pair** |
| init counts | init-routing | **neither** EP op | **not in this pair** |

---

## Reverse contract?

| dispatch does | combine does |
| --- | --- |
| `(token,k) → (dst_rank, local_e, slot)` + FillTriple in payload | assist triple → origin window slot `(tokenId, topkId)` → token-major weighted sum |
| optional quant on send | optional `comm_quant_mode` on reverse; main README path is float expert out |
| emits post-EP local counts | does **not** emit groupList; does emit token-major `x` |

It is a **rank-shuffle inverse + weighted reduce**, not an inverse of InitRouting.

---

## What this pair kills / keeps for the chain

Keeps:

- `expert_ids` (expert identity) across the whole EP+GMM span.
- gate weights only if they are **fp32** (or converted) into combine `expert_scales`.
- `assist_info` as the reverse **(src_rank, token, k)** map; `ep_recv_count` as occupancy.

Kills:

- “`init.expanded_row_idx` survives EP+GMM into Finalize” on this production pair.
- “`init.counts` is what GMM reads” — GMM’s candidate is **dispatch `expert_token_nums`** (still attr-aligned, still no joint test).
