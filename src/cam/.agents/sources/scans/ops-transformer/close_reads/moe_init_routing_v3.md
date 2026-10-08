# Close-read: MoeInitRoutingV3

Status: **Case A** `cases/compute/moe/ascend950-moe-local-permutation/`. This file remains the evidence dump.

Not a Case card. Does not prove a framework call graph.

```yaml
source_identity:
  registry_ref: ops-transformer
  scanned_commit: 9311b63bcd065b9779eb08a7d634e9463ee41ef1
  scanned_branch: master
  scan_path: /home/wangyibo/ops-transformer_bak
  origin: git@gitcode.com:wang_yibo/ops-transformer.git
  path: moe/moe_init_routing_v3/
  aliases_on_950: moe_token_permute* → v2/v3 (Scout; not re-walked here)
  upstream_presence:
    verified: false
    upstream_commit: null
    note: Path exists on this master. cann/ops-transformer not fetched this turn.
```

Live 950 binary: `moe_init_routing_v3_apt.cpp`.

---

## 1. API contract

Inputs (`moe_init_routing_v3_def.cpp`):

| name | required | 950 dtypes | meaning |
| --- | --- | --- | --- |
| `x` | yes | fp16/bf16/fp32/int8/hif8/fp8/fp4 | **token hidden**, `[T, H]`. Not gating logits. |
| `expert_idx` | yes | int32 | `[T, K]` expert ids |
| `scale` | optional | fp32 or e8m0 | quant smooth / MX scale, not gate weights |
| `offset` | optional | fp32 | static-quant offset |

Outputs:

| name | dtype | dropless shape | drop-pad shape |
| --- | --- | --- | --- |
| `expanded_x` | same as `x` if `quant_mode=-1`; else quantized | `[T*K, H]` 2-D | `[E, C, H]` 3-D |
| `expanded_row_idx` | int32 | 1-D length `outNum` (typically `T*K`) | 1-D |
| `expert_tokens_count_or_cumsum` | int64 | `[expertEnd-expertStart]` (type 0/1) or `[E, 2]` (type 2) | same; DropPad forces type 1 |
| `expanded_scale` | fp32 or e8m0 | depends on `quant_mode` | — |

Key attrs (`infershape` enums):

| attr | values | kernel meaning (`source-code`) |
| --- | --- | --- |
| `drop_pad_mode` | 0 dropless / 1 drop-pad | 2-D vs 3-D `expanded_x` |
| `expert_tokens_num_type` | 0 `CUMSUM` / 1 `COUNT` / 2 `KEY_VALUE` | histogram vs prefix-sum vs (id, count) pairs |
| `expert_tokens_num_flag` | bool | README constraint: 950 “只支持 true” |
| `quant_mode` | `-1` and many 950 MX/FP8/HiF8/INT4 modes | changes `expanded_x` dtype |
| `row_idx_type` | 0 `GATHER` / 1 `SCATTER` | how `expanded_row_idx` is filled |
| `active_expert_range` | `[start, end)` or empty=`[0, expertNum)` | filters which expert ids are active |

`active_num` is validated (`== T*K`) but README says it is unused for the actual row count.

Evidence: `source-code` def + `moe_init_routing_v3_infershape.cpp`; `design-doc` README.

---

## 2. Host / tiling contract

950: `MoeInitRoutingV3Arch35TilingClass::GetTilingKey()`.

- Empty tensor → `3000000`.
- Full-load (when `isFullload_` and quant mode allowed) → `FULLLOAD` base + quant factor.
- Else composite: sort mode × 1e6-class bases + quant factor + `rowIdxType` + `dropPadMode` (see `TILINGKEY_BASE` in `moe_init_routing_v3_tiling_arch35.cpp`).

950 apt dispatches unquant / static / dynamic / MXFP8 / MXFP4 / FP8 per-block / FP8 group / HiF8 / DropPad gather paths (`moe_init_routing_v3_apt.cpp` includes).

A2/A3 cpp uses a different numeric key space (`1000000`… `2312000`) and `KERNEL_TYPE_MIX_AIV_1_0`.

---

## 3. Live kernel

910 cpp: `KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_MIX_AIV_1_0)` then **`if (g_coreType == AIC) return`**. MIX binary, Cube idle.

950 apt: same AIC early-return; no `KERNEL_TASK_TYPE` in apt. Work is Vector (sort, histogram, gather/scatter of rows, optional quant).

`execution.task_type: mix` on 910, `engines: [vector]` on both. Do not read MIX as Cube.

Kernel stages (README + cpp includes, unquant dropless):

1. Sort `expert_idx` → `sortedExpertIdx` + `sortedRowIdx`.
2. Build `expanded_row_idx` (gather vs scatter).
3. Histogram → `expert_tokens_count_or_cumsum`.
4. Gather `x` rows into expert-major `expanded_x` (and quant if requested).

---

## 4. Data transformation

Token order `[T, H]` + `[T, K]` ids → **expert-major** `expanded_x`:

- Dropless: rows are tokens duplicated by K, **sorted by expert id**. Shape `[T*K, H]`.
- DropPad: `[E, C, H]`, capacity `C` per expert; overflow dropped, shortfall padded.

Rank: none. This op does not talk to other EP ranks.

`x` hidden dim `H` is preserved (except packed INT4 along H).

---

## 5. Intermediate metadata

This is the expensive contract.

### `expert_idx` (input)

Produced by gating / softmax-v2 / caller. This op **sorts** it; it does not emit a new expert-id tensor.

