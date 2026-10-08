# Close-read: MoeDistributeCombineV2

Status: **Case B** `cases/compute-communication/moe/ascend950-moe-ep-dispatch-combine/`. This file remains the evidence dump.

Pair with dispatch. README: “按 DispatchV2 收集数据的路径**原路返还**”. Output is **token-major** `x [BS, H]`, already gate-weighted. This is **not** “restore init expanded-slot order for Finalize”.

```yaml
source_identity:
  registry_ref: ops-transformer
  scanned_commit: 9311b63bcd065b9779eb08a7d634e9463ee41ef1
  scanned_branch: master
  scan_path: /home/wangyibo/ops-transformer_bak
  path: mc2/moe_distribute_combine_v2/
  live_950: moe_distribute_combine_v2.cpp → A5 MTE (`moe_distribute_combine_v2_a5_mte.h`)
  upstream_presence:
    verified: false
    upstream_commit: null
```

`engines: [vector, communication]`. `Process()` is `if ASCEND_IS_AIV`. Optional AddRMSNorm fusion exists in the same A5 file (not the main EP contract).

---

## Coordinates

```
GMM / expert FFN output
expand_x [A, H]     # SAME slot order as dispatch.expand_x  (local expert-major)
     │
     ↓ ExpertAlltoAllDispatchCopyAdd
assist_info_for_combine
    packed (src_rank, tokenId, topkId)   # FillTriple from dispatch; reverse slot
              ↓
send each local expert row back to origin window at
    epOffset = tokenId * (K + sharedExpertNum) + topkId

expert_ids [BS, K]
    reused separately: expert identity / mask / special-expert branch
    does not itself yield tokenId or topkId
     │
     ↓ wait flags in src-rank window
     ↓ LocalWindowCopy + weighted add
token-major x [BS, H] = sum_k expert_scales[token,k] * returned_row
```

Infershape: `x.dim0 = expert_ids.dim0 (BS)`, `x.dim1 = expand_x.dim1 (H)`.

Combine **does** invert dispatch’s rank shuffle. Combine **does not** consume `expanded_row_idx`. Gate reduce lives **here**, so Finalize’s formula is a **single-device sibling**, not the next EP op.

---

## 1. API contract

| name | role |
| --- | --- |
| `expand_x` | **expert output** rows, dispatch’s recv layout. README dtype fp16/bf16 on combine; dispatch may have quantized `expand_x` into GMM and GMM emits a new payload — combine wants the **post-FFN** tensor, same *layout*, often different dtype/lifetime than dispatch `expand_x`. **tensor name ≠ lifetime** again. |
| `expert_ids` | original `[BS, K]` — **same buffer as dispatch input**, not rewritten |
| `assist_info_for_combine` | dispatch `assist_info_for_combine` |
| `ep_send_counts` | dispatch `ep_recv_count` (name flip) |
| `expert_scales` | `[BS, K]` fp32 gate weights |
| `expand_scales` optional | dispatch `expand_scales` |
| `group_list` optional | README **reserved, pass null**. Combine does **not** consume GMM groupList. |
| `shared_expert_x`, `ori_x`, const-expert extras | optional special experts |
| **out `x`** | `[BS, H]` token-major |

Formula (README): `xOut = Sum(expertScales * AllToAllV(expandX) + … shared …)`.

---

## 2. Host / tiling

950 `op_host/op_tiling/arch35/`. `comm_alg` paired with dispatch. `group_list_type` attr exists; input is unused.

---

## 3. Live kernel (950 MTE)

`Process()` (`moe_distribute_combine_v2_a5_mte.h`):

1. `BuffInit`
2. `SetWaitTpStatusAndDisPatch` → `ExpertAlltoAllDispatchCopyAdd`  
   Reads `assist_info` triples `(src_rank, tokenId, topkId)` (`expandIdxLocal[baseOffset+0/1/2]`). Copies `expand_x[tkIndex * H]` to dest window at `epOffset = tokenId * (K + sharedExpertNum) + topkId`.
3. `AlltoAllBuffInitAndMaskCal`
4. `LocalWindowCopy` — wait packed flags, gather returned rows, `ProcessMoeExpert` weighted add into `XOut[token]`

