# DeepEP A5 split vs Case B

Contrast only. Status: **accepted** (no new Case).

Baseline:

- Case B: `cases/compute-communication/moe/ascend950-moe-ep-dispatch-combine/`
- Pair table: `sources/scans/ops-transformer/close_reads/moe_distribute_v2_pair_contract.md`
- Layer 1: `knowledge/engineering-experience/moe-token-expert-rank-data-contracts.md`

Question: is A5 `MoeDistributeDispatchV2A5` / `CombineV2A5` a **new EP mechanism**, or Case B with a different serving wrapper?

```yaml
source_identity:
  registry_ref: sgl-kernel-npu
  scanned_commit: 8d76a8da75315ae9add5cfd2458b8028289f1ada
  scanned_branch: main
  scan_path: /home/wangyibo/sgl-kernel-npu
  live_950:
    dispatch: csrc/deepep/ops/op_kernel/moe_distribute_dispatch_v2_a5.h
    combine: csrc/deepep/ops/op_kernel/moe_distribute_combine_v2_a5.h
  python: python/deep_ep/deep_ep/buffer.py  (Buffer.dispatch / combine)
  upstream_presence:
    verified: false
  review: accepted
  case: none
```

Do **not** walk A3 `moe_distribute_v2.h`, A2 `ops2/`, or low-latency as a second 950 mechanism.

---

## Communication job vs mechanism

| Layer | What it is | A5 split |
| --- | --- | --- |
| **job / API name** | README AllToAllV / Python Buffer | `dispatch` / `combine` |
| **mechanism** | how bytes move | `GetHcclContext` + window `DataCopy` (**dest-push**) + status flag |
| **serving packaging** | how SGLang picks a backend | `normal_strategy` / `low_latency_strategy` / env HCCS·RDMA·AlltoAll |

Case B already taught: README “AllToAllV” is the **job name**. Live 950 is MTE window. A5 is the same split.

A5 packaging extras (not a new loop): `A5_MTE_STATE_WIN_SIZE` deducted from window; MXFP8/MXFP4 on send; elastic `sharedExpertRankNum`; Python `quant_mode` / Buffer handle.

---

## Same routing loop

```text
token-major x[BS,H] + expert_ids[BS,K]
        ↓ dest-push  (dst_rank, local_e, slot)
window + status
        ↓ WaitDispatch + LocalWindowCopy
expand_x  local-expert-major
        ↓ expert FFN (not this file)
expand_x'
        ↓ Combine reverse (src_rank, token, k) + weighted sum
token-major x_out
```

InitRouting is **not** a predecessor. Combine already restores token-major. No Finalize.

Rank (Moe expert): `toRankId = dstExpertId / moeExpertNumPerRank_ + sharedExpertRankNum_` (`source-code` `SendToMoeExpert`). Same integer `/` as Case B, including shared-expert offset.

Dispatch `Process()` (`source-code`):

```text
AlltoAllDispatch → SetStatus → WaitDispatch → LocalWindowCopy
→ optional AllGather → UpdateTokenNumsOut
```

AIV-only. No MMA.

---

## Map: `expandIdx` is `assist_info`

`EXPAND_IDX_INFO = 3`. `FillTriple` writes `(epRankId_, tokenIndex, k)` into the payload tail. `LocalWindowCopy` copies that triple to GM `expandIdxOut`. Combine reads `expandIdx[tk*3 + {0,1,2}]` as `(rank_id, token_id, topk_id)` and reverse-sends to origin window slot.

| Case B name | A5 name | Role |
| --- | --- | --- |
| `assist_info` triples | `expandIdx` | reverse map; **same lifetime** |
| `expand_x` | `expandXOut` / combine `expandX` | local-expert-major payload |
| `ep_recv_count` | `sendCountsOut` / combine `epSendCount` | occupancy; rename pair |
| `expert_token_nums` | `expertTokenNumsOut` | post-recv GMM candidate; type 0 prefix / 1 count (`expertTokenNumsType_` default 1) |

Layer 1 Rule 1: tensor name ≠ tensor lifetime. Do not Case a second map because the buffer is called `expandIdx`.

---

## What is packaging (not a new Case)

- Python `Buffer.dispatch` → strategy → `torch_npu.npu_moe_distribute_dispatch_v2` (low-latency path) or DeepEP runtime intranode/internode.
- Strategy switch (HCCS / RDMA / AlltoAll env) is **how the job is scheduled**, not a new window protocol.
- A5 MX quant on the send path is dtype on the **same** dest-push.
- Shared-expert ranks and elastic info are Case B knobs that MegaMoe 950 MTE **turned off**; they do not change the loop.

---

## What this file does **not** claim

- That A5 dest-push is MegaMoe dest-pull (that contrast is `fused_deep_moe_a5_vs_megamoe.md`).
- That `DISPATCH_FFN_COMBINE` is 950 (CMake skip).
- Upstream SoT (`upstream_presence.verified: false`). Formal Case accept still needs path/commit on `sgl-project/sgl-kernel-npu`.