### `expanded_row_idx` (output, INT32, 1-D)

`row_idx_type`:

- `0 GATHER` (`moe_v3_common.h`): `RowIdxGather` writes `expandedRowIdx[sortedExpertIndices[i]] = i` (`arch35/moe_v3_row_idx_gather.h` SIMT). Values are **expanded slots**, indexed by **original flattened (token, k)**. Invalid slots filled with `-1` (README).
- `1 SCATTER`: sort kernels keep the sorted original indices as the output (see `moe_v3_sort_one_core.h` `if (rowIdxType_ == SCATTER)`). Indexed by **expanded slot**, values are **original positions**. Invalid slots uninitialized (README).

These two are **inverses**. Finalize’s `drop_pad_mode` column vs row chooses a different **read stride** into this 1-D buffer (`i + k*T` vs `i*K + k`). Pairing is **not** “same name ⇒ same layout”. See chain contract.

### `expert_tokens_count_or_cumsum` (output, INT64)

Kernel (`moe_v3_expert_tokens_count.h`):

| `expert_tokens_num_type` | enum | what is stored |
| --- | --- | --- |
| 0 | `CUMSUM` | prefix-sum over the active expert range (fills gaps with running total) |
| 1 | `COUNT` | per-expert **histogram counts**, not prefix-sum |
| 2 | `KEY_VALUE` | `[E, 2]` (expert id, count) for nonzero experts |

Shape for 0 and 1 is the same `[expertEnd-expertStart]`. **You cannot tell count vs prefix-sum from shape.** Downstream GMM `groupList` cares which one it is. Finalize **does not take this tensor**.

### `expanded_scale` (output)

Quant path only. MX/e8m0/group/block layouts follow `quant_mode`. Not gate weights. Not finalize `scales`.

### `scale` / `offset` (inputs)

Quant parameters, produced by the caller / previous quant op, **not** by gating `y`.

---

## 6. Synchronization / communication

No HCCL. Multi-core Vector sort/merge uses workspace and the usual copy-compute pipe inside one kernel. AIC is not waiting on AIV; AIC is empty.

---

## 7. Relation to previous / next op

**Previous**

| candidate | verdict |
| --- | --- |
| `MoeGatingTopK.expert_idx` → `expert_idx` | **API-compatible direct-data-edge** (INT32 `[T,K]`). Canonical `design-doc` names SoftmaxV2 instead. |
| `MoeGatingTopK.y` → this op | **no edge**. No gate-weight input. |
| `MoeGatingTopK.x` (logits) → `x` | **not an edge**. Wrong tensor (logits vs hidden). |
| hidden activations → `x` | **logical-edge** to the rest of the model; producer is outside these three ops. |

**Next (this close-read trio only)**

| output | finalize input? |
| --- | --- |
| `expanded_x` unquant fp16/bf16/fp32, dropless 2-D | **conditional** name/dtype/rank match; **not** if quantized; **not** if a GMM sits in between (then this tensor is expert *input*, finalize wants expert *output*) |
| `expanded_row_idx` | **conditional** on `row_idx_type` vs finalize `drop_pad_mode` stride |
| `expert_tokens_count_or_cumsum` | **no edge** to finalize |
| `expanded_scale` | **no edge** to finalize `scales` |

---

## 8. Relation to existing CATLASS Cases

- vs **60**: **same kind of metadata**, not a proven producer→consumer edge. Type 0 prefix-sum is the same *shape of contract* as Case 60 `groupList`. Whether ops-transformer GMM actually consumes **this** tensor (vs post-EP dispatch counts) is a Part 2 unresolved edge. 60 itself does not sort tokens.
- vs **53**: MX-class dtypes appear on **routing quant** (`quant_mode` 2/3/9/16/17), which is **not** Case 53 `MmadMx` / group-32 B-scale GEMM.
- vs 43 / 66 / 68 / 73: **unrelated**.

---

## 9. Test coverage

| area | path |
| --- | --- |
| 950 tiling | `tests/ut/op_host/test_moe_init_routing_v3_arch35_tiling.cpp` (count/cumsum/kv, drop-pad, scatter, quant keys) |
| A2 tiling / infershape | `test_moe_init_routing_v3_tiling.cpp`, `test_moe_init_routing_v3_infershape.cpp` |
| kernel UT | `tests/ut/op_kernel/test_moe_init_routing_v3.cpp` |
| aclnn | ST `tests/st/aclnnMoeInitRoutingV3/`; example uses **synthetic** `expertIdx`, `quantMode=-1` but allocates INT8 `expandedX` (example is not a live chain) |

No joint test with GatingTopK or FinalizeV2.

---

## 10. Evidence conflicts

1. README attr table: type `1` “count模式，即…**累计值**”. Kernel `COUNT_MODE` writes **per-expert counts**; **累计 / prefix-sum is type 0 `CUMSUM`**. Output-table row for type 1 says “处理token的**总数**”, which matches COUNT. **design-doc attr row contradicts source-code and the output-table row.**
2. Init README / 950 op-list: “根据 aclnnMoeGatingTopK**SoftmaxV2**”. This close-read’s sibling is `MoeGatingTopK`. Same `expert_idx` dtype/shape; different op.
3. Example `test_aclnn_moe_init_routing_v3.cpp`: `quantMode=-1` with `ACL_INT8` `expandedX` — dtype pairing looks like a leftover from a quant example (`test` vs def).
4. 910 `KERNEL_TYPE_MIX_AIV_1_0` vs empty AIC: MIX ≠ Cube.
