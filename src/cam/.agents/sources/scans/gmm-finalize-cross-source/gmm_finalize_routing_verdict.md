# Verdict: CATLASS 71 / ops GMM Finalize-Routing

Contrast 之后的分类。**Not a Case. Not Knowledge.**

证据：[catlass71_vs_ops_finalize_routing.md](catlass71_vs_ops_finalize_routing.md)

```yaml
status: verdict-accepted
review: passed
result: A                         # 已有资产吸收
new_case: none
new_knowledge: none
absorb:
  - cases/compute/grouped-gemm/ascend950-grouped-matmul-slice-m/          # 60
  - cases/compute/moe/ascend950-moe-local-permutation/                    # A
  - cases/compute/grouped-gemm/ascend950-gmm-swiglu-mx-quant/             # C MIX 形态
  - cases/compute/gemm/ascend950-fp8-mx-matmul/                           # 53 MX，不新开
canonical_ops:
  catlass: examples/71_ascend950_fp8_mx_grouped_matmul_finalize_routing/
  ops: gmm/grouped_matmul_finalize_routing/   # 950 = V3 MX DAV_3510
not:
  - moe/moe_finalize_routing_v2/              # Case A 未融合；map 不同
  - Case B Combine
```

---

## 结果

**结果 A。**

CATLASS 71 与 ops `grouped_matmul_finalize_routing`（950 V3 MX）是：

```text
existing grouped GEMM（60）
  + local-routing weighted restore（A 的 loop / 数学）
  + Cube→AIV MIX fusion（C 的形态，不是 C 的 SwiGLU 数学）
  + MX（53）
```

**不开 Case。** 作为 60 / A / C 的 **cross-source evidence**（cousins：71 example ↔ 生产 fused op）。

源码没有把这条抬成结果 B：没有新的 routing-loop / map-lifetime family 需要单独教学；`row_index[slot]=token` 是该 fused implementation 的另一种 restore-map encoding。materialization 是 Layer 2 已有的 deleted/relocated（GMM `C` 不再是 API），调度差（workspace vs L0C→UB）不够新卡。

---

## 五个问题的结论

1. **finalize 什么** — expert-major GMM 行 → `logit` 加权 → `row_index` scatter-add → token-major `y`。不是只融 epilogue 且保持行序，也不是 EP Combine。
2. **map lifetime** — `row_index[slot] = token`（+ 对齐的 `logit[slot]`）。**不是** Case A 的 `expanded_row_idx[(token,k)] = slot`。
3. **loop** — Case A 所代表的 **local** permutation 的 compute 侧。不是 B。**不是** `MoeInitRoutingV3.expanded_row_idx` 直连。
4. **耦合** — Cube 产 C，AIV restore；API 上的 GMM `C` / Finalize `expanded_x` **deleted**。71 workspace = relocated。
5. **delta** — 不够新卡。新句子只有：AIV epilogue 是 scatter-add 不是 SwiGLU；map 字段换了。

---

## 为什么不是结果 B

结果 B 要的是：pipeline 内 restore 的 materialization / scheduling **明显不同于** A/C。

不同的只是：

- 相对 A：GMM 被融进来；map 编码从 gather 换成 slot→token
- 相对 C：AIV 做 scatter-add 而不是 SwiGLU；输出改成 token-major

相同的是：60 的 `group_list` Cube、C 的 MIX producer/consumer、A 的单卡 restore、无通信。Agent 用现有四问（60 切 M？A 哪张 map？C 是否保持行序？）就能读这条 fused op。

---

## 名字陷阱（必须留在 Agent 面前）

```text
FinalizeRouting in the name
    ≠ Case A MoeFinalizeRoutingV2
```

先看消费什么 map、输入 row 是什么坐标、输出恢复到什么坐标。再看有没有 Cube。

---

## 不要做

- 开 GMM+finalize Case / 71 Case / 第二张 C。
- 写第三份 MoE Knowledge 或第二份 GEMM Knowledge。
- 把这条并进 Case B / MegaMoe。
- 继续榨 ops-transformer（M3 / 再扫 fused 名）。

ops-transformer **到此冻结**。下一阶段：换 source（cann-ops-adv / RecSDK 等），找 13 Case + 6 Knowledge **吸收不了** 的生产机制。
