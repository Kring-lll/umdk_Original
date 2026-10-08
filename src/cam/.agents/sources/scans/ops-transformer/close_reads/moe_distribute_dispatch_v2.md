# Close-read: MoeDistributeDispatchV2

Status: **Case B** `cases/compute-communication/moe/ascend950-moe-ep-dispatch-combine/`. This file remains the evidence dump.

Not a Case card. Coordinates first; HCCL AllToAllV in README is the *job*, live 950 path is **MTE window** (`GetHcclContext` + `DataCopy` into rank windows), not `Hccl::AlltoAllV`. Must be read with combine (`moe_distribute_v2_pair_contract.md`).

```yaml
source_identity:
  registry_ref: ops-transformer
  scanned_commit: 9311b63bcd065b9779eb08a7d634e9463ee41ef1
  scanned_branch: master
  scan_path: /home/wangyibo/ops-transformer_bak
  path: mc2/moe_distribute_dispatch_v2/
  live_950: moe_distribute_dispatch_v2.cpp → MTE FullMesh / layered / CCU A5
  upstream_presence:
    verified: false
    upstream_commit: null
```

`execution.task_type`: mix-class binary with **AIV-only `Process()`** (`if ASCEND_IS_AIV`). `engines: [vector, communication]`. No MMA.

---

## Coordinates (this is the point)

```
original local token
(token, k)  +  x[token, :]           # dispatch INPUT. token-major. NOT init.expanded_x
     │
     ├─ dst_rank  = expert_id / localExpertsPerRank  + sharedExpertRankNum
     ├─ local_e   = expert_id % localExpertsPerRank
     └─ send slot = running count for (dst_rank, local_e)
     │
     ↓ MTE write into dest rank window
(dst_rank, src_rank, local_e, slot)  # window packing; payload + triple (src_rank, token, k)
     │
     ↓ wait status, LocalWindowCopy
recv expand_x
(local expert, then src-rank sub-order, slot)   # GMM input rows
     + expert_token_nums[local_e]               # count or prefix-sum of *received* tokens
     + assist_info  (A×128)                     # Combine reverse map, NOT init expanded_row_idx
     + ep_recv_count
```

Rank formula is from live send (`moe_distribute_dispatch_v2.h`):

```
toRankId = dstExpertId / moeExpertNumPerRank_ + sharedExpertRankNum_
win addr  = GetWindAddrByRankId(toRankId)
          + expertPerSizeOnWin_ * (epRankId_ * moeExpertNumPerRank_ + dstExpertId % moeExpertNumPerRank_)
          + hAlignWinSize_ * curExpertCnt
```

(`design-doc` A2 intro used `ceil(expertIds / localExpertNum)`; live send uses **integer `/`**. Treat `/` as SoT.)

---

## The seven questions

### 1. Dispatch input `x` order

**Token-major `[BS, H]`.** Infershape: `bs = x.dim0`, `h = x.dim1`, `expert_ids` is `[BS, K]`. Kernel copies `xGMTensor_[tokenIndex * axisH_]`.

This is **not** init-routing `expanded_x` (`[BS*K, H]` expert-major). Dispatch **re-does** permute from `(token, k)` + `expert_ids`.

**`init.expanded_x → dispatch.x` is not a direct-data-edge.**

`expert_ids` **is** the same INT32 `[T,K]` contract as gating / init. **direct-data-edge** from gating (or SoftmaxV2) `expert_idx`.

### 2. How it knows which rank a token goes to

Global expert id → `(dst_rank, local_expert)` as above. Shared-expert ranks sit in front (`epRankId < sharedExpertRankNum`). Elastic remap optional.

No init histogram required for routing.

### 3. Reorder / pack before send

Yes. Tokens that share `(dst_rank, local_e)` are packed **contiguous in that rank’s window slot strip**, counted by `curExpertCnt`. This is the in-kernel “MoePermute” (`design-doc`). It is **not** a call to `MoeInitRoutingV3`.

Payload tail `FillTriple`: `(epRankId, tokenIndex, k)` packed after the hidden (`tokenQuantAlign_`). That becomes Combine’s reverse address.

### 4. `expand_x` (recv) organization

After `WaitDispatch` + `LocalWindowCopy`: rows are compacted into `expand_x` at prefix-summed `beginIdx`. Multi-expert-per-rank remaps window index:

`winOffset = index % epWorldSize * moeExpertNumPerRank + index / epWorldSize`

So GM `expand_x` is **local-expert-major**, tokens from different src ranks concatenated inside each local expert (order of the status scan / cumsum). Shape `[A, H]` where `A = globalBS * min(localExpertNum, K)` (moe ranks).

### 5. Output metadata

