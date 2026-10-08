# Verdict: vLLM P2 MoE / MC2 absorb

Contrast after Scout. **Not a Case. Not Knowledge.**

证据：[p2_moe_mc2_absorb.md](p2_moe_mc2_absorb.md)

```yaml
status: verdict-written
review: passed
result: A                         # 已有资产吸收
new_case: none
new_knowledge: none
absorb:
  - cases/compute-communication/moe/ascend950-moe-ep-dispatch-combine/   # B contract alternative
  - cases/compute/grouped-gemm/ascend950-gmm-swiglu-mx-quant/            # C
  - cases/compute-communication/moe/ascend950-mega-moe-fused-ep-ffn/     # D if FUSED_MC2
  - cases/compute/moe/ascend950-moe-local-permutation/                   # A (ALLGATHER)
  - cases/compute/grouped-gemm/ascend950-grouped-matmul-slice-m/         # 60 GMM2
  - cases/compute/gemm/ascend950-fp8-mx-matmul/                          # 53 MX
  - knowledge/engineering-experience/moe-token-expert-rank-data-contracts.md
  - knowledge/engineering-experience/mc2-compute-collective-coupling.md  # 四问；job ≠ mechanism
not:
  - third MC2 Case
  - MoECommType.MC2 as matmul-collective
  - EPLB Case
  - MTE dest-push identity re-proved from framework calls
  - M1 RS kernel morphology re-proved from npu_mm_reduce_scatter_base
```

---

## 结果

**结果 A。** vLLM framework path 没有新的 MoE loop / GMM family / MC2 coupling。

A5 **eligible default** EP path（`tokens ≤ mc2_capacity` **and** `world_size > 1`）：

```text
Python Buffer
  → torch_npu.npu_moe_distribute_dispatch(_v2)
  → GMM1+SwiGLU (C) + GMM2 (60)
  → torch_npu.npu_moe_distribute_combine(_v2)
```

= Case B **dispatch/combine 数据合同** + Case C / 60。Layer 1 坐标系够用。  
本轮 **不** 从 framework call 证明底层仍是 B 的 MTE dest-push kernel。

---

## 分项

| vLLM path | absorb | new Case? |
| --- | --- | --- |
| `MoECommType.MC2` dispatch/combine | **B contract alternative / framework packaging** | no |
| A5 GMM SwiGLU v2 + GMM2 | **C + 60**（MX → 53） | no |
| `FUSED_MC2` + `cann_ops_transformer.mega_moe` | **D alternative**（A5 selector 不选） | no |
| `dispatch_ffn_combine` | A2/A3 fused; 950 skip | no |
| `ALLGATHER` init_routing + unpermute | **A alternative** | no |
| `ALLTOALL` permute + HCCL A2A | Layer 1 packaging（非 B 的 MTE window） | no |
| `npu_mm_reduce_scatter_base` | MC2 Knowledge 四问；**MM→RS semantic/job**；M1 morphology 未重证 | **no** |
| EPLB Python | deferred scheduling | no |

---

## 为什么不是结果 B

结果 B 要的是：framework 上出现现有卡 **吸收不了** 的 map / loop / coupling。

没有：

- 新的 EP 坐标系（eligible A5 MC2 仍是 token-major + `expert_ids` → `assist_info` + `expert_token_nums`）
- 新的 GMM 切 M 规则（`group_list` 仍来自 Dispatch counts/cumsum）
- 新的 **framework-visible** matmul–collective 家族（TP MMRS 只证明 MM→RS job/semantic direction；`comm_mode` 改变也不开新 family）
- 把 MoE 的 “MC2” 名字变成 AG/RS/AR

ALLTOALL 用 HCCL AllToAll 而不是 B 的 window，仍是 Layer 1 换包装，和 DeepEP split 同一类 absorb，不开卡。

---

## A5 eligible default vs 名字

```text
A5 EP, tokens ≤ mc2_capacity, world_size > 1
        → MoECommType.MC2          # B contract
A5 不走 _select_a3 的 FUSED_MC2
        → 不要把 MegaMoe 写成 950 默认 serving

MoECommType.MC2
    ≠
Matmul × collective MC2 Knowledge
```

---

## 不要做

- 开第三张 MC2 Case / 第二张 B/C/D。
- 写第三份 MoE Knowledge。
- 把 `MoECommType.MC2` 并进 MC2 Knowledge。
- 从 `torch_npu.npu_moe_distribute_*` 自动写成 “same dest-push kernel”。
- 从 `npu_mm_reduce_scatter_base` 自动写成 “same M1 RS morphology”。
- 回改 ops-transformer / RS / M2 卡。
- 用 P2 重开 P1 indexer Case。

---

## Status

```text
P2 framework-path classification   ✅
P2 B/C/60/53 absorb                ✅
P2 FUSED_MC2 → D filtering         ✅
P2 ALLGATHER / ALLTOALL filtering  ✅
P2 name trap                       ✅
P2 no new Case / Knowledge         ✅
P2 MUST-FIX (contract ≠ identity)  ✅
P2 verdict                         ✅ PASSED / FREEZE

P1 indexer                         ⏸
P3 custom-op surfaces              🔜
```
