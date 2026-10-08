# Close-read: MoeFinalizeRoutingV2

Status: **Case A** `cases/compute/moe/ascend950-moe-local-permutation/`. This file remains the evidence dump.

Not a Case card. Does not prove a framework call graph.

```yaml
source_identity:
  registry_ref: ops-transformer
  scanned_commit: 9311b63bcd065b9779eb08a7d634e9463ee41ef1
  scanned_branch: master
  scan_path: /home/wangyibo/ops-transformer_bak
  origin: git@gitcode.com:wang_yibo/ops-transformer.git
  path: moe/moe_finalize_routing_v2/
  aclnn_wrappers: v2 / v3 / v4 (same kernel family; v3/v4 host API only walked as names)
  upstream_presence:
    verified: false
    upstream_commit: null
    note: Path exists on this master. cann/ops-transformer not fetched this turn.
```

Live 950 binary: `moe_finalize_routing_v2_apt.cpp`.

---

## 1. API contract

Inputs (950 `OpDef`):

| name | required | dtype | meaning |
| --- | --- | --- | --- |
| `expanded_x` | yes | fp16 / fp32 / bf16 **only** | README: **FFN / expert output**, not the raw permute of hidden `x` unless the expert compute is a no-op |
| `expanded_row_idx` | yes | int32, **1-D** | map used to gather expert rows back to tokens |
| `x1`, `x2` | optional | same float family | skip / residual addends |
| `bias` | optional | float family | per-expert bias; needs `expert_idx` |
| `scales` | optional | fp16 / fp32 / bf16 | per-(token, k) gate weights. Shape `[T, K]` when present |
| `expert_idx` | optional | int32 `[T, K]` | which expert’s bias to add |
| `x`, `a1`, `a2`, `v` | optional, **950 only** | float family | copy / constant-expert extras (`zero_expert_range` / `copy_expert_range` / `constant_expert_range`) |

Output: `y` `[T, H]`, same float family as `expanded_x`.

README formula (membase / common path):

```
out(i,j) = x1[i,j] + x2[i,j] + sum_k scales[i,k] * (expanded_x[expanded_row_idx[i + k * num_rows], j] + bias[expert_idx[i,k], j])
```

That formula uses **column-major (token, k)** indexing into `expanded_row_idx`. 950 kernels also implement **row-major** (`i*K+k`) when `drop_pad_mode` is 2 or 3.

Attrs:

| attr | values | effect |
| --- | --- | --- |
| `drop_pad_mode` | 0 / 1 / 2 / 3 | **both** drop vs pad **and** row-idx stride. Infershape: 0,2 → `expanded_x` 2-D; 1,3 → 3-D |
| `k` | int | used when `scales` is null to size `y` |

`expert_tokens_count_or_cumsum` is **not an input**.

Evidence: `source-code` def + `moe_finalize_routing_v2_infershape.cpp`; `design-doc` README (formula is column-stride only).

---

## 2. Host / tiling contract

950 `MoeFinalizeRoutingV2Regbase::CalcTilingKey()`:

Capability order (coarse → fine): `IsRowKHFullLoad` → `IsKHFullLoad` → `IsHFullLoad` → else split-H.

| base | meaning |
| --- | --- |
| `30000` | row+K+H full load |
| `40000` | K+H full load |
| `10000` | H full load |
| `20000` | H partial / split |

Then add drop/layout: `+0` dropless-col, `+10` drop-pad-col, `+20` dropless-row, `+30` drop-pad-row, plus a scale-dtype offset (`fp16=1`, `bf16=2`).

A2/A3 cpp: keys `20000`–`20049` (dtype × cut-H/cut-K/db × bias × mix-precision). Different space from 950.

---

## 3. Live kernel

910 cpp: `KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_AIV_ONLY)`.

950 apt: no AIC guard and no `KERNEL_TASK_TYPE`; templates are Vector (`arch35/moe_finalize_routing_v2_*.h`). No Cube MMA.

`execution.task_type: aiv_only`, `engines: [vector]`.

Per-core work: for each token row, load K expert rows from `expanded_x` using `expanded_row_idx`, weight by `scales`, add skips/bias, write `y`.

