# Close-read: MegaMoe as split-pipeline compression

Not an operator manual. Baseline is the accepted Knowledge
[`moe-token-expert-rank-data-contracts.md`](../../../knowledge/engineering-experience/moe-token-expert-rank-data-contracts.md).

This file answers three questions against that model:

1. Which **API-visible / inter-op GM tensor** edges disappear?
2. Which **metadata** stays, and where (API tensor / workspace GM / window / UB)?
3. Which **synchronization boundaries** are compressed from Host launches into intra-kernel flags?

Do **not** re-teach Case A/B/C, 60, or 53. MegaMoe is the fused **EP loop + expert compute**, not a third routing loop and not a fourth GEMM Case.

```yaml
source_identity:
  registry_ref: ops-transformer
  scanned_commit: 9311b63bcd065b9779eb08a7d634e9463ee41ef1
  scanned_branch: master
  scan_path: /home/wangyibo/ops-transformer_bak
  path: mc2/mega_moe/
  live_950: op_kernel/arch35/mega_moe.h  (class MegaMoe; KERNEL_TYPE_MIX_AIC_1_2)
  sibling_not_host_selected: op_kernel/arch35/mega_moe_layered.h  (URMA; compiled, see below)
  upstream_presence:
    verified: false
    upstream_commit: null
```

`execution.task_type: mix`. `engines: [cube, vector, communication]`.

Live SoT for this close-read is **950 MTE `MegaMoe`**. Host `CalTilingKey` **hardcodes** `TILINGKEY_TPL_MTE` (`op_host/op_tiling/arch35/mega_moe_tiling.cpp`). `MegaMoeLayered` exists in the TPL and `apt.cpp`, but this Host function does not select it. `tilingData->topoType` still copies attr `topo_type`; that is **not** the tiling-key path. Do not treat URMA as the 950 production Process.

README product table: 950 √, A2/A3 ×. OpDef still `AddConfig` `ascend910b` / `ascend910_93` and there is an `arch22/` tree. **This file does not walk arch22.**

README “RDMA peermem” is `Mc2MoeContext.epHcclBuffer` + `GetRankWinAddrWithOffset`. Same window family as Case B. Not `Hccl::AllToAllV`.

`apt.cpp` actually instantiates the 950 body only when `ORIG_DTYPE_X/Y == BF16` and weight1/weight2 dtypes match. OpDef lists more x dtypes; live kernel guard is narrower (`source-code` vs `design-doc`).

---

## What this op is (one sentence)

One MIX kernel for the **Case B routing loop** plus **Case C GMM1+SwiGLU+quant** plus **GMM2 down-proj** plus weighted restore. Inputs are still token-major `x` + `topk_ids`. Output `y` is token-major hidden. **Not** Case A; no InitRouting, no Finalize, no `expanded_row_idx`.

---

## API-visible I/O vs split pipeline

Split EP+FFN graph (Knowledge / chain E16–E22) materializes **API-visible / inter-op GM tensors** between ops:

| split tensor | MegaMoe API I/O |
| --- | --- |
| dispatch `x` `[BS,H]` | **in** `x` |
| `expert_ids` `[BS,K]` | **in** `topk_ids` |
| combine `expert_scales` | **in** `topk_weights` (fp32 **or** bf16; kernel casts bf16→fp32) |
| GMM `weight` / MX scales | **in** `weight1`/`weight2` + `weight_scales1/2` |
| dispatch `expand_x` | **gone** (API-visible) |
| dispatch `assist_info` | **gone** (API-visible) |
| dispatch `ep_recv_count` | **gone** (API-visible) |
| GMM `group_list` | **gone** (API-visible) |
| GMM1 `y` / `y_scale` | **gone** (API-visible) |
| GMM2 `y` (= combine `expand_x`) | **gone** (API-visible) |
| combine `x` | **out** `y` `[BS,H]` |
| dispatch `expert_token_nums` | **out** `expert_token_nums` `[localE]` **INT32** |