| tensor | dtype | shape | meaning |
| --- | --- | --- | --- |
| `expand_x` | fp / quant | `[A, H]` | local expert-major payload for GMM |
| `dynamic_scales` | fp32 / e8m0 | `[A]` or 2-D (MX/group) | per-recv-token quant scales |
| `assist_info_for_combine` | int32 | `[A * 128]` | V2 replacement for old `expandIdx [BS*K]`. Combine reverse. **Not** init `expanded_row_idx`. |
| `expert_token_nums` | int64 | `[localExpertNum]` | **post-recv** per **local** expert. Attr `expert_token_nums_type`: **0 prefix-sum, 1 count (default 1)** |
| `ep_recv_count` | int32 | `epWorldSize * localExpertNum` (+ extra if `expert_scales`) | Combine names this **`ep_send_counts`** (same buffer, opposite name) |
| `tp_recv_count` | int32 | reserved | unused |
| `expand_scales` | fp32 | `[A]` if `expert_scales` given | per-recv-row gate weight |

### 6. Which metadata is for GMM

**`expert_token_nums`**, length = **local** experts, INT64, count or cumsum via attr.

This is **not** init-routing’s `expert_tokens_count_or_cumsum` (that was pre-EP, possibly global expert range).

GMM `group_list` is INT64 `[E]` with `group_list_type` 0=cumsum / 1=count. **API-compatible conditional-direct** if caller sets matching type and `E == localExpertNum`. Defaults **disagree** (dispatch default type **1 count**, GMM default type **0 cumsum**). No in-tree joint test.

`ep_recv_count` / `assist_info` are **Combine**, not GMM.

### 7. Does this preserve init `expanded_row_idx`?

**No.** Dispatch never reads `expanded_row_idx`. Reverse path is `assist_info_for_combine` + original `expert_ids`. See combine / pair contract.

---

## 1. API contract (short)

Inputs: `x [BS,H]`, `expert_ids [BS,K]`, optional `scales` (quant smooth, not gate weight), `x_active_mask`, `expert_scales` (gate weights, optional), elastic / perf.

Attrs that change coordinates: `ep_world_size`, `ep_rank_id`, `moe_expert_num`, `shared_expert_*`, `quant_mode`, `global_bs`, `expert_token_nums_type`, `comm_alg`.

---

## 2. Host / tiling

950: `op_host/op_tiling/arch35/`. Keys distinguish FullMesh vs layered vs CCU, quant mode, scale presence (`moe_distribute_dispatch_v2_tiling_key.h`). `comm_alg` replaces old HCCL env flags (README).

A2 design doc is **A2 Fullmesh + AICPU RDMA**. Do not treat it as 950 SoT; use it only where kernel agrees.

---

## 3. Live kernel

950 `Process()` (`moe_distribute_dispatch_v2.h`):

1. `AlltoAllDispatch` — mask, `SendToSharedExpert` / `SendToMoeExpert` (MTE into dest windows)
2. `SetStatus` — count + ready flag (data first, then status)
3. `WaitDispatch` — poll own status slots
4. `LocalWindowCopy` — compact window → `expand_x` + assist triples + scales
5. `SetExpertTokenNumsA5` — local expert count or prefix-sum

Communication: window `DataCopy` / `DataCopyPad`, `GetHcclContext`. Not `Hccl::AlltoAllV`.

---

## 4–5. Data transform / metadata

Covered in Coordinates + Q1–Q6.

---

## 6. Sync / communication

MTE window protocol: write payload, then status; waiters poll their slots; then copy out. Combine uses the **other half** of the same window (comment in Init: dispatch rear half, combine front). Pair file.

AIC empty on this `Process`.

---

## 7. Relation to previous / next

| edge | verdict |
| --- | --- |
| hidden residual → `x` | **logical** (same as init’s `x`, but dispatch consumes it **unpermuted**) |
| `gating.expert_idx` → `expert_ids` | **direct-data-edge** |
| `gating.y` → `expert_scales` | **conditional-direct** (optional; fp32 on dispatch) |
| `init.expanded_x` → `x` | **not an edge** (rank/layout) |
| `init.counts` → this op | **no input**. Unused. |
| `expand_x` + `dynamic_scales` → GMM `x` / `x_scale` | **conditional-direct** (quant dtype must match GMM; see GMM close-read) |
| `expert_token_nums` → GMM `group_list` | **conditional-direct**, attr-aligned, **unproven by joint test** |
| `assist_info` + `ep_recv_count` → combine | **direct-data-edge** (pair) |

---

## 8. CATLASS

**unrelated** as a kernel. Metadata kind overlaps Case 60 `groupList` **after** EP, not before.

---

## 9. Tests

Host UT arch35/arch22, kernel UT, aclnn ST, example — all **standalone**. No chain with init-routing or GMM.

---

## 10. Evidence conflicts

1. README formulas write `AllToAllV(X)`. Live 950 is MTE window. **design-doc name vs mechanism.**
2. README `assistInfoForCombineOut` text “给同一专家发送的token个数” — kernel packs **triples and 128-wide assist**, not a plain count vector. Combine README repeats “同一专家收到的token个数”. Treat as **lossy design-doc**; SoT is kernel + `A*128` shape.
3. A2 design `ceil(id/localE)` vs live `/`.
4. Output name `ep_recv_count` is combine input `ep_send_counts`.