Send uses **token-major (token, k)** window slots recovered from **assist triples**, not from `expert_ids`. Reverse of dispatch’s pack-by-(dst_rank, local_e).

---

## 4. Data transformation

Expert-major (post-GMM, this rank) → AllToAll reverse → **token-major** `[BS, H]`.

Rank ownership of tokens returns to the rank that **owned the original batch**, not to init-routing’s expanded slots.

---

## 5. Intermediate metadata (consumer side)

| tensor | producer | how consumed |
| --- | --- | --- |
| `assist_info_for_combine` | dispatch | reverse index: packed `(src_rank, tokenId, topkId)` (V2; not `expandIdx [BS*K]`) |
| `ep_send_counts` | dispatch `ep_recv_count` | how many rows came from each peer / local expert — wait + copy bounds |
| `expert_ids` | gating | expert identity / mask / special-expert; **not** the reverse slot |
| `expert_scales` | gating `y` **only if fp32**; else converted | weighted sum (Finalize-like math **inside** combine). OpDef is fp32. |
| `expand_scales` | dispatch | optional per-recv-row weights |
| `group_list` | — | **not used** (reserved) |
| init `expanded_row_idx` | — | **not an input** |

`ep_send_counts`: treat as **recv-side histogram of the dispatch step**, reused as send-side bounds on combine. Pair file. Kernel uses it as counts for window occupancy, not as GMM prefix-sum.

---

## 6. Sync

Same MTE window as dispatch, opposite half + opposite data/status direction. Wait on packed token flags (`CheckPackedTokenArrive*`), then consume, then clear. Must not start combine copy until dispatch on the peer has published.

---

## 7. Relation to previous / next

| edge | verdict |
| --- | --- |
| dispatch `assist_info` → combine `assist_info` | **direct-data-edge** |
| dispatch `ep_recv_count` → combine `ep_send_counts` | **direct-data-edge** (rename) |
| dispatch `expand_x` → combine `expand_x` | **logical-edge** if GMM sits in between (same layout, different lifetime). **conditional-direct** only if expert compute is skipped and dtypes match. |
| GMM1 SwiGLU-quant `y` → combine `expand_x` | **logical-edge** (wrong FFN stage / often wrong dtype-width). See GMM2 contract. |
| GMM2 down-proj `y` → combine `expand_x` | **conditional-direct** candidate (same M order, float `[A,H]`) |
| `gating.y` → `expert_scales` | **conditional-direct**: shape/semantics `[BS,K]` gate weights; **dtype direct only if gating `y` is fp32**. Otherwise conversion / another producer. Combine OpDef is fp32. |
| `gating.expert_idx` → `expert_ids` | **direct-data-edge** (same tensor as dispatch) |
| combine `x` → finalize `expanded_x` | **not an edge**. Combine `x` is already `[BS,H]` token-major; finalize wants expert-major float `expanded_x`. |
| combine `x` vs finalize `y` | **same lifetime class** (token-major hidden). EP path typically **ends at combine**, single-device path ends at finalize. **logical alternatives**, not a sequence. |

### Q7 from the EP brief

**Combine does not restore InitRouting expanded-slot order.** It restores **original token order** and applies weights. Therefore:

```
init.expanded_row_idx  ──×──  finalize     (EP path: unused)
dispatch.assist_info   ─────  combine      (EP reverse map)
```

Part 1’s conditional-direct on `expanded_row_idx` is a **non-EP** pairing. EP does not keep that map alive.

---

## 8. CATLASS

**unrelated.** Weighted reduce is Vector, not 43/60/53.

---

## 9. Tests

UT/ST/example standalone. Pairing with dispatch is documented (`必须与 DispatchV2 配套`) but not a joint binary test in-tree.

---

## 10. Evidence conflicts

1. README `AllToAllV` vs MTE window (same as dispatch).
2. `assistInfo` described as “token 个数”; shape is `A*128`.
3. Combine infershape copies **input** `expand_x` dtype to output `x`. Dispatch can emit INT8/FP8 `expand_x`; GMM then SwiGLU-quant may emit INT8 again; combine README lists fp16/bf16. **Caller must present float expert outputs to combine** unless a quant combine path is selected (`comm_quant_mode`). Do not wire dispatch quant `expand_x` straight into combine.
4. `group_list` on the OpDef is dead.