`SetExpandedRowIdxOffset` (`h_full_load.h`):

- `DROPLESS_COLUMN` / `DROP_PAD_COLUMN`: `offset = token + k * T`
- else (ROW): `offset = token * K + k`

Then that offset is used to **read** `expanded_row_idx`, and the stored value indexes `expanded_x`.

---

## 4. Data transformation

Expert-major (or drop-pad `[E,C,H]`) weighted rows → **token-major** `y` `[T, H]`.

This is the inverse permutation **plus** TopK weighted reduce. It is not a pure unpermute if `scales` / skip / bias / constant experts are present.

Rank: none.

---

## 5. Intermediate metadata

| tensor | who produces | who consumes here |
| --- | --- | --- |
| `expanded_row_idx` | init-routing (intended) | this kernel, layout-sensitive |
| `scales` | gating `y` (intended) | optional weight |
| `expert_idx` | gating `expert_idx` (intended) | only if bias / expert-range extras need it |
| `expanded_x` | **expert compute**, not init-routing, in a real FFN | required payload |
| skip `x1`/`x2` | residual stream | optional add |
| `bias` | expert bias table | optional, indexed by `expert_idx` |
| counts / `groupList` / MX scales / windows | — | **not consumed** |

`INVALID_IDX = -1` in 950 base header matches init gather fill for unused slots.

---

## 6. Synchronization / communication

None. Vector-only. No HCCL.

---

## 7. Relation to previous / next op

**Previous (this trio)**

See `moe_chain_contract.md`. Short version:

- `expanded_row_idx`: **conditional direct-data-edge** from init-routing.
- `expanded_x`: **logical-edge** to init-routing’s permute (same name, usually a GMM/FFN in between); **direct** only in the degenerate “permute then immediately finalize with unquant fp `expanded_x`” path, which no in-tree test shows.
- `scales`: **API-compatible** with gating `y` (`[T,K]`, overlapping dtypes). Finalize `scales` also allows fp32 while gating `y` follows logits dtype.
- `expert_idx`: **API-compatible** with gating `expert_idx`. Optional here.

**Next:** token-order hidden `y`. Downstream is the rest of the decoder block (**logical-edge**).

---

## 8. Relation to existing CATLASS Cases

**unrelated** to 43 / 60 / 66 / 68 / 53 / 73. Weighted gather-reduce, not GEMM.

Case 60 consumes the **counts** this op ignores. Do not treat finalize as “grouped matmul epilogue”.

---

## 9. Test coverage

| area | path |
| --- | --- |
| 950 tiling | `tests/ut/op_host/test_moe_finalize_routing_v2_arch35_tiling.cpp` |
| infershape / A2 tiling | `test_moe_finalize_routing_v2_infershape.cpp`, `test_moe_finalize_routing_v2_tiling.cpp` |
| kernel UT | `tests/ut/op_kernel/test_moe_finalize_routing_v2.cpp` |
| aclnn | ST `tests/st/aclnnMoeFinalizeRoutingV2/`; host API UT v2 and v3 |
| example | `examples/test_aclnn_moe_finalize_routing_v2.cpp` — **hand-written** `expandedX` / `expandedRowIdx` / `scales` / `expertIdx`; `dropPadMode=0`; not wired to init-routing |

---

## 10. Evidence conflicts

1. README formula only shows `expanded_row_idx[i + k * num_rows]` (column). 950 live kernels also use `token * K + k` for modes 2/3. **design-doc incomplete vs source-code.**
2. README says `expandedX` is “MoE FFN 输出”. Init-routing README says `expandedXOut` is the **permuted (maybe quantized) token features**. Same name, **different lifetime**. Treating them as one tensor is a **logical-edge** unless expert compute is skipped and dtypes match.
3. Init can emit INT8 / FP8 / INT4 `expanded_x`. Finalize infershape `IsValidType` is **only** fp32/fp16/bf16. Quantized permute **cannot** feed finalize directly.
4. Example uses `dropPadMode=0` (column). Init example uses `rowIdxType=1` (scatter). That pair is **not** demonstrated to be consistent; they are separate programs.