Infershape: `y` copies `x` `[BS,H]`; `expert_token_nums` length = `moe_expert_num / ep_world_size` (`source-code`). No `group_list_type` / `expert_token_nums_type` attrs.

`x_active_mask` / `scales` / `combine_quant_mode` / `comm_alg`: README reserved; tiling requires `combineQuantMode==0` and empty `commAlg` on the documented path (`design-doc` + host checks).

---

## Coordinate spine (fused, 950 MTE)

```text
token-major x[BS,H] + topk_ids[BS,K]
        │  QuantProcessInRank: MX quant into **this rank** window (token-major)
        │  SendMaskCal: per global expert [mask|count] → dest window
        │  CrossRankSync
        ↓
dest pulls source window by tokenIndex (CopyGMToGMPerToken)
        → workspace local-expert-major dispatchRevData (+ scale)
        → workspace triples (rank, tokenIndex, topkIndex)   // still EP map
        │  GMM1+SwiGLU+quant  preserve M
        → workspace swigluQuantData (+ scale)
        │  GMM2 preserve M
        │  CombineTokens: scatter by triple into origin window
        → window (token, k) slots at combineSendPtr
        │  CrossRankSync
        ↓
Unpermute: read (token*K+k), * topk_weights, write y[BS,H]
```

Rank formula (`SendMaskCal`, `source-code`):

```text
dstRank = globalExpertId / expertPerRank
local_e = globalExpertId % expertPerRank
```

**No** `sharedExpertRankNum` offset. Case B’s shared-expert front ranks are **not** in this kernel. Do not copy B’s rank formula here.

Window vs Case B pack:

| | Case B Dispatch | MegaMoe MTE |
| --- | --- | --- |
| payload | **push** packed into dest `(dst_rank, local_e, slot)` | **leave** quantized token-major on **source** window; dest **pulls** |
| occupancy | recv `expand_x` rows + `expert_token_nums` | dest window `[mask\|count]` per `(local_e, src_rank)` |
| reverse map | API-visible `assist_info` triples | workspace `tripleInfoPtr`; Unpermute does **not** re-read it |

README stage-4 `π(i,k)` is `design-doc` for the **combine window** layout `(token * K + k)`, filled by `CombineTokens`. It is **not** InitRouting `expanded_row_idx`.

---

## 1. GM materialization

Fusion deletes **API-visible / inter-op GM tensor handoff**, not all device GM. Workspace buffers can still be GM.

| lifetime (Knowledge spine) | split API-visible tensor | MegaMoe where it lives | deleted as inter-op handoff? |
| --- | --- | --- | --- |
| token-major in | dispatch `x` | API `x` | no |
| rank/expert window | dest packed expand | source quant window + dest mask slots | **layout changed**; still window GM |
| local-expert-major expert **input** | dispatch `expand_x` | workspace `dispatchRevDataPtr` (+ `dispatchRevScalePtr`) | **yes** |
| GMM1 out / GMM2 in | GMM1 `y`/`y_scale` | workspace `swigluQuantDataPtr` / `swigluQuantScalePtr` | **yes** |
| expert **output** float `[M,H]` | GMM2 `y` / combine `expand_x` | not kept as expert-major API tensor; A8W4/quant paths may keep `gmm2MmadResPtr` workspace | **yes** |
| token×k restore buffer | combine window | peermem `combineSendPtr` | still window GM |
| token-major out | combine `x` | API `y` | no |

GMM1 Cube partial (`gmm1MmadResPtr`) is **conditional workspace** (A8W4 tiling mode), not a new API tensor.

Do not write “mega_moe has no expand_x”. It has no **API-visible** `expand_x`. The row buffer is workspace GM.

---

## 2. Metadata: still there, different address

Rule 5 still applies. Producer time / scope / encoding did not become “no metadata”.

### Routing map (Rule 4)

Still **EP triples**, not Case A `expanded_row_idx`.

Fill (`CopyGMToGMPerToken`): `(remoteRankIdx, tokenIndex, topkIndex)` → workspace `tripleInfoPtr`, 8×int32 per row (`TRIPLE_SIZE` / `INT32_PER_256B`).

