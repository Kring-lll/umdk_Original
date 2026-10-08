# P2 close-read: vLLM MoE / TP-RS vs B / C / D / MC2 Knowledge

P1 indexer **paused** (canonical-instance gate not closed). This round is absorb verification only.

**Not a Case. Not Knowledge. Do not reopen A/B/C/D or MC2 teaching.**

Source: `vllm-project/vllm-ascend` `main` @ `d8268faea90dd9b999a0a82f6d133751d57b5770`.

```yaml
status: close-read-written
review: passed
scope: framework-path-absorb
not: third-MC2-case
not: new-moe-case
not: eplb-as-case
```

Companion verdict: [p2_moe_mc2_absorb_verdict.md](p2_moe_mc2_absorb_verdict.md).

Live teaching (read, do not edit):

- B `cases/compute-communication/moe/ascend950-moe-ep-dispatch-combine/`
- C `cases/compute/grouped-gemm/ascend950-gmm-swiglu-mx-quant/`
- D `cases/compute-communication/moe/ascend950-mega-moe-fused-ep-ffn/`
- A `cases/compute/moe/ascend950-moe-local-permutation/`
- Layer 1 `knowledge/engineering-experience/moe-token-expert-rank-data-contracts.md`
- MC2 Knowledge `knowledge/engineering-experience/mc2-compute-collective-coupling.md`

---

## Name trap (keep in front)

```text
vLLM MoECommType.MC2
    ≠  MC2 Knowledge (matmul × collective AG/RS/AR/A2A)

MoECommType.MC2  =  EP distribute dispatch/combine
                    → Case B **data contract** at the framework boundary
                      (window/protocol identity not re-verified)
npu_mm_reduce_scatter_base  =  TP linear **MM→RS semantic job**
                    → fill MC2 四问; **not** M1 kernel morphology this round
```

A5 EP selector (`_select_a5_moe_comm_method`) never returns `FUSED_MC2`. **Eligible default** when `tokens ≤ mc2_capacity` and `world_size > 1` is `MoECommType.MC2`. Not an unconditional A5 default.

---

## Path B — `MoECommType.MC2` (A5 eligible default EP path)

```text
AscendMoERunner
     ↓
MC2CommImpl.fused_experts
     ↓
TokenDispatcherWithMC2.token_dispatch
     torch_npu.npu_moe_distribute_dispatch(_v2)
     x=token-major hidden
     expert_ids=topk_ids
     ↓
expand_x, assist_info_for_combine, expert_token_nums, ep_recv_counts
     ↓
unified_apply_mlp   # GMM, preserve recv row order
     ↓
token_combine
     torch_npu.npu_moe_distribute_combine(_v2)
     assist_info_for_combine  (v2) / expand_idx (v1)
     expert_scales=topk_weights
     ↓
token-major y
```

This is Case B’s **API contract** on a framework path:

| B teaching | vLLM MC2 path |
| --- | --- |
| token-major `x` + `expert_ids` | `hidden_states` + `topk_ids` |
| Dispatch packs; not InitRouting | no `npu_moe_init_routing*` on this path |
| `expand_x` local-expert-major | `expand_x` |
| `assist_info` for Combine | `assist_info_for_combine` |
| `expert_token_nums` after comm | `expert_token_nums` → MLP `group_list` |
| Combine weighted restore | combine + `expert_scales` |
| no Finalize after EP | `finalize()` does not call `npu_moe_finalize_routing` |

Python / HCCL group name / `global_bs` / A5 extra kwargs (`expert_scales`, `y_dtype`, MX `quant_mode=4`) are **packaging**.

```text
Same Case-B-shaped EP data contract / routing loop at the framework boundary.
Underlying window/protocol identity is not re-verified in this P2 close-read.
```

同属 Case B 的 EP dispatch/combine 数据合同；本轮不重新证明底层仍是 Case B 的 MTE dest-push 实现。

`API contract aligned` ≠ `kernel mechanism identity proved`.

`expert_token_nums_type`: count for W4A8 GMM-SwiGLU v2, else cumsum. Case C already taught count vs cumsum; `cumsum_group_list` in `moe_mlp.py` is that conversion, not a new map.

---

## Path C — expert FFN after dispatch

```text
expand_x + group_list(=expert_token_nums)
     ↓
A5 DeviceOperator.npu_grouped_matmul_swiglu_quant
     → torch_npu.npu_grouped_matmul_swiglu_quant_v2     # default
     → or npu_grouped_matmul + SwiGLU / MX variants
     ↓
GMM2  DeviceOperator.npu_grouped_matmul_gmm2
     → torch_npu.npu_grouped_matmul
```

