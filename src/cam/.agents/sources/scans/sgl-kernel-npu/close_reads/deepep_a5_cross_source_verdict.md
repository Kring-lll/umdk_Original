# DeepEP A5 cross-source verdict

Decision file. Status: **accepted** → no Case / no Knowledge. Evidence in:

- `deepep_a5_split_vs_case_b.md`
- `fused_deep_moe_a5_vs_megamoe.md`

```yaml
source_identity:
  registry_ref: sgl-kernel-npu
  scanned_commit: 8d76a8da75315ae9add5cfd2458b8028289f1ada
  upstream_presence:
    verified: false
    note: Scout/close-read on wangyibo1005 fork. Formal Case accept still needs sgl-project SoT path/commit.
  review: accepted
  case: none
```

Success criterion for this source: **existing Case/Knowledge eat duplicate mechanism**. New Case only if source-code shows a new one.

---

## Split DeepEP A5

| question | answer |
| --- | --- |
| new mechanism? | **no** |
| what is it? | Case B **alternative** / production serving wrapper |
| new Case? | **no** |

Same EP loop, dest-push MTE window, FillTriple map, AIV `Process` sequence, Combine as rank-shuffle inverse + weighted reduce. `expandIdx` = `assist_info` triples (Layer 1 Rule 1).

What SGLang adds is **packaging**: Python Buffer, strategy switch (HCCS/RDMA/AlltoAll/low-latency), A5 MX dtypes, 1MB window state zone, elastic/shared-expert knobs.

Do **not** confuse:

| | |
| --- | --- |
| communication **job** | AllToAllV / `Buffer.dispatch` |
| communication **mechanism** | MTE window dest-push + status |
| serving **semantics** | which strategy/env launches that job |

---

## Fused DeepEP A5

| question | answer |
| --- | --- |
| new mechanism? | **no** |
| what is it? | Case D **alternative**: fused EP+FFN on the Case B loop |
| inner comm vs D | dest-**push** (B morphology), not dest-**pull** (D morphology) |
| Layer 2 survives? | **yes** — strengthen evidence; do **not** write Knowledge layer 3 |
| new Case? | **no** |
| new GEMM Case? | **no** (CATLASS MX GMM inside fusion) |

Layer 2 three questions classify A5 fused. Several table **cells** differ from MegaMoe (`group_list` relocated vs replaced; GMM2 `y` relocated vs deleted; `SetStatus/WaitDispatch` vs `CrossRankSyncInWorldSize`). The **taxonomy** still fits. That is the test passing.

Python FuseMode 1 “InitRouting + AllToAll” is **not** the A5 kernel. Do not let that docstring reopen Case A.

`DISPATCH_FFN_COMBINE` stays out of 950 (CMake skip). Not this verdict.

---

## Gates that would have opened a new Case (none hit)

| gate | A5 evidence |
| --- | --- |
| new rank/window protocol class | no — B dest-push |
| new map lifetime | no — same triples, workspace or API |
| new expert scheduling | no |
| new communication-compute **pipeline class** vs D | fused EP+FFN is D’s class; push vs pull already in B vs D |
| new capacity / token-ownership contract | A5 capacity attr is host packing; not a new ownership model in this close-read |

---

## Knowledge

| asset | action |
| --- | --- |
| Layer 1 split contracts | **reuse** — A5 split is Case B |
| Layer 2 fusion Q1–Q3 | **reuse + strengthen** — A5 fused is a second filled table |
| new Knowledge file | **none** |

Optional later (not now): one sentence in Layer 2 “Related” pointing at these three files as cross-source evidence. Only after Review, and without turning Layer 2 into a MegaMoe+DeepEP manual.

---

## Catalog / next

| id | status |
| --- | --- |
| `sgl-deepep-ep-dispatch-combine` | **accepted** contrast; Case B alternative; **no Case** |
| `sgl-deepep-fused-moe-a5` | **accepted** contrast; Case D alternative; **no Case**, **no new Knowledge** |
| `sgl-sparse-attention-score` | Case **accepted** at `cases/compute/attention/ascend950-block-sparse-paged-attention/` |
| CATLASS wrappers / A3 MLA·indexer·FLA / DeepEP A2 | deferred |

sgl-kernel-npu’s success metric: DeepEP A5 did **not** mint a Case or a Knowledge file. The only new family left is Sparse Attention.