Consume (`CombineTokens` in `mega_moe_combine_send.h`): each GMM2 row writes origin window at `(tokenIdx * topK + topkIdx) * n + nLoc`.

Unpermute iterates `token × k` on that window and **does not** index `tripleInfoPtr`. The map is used at **scatter**, then dropped from the restore loop.

API-visible `assist_info` / `ep_recv_count` edges (**E18/E19**) are gone. The map did not change loop.

### Group counts (Rule 5)

Internal, post-recv, local-expert scope:

- `SendCntCal` reads dest-window counts per src rank, builds **per-src-rank prefix** in `cumsumInfoTensor_` (`[local_e, src_rank]`).
- Writes **count** `sendCnt` (this expert, all src ranks) to workspace `expertRevTokenNumsPtr` and notifies AIC (`flagSendCntCalToUpdParamsPtr`).
- GMM `M` = that count (`UpdateGroupParams`). No API-visible `group_list`. No type-0/1 attr. Internally **count**, then GMM walks groups by looping `expertPerRank` (60-style slice-M without a list tensor).
- API-visible `expert_token_nums` is a **late copy-out**: last-src-rank prefix minus previous expert (`ExpertTokenNumCopyOut`) → **INT32 counts**, length `localE`.

| | Dispatch V2 nums | MegaMoe `expert_token_nums` |
| --- | --- | --- |
| time | post-EP recv | same |
| scope | local experts this rank | same |
| encoding | attr 0 prefix / 1 count (default 1) | **count only** (kernel subtract) |
| dtype | INT64 | **INT32** |
| role | GMM `group_list` candidate | **output only**; GMM already consumed workspace counts |

Rule 3 “Dispatch default count vs GMM default cumsum” **does not apply to the fused API graph**. The compose bug is internalized (they never pass a list). Do not assume the **output** nums can still feed a standalone GMM without a dtype/type check — INT32 vs GMM INT64, and no type attr.

Init counts remain **not** this op’s producer (`active_expert_range` is not a MegaMoe attr).

### Gate weights (Rule 3 sibling)

Split Combine OpDef: `expert_scales` **fp32** only → gating `y` was **conditional-direct**.

MegaMoe OpDef: `topk_weights` fp32 **or** bf16. Kernel `UnpermuteBuffInit` copies fp32 as-is or **Cast** bf16→fp32. The dtype compose is handled **inside** the fused op. Still not fp16 weights.

---

## 3. Synchronization boundaries

Split Host sequence (four launches, GM happens-before):

```text
Dispatch  →  GMM1  →  GMM2  →  Combine
```

MegaMoe **one** launch. Intra-kernel (`Process`, `source-code`):

```text
SendMaskCal + QuantProcessInRank
    SyncAll + CrossRankSyncInWorldSize          // world: masks + quant visible
Dispatch expert e+1  ∥  GMM1 expert e
    wave flags dispatchToGmm1Flag (AtomicAdd vs busy-wait)
    sendCnt flag per (expert, AIC)
GMM1 epilogue → flagSwiGluToGmm2Ptr             // per expert
GMM2 + CombineTokens (same expert loop)
    PipeBarrier + SyncAll
CrossRankSyncInWorldSize                        // world: combine window filled
Unpermute
```

Compressed (no Host edge):

- Dispatch complete → GMM1 start (now wave-grain flags; prefetch next expert’s dispatch)
- GMM1 `y` GM → GMM2 (workspace + per-expert flag; Case C’s MIX AIC→AIV stays **inside** GMM1)
- GMM2 result → CombineTokens handoff (from L0C/UB using triples; generic no-quant path does not require an expert-major API tensor equivalent to split GMM2 `y`)
- `ep_recv_count` rename handshake

**Not** compressed to zero:

- Two **world** `CrossRankSyncInWorldSize` barriers (after quant/mask; after combine send). Fusion does not delete EP visibility.
- `SyncAll` around flag reset / GMM2 end.
- A8W4: extra GM backup of cumsum because SwiGLU epilogue overwrites UB (`cumsumInfoGlobalTensor_`). That is a **fusion tax**, not a deleted edge.