- M not reordered (Dispatch recv order).
- GMM1+SwiGLU+requant = Case C job; down-proj = 60, not a fourth card.
- A5 MX lives in the adaptor (`npu_dynamic_mx_quant`, MX GMM kwargs) = 53 extension, not a new GEMM Case.
- `_C_ascend.grouped_matmul_swiglu_quant_weight_nz*` is Base / `enable_custom_op` gated. A5 default GMM1 is **torch_npu v2**.

In-tree `csrc/gmm/grouped_matmul_swiglu_quant_v2/` is a compile surface. Framework live call on A5 is torch_npu. Do not open a Case from the extra binding.

---

## Path D — `FUSED_MC2` (not A5 default)

A3 selector may pick `FUSED_MC2` when `enable_fused_mc2==1`. A5 selector does **not**.

If that type is on:

```text
cann_ops_transformer present?
  yes → FusedMC2CommImpl._apply_cann_mega_moe
        token-major x + topk_ids + weights → token-major y
        = Case D alternative (importable MegaMoe)
  no  → torch.ops._C_ascend.dispatch_ffn_combine
        Scout: A2/A3 only; 950 skip
```

Layer 2 (what is internalized, metadata, barriers) still classifies MegaMoe. No third fused Case. Do not treat vLLM’s fused-MC2 **name** as matmul-collective M2.

---

## Path A packaging — `ALLGATHER` (no EP, or A5 fallback)

`AllGatherCommImpl` uses `npu_moe_init_routing_v2` then `npu_moe_token_unpermute`. Comment: they **avoid** `npu_moe_finalize_routing` (accuracy). That is Case A’s local permute maps (`expanded_row_idx`), not B’s `assist_info`, not Finalize-Routing as a new family.

When EP is off (`ep_size==1`), this is the expected single-rank loop.

---

## Path ALLTOALL — HCCL A2A + permute (A5 overflow / LoRA+EP)

`TokenDispatcherWithAll2AllV`: local permute → `async_all_to_all` → expert-major → MLP → reverse A2A → unpermute.

Still Layer 1 (token-major ↔ expert-major + `group_list`). Communication is generic HCCL AllToAll, **not** B’s MTE window / `assist_info`. Same class of absorb as sgl DeepEP split: **packaging**, not a new routing-loop Case.

---

## TP `npu_mm_reduce_scatter_base` — MC2 Knowledge, not MoE

```text
RowParallel linear  (flash_comm_v1 + mmrs_fusion)
     ↓
DeviceOperator.npu_mm_reduce_scatter_base
     A5 default comm_mode="ai_cpu"
     HCCL_OP_EXPANSION_MODE=CCU_SCHED → "ccu"
     Base/A2-A3 default "aiv"
     ↓
torch_npu.npu_mm_reduce_scatter_base
```

Four questions at **framework** granularity (kernel internals stay on the RS Case; do not reopen ops-transformer):

1. **ownership direction** — semantic MM → RS. Same **direction** as the M1 RS Case. vLLM Python does not expose per-tile handle transfer.
2. **progress / completion** — encapsulated inside the torch_npu op. Not visible this round. Framework sees one call / one tensor out.
3. **local readiness / remote completion** — encapsulated. Not visible this round.
4. **transform** — no extra transform stage at the Python call boundary.

MC2 Knowledge is for **asking** these questions and not confusing job with mechanism. This round only proves the **RS job / semantic direction**. It does **not** re-prove M1 implementation morphology (ASW MM chunk → local readiness → native RS handle → deferred Wait).

`comm_mode` ai_cpu / aiv / ccu: no new **framework-visible** coupling. Underlying mechanism is not re-verified, so a `comm_mode` change is **not** a new family. **No third MC2 Case.**

If `mmrs_fusion` is off: separate matmul + `reduce_scatter`. Still not a Case.

---

## Explicitly not production EP

- `npu_moe_gating_top_k` — gating, not a loop Case.
- Python EPLB — scheduling; deferred; no dispatch-contract change observed.
- `grouped_matmul_finalize_routing` — still absent as a named vLLM path.
- InitRouting → Dispatch as one chain — **false** on A5 MC2.

---

## What this file does not do

- Re-read MegaMoe / Dispatch kernel.
- Declare A5 default serving uses MegaMoe.
- Equate `MoECommType.MC2` with RS/AG/AR.
- Start P3 surfaces.
