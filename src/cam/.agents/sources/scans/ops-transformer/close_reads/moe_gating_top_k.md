# Close-read: MoeGatingTopK

Status: **upstream of Case A** `cases/compute/moe/ascend950-moe-local-permutation/`. Not a standalone Case. This file remains the evidence dump.

Not a Case card. Does not prove a framework call graph.

```yaml
source_identity:
  registry_ref: ops-transformer
  scanned_commit: 9311b63bcd065b9779eb08a7d634e9463ee41ef1
  scanned_branch: master
  scan_path: /home/wangyibo/ops-transformer_bak
  origin: git@gitcode.com:wang_yibo/ops-transformer.git
  path: moe/moe_gating_top_k/
  upstream_presence:
    verified: false
    upstream_commit: null
    note: Path exists on this master. cann/ops-transformer not fetched this turn. Official docs link the same tree path.
```

Live 950 binary: `moe_gating_top_k_apt.cpp` (`opFile.value` = `moe_gating_top_k_apt`).

---

## 1. API contract

Inputs (950 `OpDef` in `moe_gating_top_k_def.cpp`):

| name | required | dtype | shape (infershape) | meaning |
| --- | --- | --- | --- | --- |
| `x` | yes | fp16 / fp32 / bf16 | `[T, E]` | per-token expert logits. **Not** hidden activations. |
| `bias` | optional | same as `x` | `[E]` | added to normalized scores before TopK |
| `input_ids` | optional, 950 only | int32 / int64 | — | Hash mode |
| `tid2eid` | optional, 950 only | int32 / int64 | — | Hash mode lookup |

Outputs:

| name | dtype | shape | meaning |
| --- | --- | --- | --- |
| `y` | same as `x` | `[T, K]` | gate weights for the selected K experts (after gather from `normOut`, optional renorm, `* routedScalingFactor`) |
| `expert_idx` | int32 | `[T, K]` | selected expert ids, token-major |
| `out` | fp32 | `[T, E]` | full `normOut` if `out_flag=true`; empty otherwise |

Attrs that change the contract: `k`, `k_group`, `group_count`, `group_select_mode`, `renorm`, `norm_type` (0 softmax / 1 sigmoid / 2 sqrt-softplus, 2 is 950-only per README), `out_flag`, `routed_scaling_factor`, `eps`.

Evidence: `source-code` `op_host/moe_gating_top_k_def.cpp`, `op_host/moe_gating_top_k_infershape.cpp`; `design-doc` `README.md`.

---

## 2. Host / tiling contract

950 path: `MoeGatingTopKTilingRegbase` in `moe_gating_top_k_tiling_arch35.cpp`.

| condition | tiling key |
| --- | --- |
| TopK mode (`hashFlag_` false) | `10000` |
| Hash + `input_ids` int32, `tid2eid` int64 | `10001` |
| Hash + both int32 | `10002` |
| Hash + both int64 | `10003` |
| Hash + `input_ids` int64, `tid2eid` int32 | `10004` |

A2/A3 (`moe_gating_top_k.cpp`, not apt): keys `0` (E=256, groups=8, kGroup=4, k≤32, with bias), `1` (no real grouping), `2` (generalized grouped TopK). 950 does **not** use those three keys.

---

## 3. Live kernel

- 910 cpp: `KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_AIV_ONLY)`; `if (g_coreType == AIC) return`.
- 950 apt: same AIC early-return; no `KERNEL_TASK_TYPE` in the apt file. Work is Vector.

`execution.task_type: aiv_only`, `engines: [vector]`. Cube is unused.

950 kernel (`moe_gating_top_k_apt.cpp` → `arch35/moe_gating_top_k_regbase.h`): normalize logits, optional group mask, TopK (or hash lookup), write `y` + `expert_idx` (+ optional `out`).

---

## 4. Data transformation

- Rank: none.
- Token dim `T` is preserved.
- Expert dim `E` → `K` on `y` and `expert_idx`. Layout of those two is **token-major `[T, K]`**.
- `out` stays `[T, E]` (full expert axis).

Does **not** permute hidden tokens. Does **not** emit `expanded_row_idx`, counts, or `groupList`.

---

## 5. Intermediate metadata

| tensor | producer | consumer in this op | notes |
| --- | --- | --- | --- |
| `expert_idx` | this op | none internally after write | INT32 `[T,K]`. This is the object init-routing sorts. |
| `y` (gate weights) | this op | none internally after write | **Not** an init-routing input. Candidate for finalize `scales`. |
| `out` / `normOut` | this op, optional | none in the routing trio | full expert scores, not TopK weights |
| `input_ids` / `tid2eid` | caller | hash path only | bypasses TopK; still writes `expert_idx` |

No expert counts. No prefix-sum. No window/status.

---

## 6. Synchronization / communication

None. Single-device Vector. No HCCL.

---

## 7. Relation to previous / next op

Previous: logits `x` come from a gate linear (outside this repo’s close-read). **logical-edge**.

Next, **only** `expert_idx` is type-compatible with `MoeInitRoutingV3.expert_idx` (INT32, 2-D `[T,K]`). That is an **API-compatible direct-data-edge** on that one tensor.

It is **not** the documented canonical producer. `docs/zh/ascend950_op_list.md` and `moe_init_routing_v3/README.md` name **`aclnnMoeGatingTopKSoftmaxV2`**, a sibling op (`moe_gating_top_k_softmax_v2/`) that also emits `y` + `expert_idx` + `softmax_result`. Same INT32 `[T,K]` expert-id contract, different kernel family.

`y` is **not** consumed by init-routing. See `moe_chain_contract.md`.

No in-tree example or ST chains GatingTopK → InitRoutingV3.

---

## 8. Relation to existing CATLASS Cases

**unrelated** to 43 / 60 / 66 / 68 / 53 / 73. No matmul, no `groupList`, no MX group-32 GEMM.

Shared with later routing docs only as the source of `expert_idx` / gate weights.

---

## 9. Test coverage

| live variant | coverage |
| --- | --- |
| 950 apt keys `10000`–`10004` | host UT `tests/ut/op_host/test_moe_gating_top_k_tiling_arch35.cpp` |
| 910 keys 0/1/2 | host UT `test_moe_gating_top_k_tiling.cpp`; kernel UT `tests/ut/op_kernel/test_moe_gating_top_k.cpp` |
| aclnn | ST `tests/st/aclnnMoeGatingTopK/`; example `examples/test_aclnn_moe_gating_top_k.cpp` (standalone, dummy tensors) |

No joint UT with init-routing or finalize.

---

## 10. Evidence conflicts

1. **Documented upstream of init-routing is SoftmaxV2, not this op** (`design-doc` op-list + init README vs Scout picking `moe_gating_top_k`). API tensors still match on `expert_idx`. Do not collapse the two ops.
2. README Step 4 says “这里只需要 expertIdxOut” while the API still requires `y`. `y` is the weight tensor, produced in Step 5. `design-doc` wording vs actual outputs.
3. Hash mode exists only on 950 `OpDef` (`input_ids` / `tid2eid`). A2 `OpDef` has no those inputs.