Do not claim “communication and GMM fully overlap with no barrier”. Prefetch is **next expert dispatch vs current GMM1**, after a world sync that makes source quant/masks visible.

---

## Against Knowledge rules

| Rule | After fusion |
| --- | --- |
| 1 name ≠ lifetime | API graph no longer has two `expanded_x`. Workspace still does: `dispatchRev*` = expert **input**; combine window = restored **(token,k)** slots, **not** the same layout as expert output. |
| 2 shape ≠ layout | Map is still triples vs `topk_ids` identity. Unpermute layout is `(token*K+k)`, not expert-major. README `π` ≠ Init row idx. |
| 3 API ≠ defaults | API `group_list_type` compose **disappears**. `topk_weights` bf16 is converted. Output nums INT32 count is a **new** compose trap if someone wires it to standalone GMM. |
| 4 map ∈ loop | Still **B’s EP loop**. Triples stay inside the kernel. Do not invent a Case A map. |
| 5 time+scope+type | Internal counts = post-EP, localE, count. API output is the same lifetime, different dtype/role. Ask the three questions anyway. |
| freeze: expert compute must not permute M | `UpdateGroupParams` M = recv count; GMM1/GMM2 share that M and `expertBeforeCnt`. **Preserve M** holds; no new permutation Case. |

---

## Chain edges this file closes

Relative to `moe_chain_contract.md` Part 2:

| edge | fused verdict |
| --- | --- |
| E16 nums → GMM `group_list` | **internalized**; not an API-visible edge |
| E17 GMM1 `y` → combine `expand_x` | **internalized** (workspace + GMM2) |
| E18 `assist_info` | **internalized** (workspace triples) |
| E19 `ep_recv_count` | **replaced** by window `[mask\|count]` |
| E21 gating `y` → scales | **conditional-direct relaxed**: fp32 or bf16 |
| E22 GMM2 | **internalized**; `grouped_matmul` not called |

E20 (combine vs finalize) unchanged: MegaMoe is the EP endpoint. Still not Case A.

---

## What NOT To Copy

- 「mega_moe 把 expand_x / assist / groupList 从设备上删掉了。」删的是 **API-visible / inter-op GM tensor**。workspace 仍可以是 GM。
- 「README π 是 InitRouting map。」combine window `(token,k)` + triples。
- 「和 DispatchV2 一样 push 进 dest expert slot。」MTE live 是 source-window pull。
- 「CalTilingKey 会按 `topo_type` 走 URMA。」key 写死 MTE。
- 「带 shared-expert offset 的 B 公式。」本 kernel 没有。
- 「输出 `expert_token_nums` 就是 60 的 groupList。」INT32、count、且 GMM 已跑完。
- 把 arch22 / Layered 当 950 SoT。
- 把 60/53 Cube 课再抄一遍。GMM1 仍是 60+53 extension，融合点在 **谁喂 M、结果写哪**。

---

## Still open

- Joint graph tests (none in-tree that compose split ops vs this op).
- `upstream_presence` vs `cann/ops-transformer`.
- URMA `MegaMoeLayered` contracts if Host ever selects that tiling key.
- Whether API-visible `expert_token_nums` is consumed by any in-tree successor (debug vs real producer).
- A2 `arch22` vs README product ×.

---

## Case implication

Card: `cases/compute-communication/moe/ascend950-mega-moe-fused-ep-ffn/` (`status: accepted`) — compression of **B + C + GMM2**, not a fourth GEMM Case. Knowledge layer 2: `knowledge/engineering-experience/moe-fusion-materialization-and-sync.md`.

Related:

- Knowledge baseline: `knowledge/engineering-experience/moe-token-expert-rank-data-contracts.md`
- B: `cases/compute-communication/moe/ascend950-moe-ep-dispatch-combine/`
- C: `cases/compute/grouped-gemm/ascend950-gmm-swiglu-mx-quant/`
- GMM2 footnote: `grouped_matmul_downproj_contract.md`
- Edges: `moe_chain_contract.md`
