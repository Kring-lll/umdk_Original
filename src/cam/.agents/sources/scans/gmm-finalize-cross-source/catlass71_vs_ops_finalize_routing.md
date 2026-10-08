# CATLASS 71 ↔ ops-transformer GMM Finalize-Routing

Focused contrast. **Not a Case.** 默认不新开卡。

Question this file must answer:

```text
名字里的 FinalizeRouting
到底 finalize 什么、消费哪张 map、
属于哪条 routing loop、
GMM 与 restore 的耦合是不是新机制？
还是 60 + A 的融合实现（C 那种 MIX epilogue）？
```

```yaml
status: contrast-accepted
review: passed
new_case: none
new_knowledge: none
sources:
  catlass_71:
    registry_ref: catlass
    commit: a8b4fffbd9cabc021dfd47253e907a1cde59de25
    example: examples/71_ascend950_fp8_mx_grouped_matmul_finalize_routing/
    design: 71_ascend950_fp8_mx_grouped_matmul_finalize_routing.md
    kernel: include/catlass/gemm/kernel/grouped_mx_matmul_finalize_routing_tla.hpp
    epilogue: include/catlass/epilogue/block/block_epilogue_finalize_routing.hpp
    sibling_not_this_file: no_deter ASWT schedule  # 同 math，另一套 swizzle
  ops_gmm_fr:
    registry_ref: ops-transformer
    commit: 9311b63bcd065b9779eb08a7d634e9463ee41ef1
    scan_path: /home/wangyibo/ops-transformer_bak
    op: gmm/grouped_matmul_finalize_routing/
    canonical_950: DAV_3510 + aclnnGroupedMatmulFinalizeRoutingV3 MX
    kernel: op_kernel/arch35/grouped_matmul_finalize_routing.h
      # KernelGmmFinalizeRouting + BlockEpilogueFinalizeRouting (cgmct)
    not_950_sot: aclnn V2 docs 写 950 ×；A2/A3 int8 MIX
  unfused_local:
    case: cases/compute/moe/ascend950-moe-local-permutation/   # Case A
    op: moe/moe_finalize_routing_v2/                          # 不是本对照的 ops 侧
```

Do **not** reopen GEMM / MoE / MC2 / Attention. Do **not** 读 M3 / MLA.  
**不要**把 Case B EP Combine 写进这条。

---

## Live paths

### CATLASS 71

950 example：MX FP8 Grouped Matmul + AIV finalize。确定性版 `ColumnBlockSwizzle`；`no_deter` 只换 ASWT / AIV 切 M，**数学相同**，本对照不展开。

```text
AIC  MX GMM  →  GM workspace C (expert-major rows)
AIV  Clear out
  → optional SharedInput add
  → C[p,:] *= logit[p]
  → out[rowIndex[p],:] += Ĉ[p,:]     # SetAtomicAdd
```

Flag 6 AIC→AIV、flag 4 AIV→AIC。`communication_pattern: none`。

### ops `grouped_matmul_finalize_routing`

950 live：`OpDef` `AddConfig("ascend950")`；Host `npuArch==DAV_3510` 走 arch35 tiling；kernel `grouped_matmul_finalize_routing_apt.cpp` → `KERNEL_TYPE_MIX_AIC_1_2`。

Canonical：**V3 MX**（文档 950 √）。V2 文档写 950 ×，不要当 950 SoT。没有 `config/ascend950/*.json`，不等于没有 950 kernel。

Cube：`cgmct` `KernelGmmFinalizeRouting` + `group_list`（type 0 prefix-sum / type 1 count，与 60/C 同契约）。  
AIV：`BlockEpilogueFinalizeRouting`：L0C→UB，`logit[p] * C[p]`，`rowIndex[p]` 原子加到 `y`。Prologue 可写 `shared_input`。

没有 HCCL。不是 EP。

### 对照用、但不是本 fused op

Case A `MoeFinalizeRoutingV2`：已经算完的 expert-major `expanded_x` + `expanded_row_idx`，纯 AIV 加权还原。**名字像，map 不是同一套。**

---

## 1. Finalize 到底 finalize 什么？

**加权 scatter-add，把 expert-major GMM 行还原成 token-major `y`。**

| | 71 / ops fused | Case A FinalizeV2 |
| --- | --- | --- |
| GMM row order | Cube 按 `group_list` 切 M；行序保持 expert-major | 不计算 GMM |
| token order | `out[rowIndex[p]]` 恢复 | `y[token]` 由 map gather 累加 |
| top-k weighted reduce | `logit[p] * C[p]` 再原子加；同一 token 多行相加 | `scales[token,k] * expanded_x[slot]` |
| quant / dequant | MX / pertoken 在 **GMM 侧**（53/C） | Finalize 只吃 fp16/fp32/bf16 |
| epilogue fusion | 是：GMM + restore 一个 MIX kernel | 否：独立 Vector op |

不是“只把 epilogue 贴在 GMM 上、行序不变”。Case C 的 SwiGLU **保持** expert-major M；这里 **改** 行坐标。

也不是 EP Combine。没有 window / `assist_info`。

---

## 2. routing map 是哪一种 lifetime？

**`row_index[p]` = GMM 第 p 行（expert-major slot）要加到哪个 token 行。**

```text
p  ∈ expert-major rows   （与 group_list 切出来的 C 行对齐）
rowIndex[p] → token row of y
logit[p]    → 这一 slot 的 gate 权重
```

这接近 Init 的 **SCATTER** 方向（slot → original），**不是** Case A Finalize 的 gather 编码：

```text
Case A Finalize:
  offset = token + k*T  或  token*K + k     # (token, k) 展平
  slot   = expanded_row_idx[offset]         # 值 = expert-major 行
  y[token] += scales[token,k] * expanded_x[slot]
```

同名 `FinalizeRouting` **不能**默认接 `expanded_row_idx`。要看：map 下标是 slot 还是 (token,k)，值是 token 还是 slot。

`group_list` 不是这张 restore map。它只切 GMM 的 M，和 60 一样。

---

## 3. 属于哪条 routing loop？

**Case A 的 local permutation loop，compute-side GMM epilogue。不是 Case B。**

```text
token-major hidden
    ↓
local routing / permutation producer
    ↓
expert-major x
+ group_list
+ slot→token row_index
+ aligned logit
    ↓
GMM + weighted scatter-add
    ↓
token-major y
```

属于 Case A 所代表的 **local permutation loop**，但不要因此推导 `MoeInitRoutingV3` 的 `expanded_row_idx` 可直接接线；具体 map 编码不同。本 fused op 不包含那段 Init。

EP loop（B）：Dispatch window → GMM（C 保持 recv 行序）→ Combine。Combine 已经 unpermute；EP **不**再接 Finalize。本 fused op 没有 MTE window、没有 rank。

纯 compute epilogue：是。Cube 产出仍按 expert 分组；AIV 才做 restore。

---

## 4. GMM 与 routing 的耦合点

```text
Cube grouped MM（expert-major C）
        ↓ same-device AIC→AIV
AIV logit * row + atomic scatter-add
        ↓
token-major y  （+ optional shared expert）
```

| split API handoff | fused 结局 |
| --- | --- |
| GMM `C` / Finalize `expanded_x` | **deleted** 作为可接线 API tensor。71：**relocated** 到 workspace GM。ops MX：L0C→UB，连完整 C workspace 都不必当 API |
| `group_list` | **keep**（GMM 切 M） |
| restore map / scales | **keep**，但字段是 `row_index` + `logit`，不是 `expanded_row_idx` + `scales[T,K]` |
| `y` | **keep** |

中间 expert-major C **对下一图节点不再可见**。设备上 71 仍有 workspace；问的是 API-visible handoff，用 Layer 2 的 deleted / relocated，不要写成“设备上没了这块数据”。

same-device：CrossCore flag（71：4/6；ops：FIXPIPE ↔ Vector）。不是 cross-rank Wait。

---

## 5. 相对 60 / A / C 的 teaching delta

| 已有卡 | 本 fused op 用了什么 | 是不是新机制 |
| --- | --- | --- |
| **60** | `group_list` 切 M 的 grouped Cube | 否 |
| **53 / C** | 950 MX `MmadMx`；C 的 MIX：Cube 产、AIV 消费 | MX / MIX 形态否；**epilogue 数学不同**（SwiGLU vs scatter-add） |
| **A** | local loop 上 expert-major → token-major 加权还原 | 数学同类；**map 编码不同**；A 不融合 GMM |

若开新卡，教的会是：

```text
60 的 GMM
  + A 的加权还原（换了一张 row_index）
  + C 的 Cube→AIV MIX 融合
  + 53 的 MX
```

那不是无法被 60/A/C 吸收的第四种 compute 机制。scatter-add epilogue 相对 C 的 SwiGLU 是 **另一份 fused epilogue 实现**，不是新 family。

71 与 ops fused：**cousins**（同 math、同 loop）。调度差（workspace tile vs L0C→UB；deter vs ASWT）不够开第二张卡，也不够相对 60/A/C 开第一张。

---

## 不要做

- 名字有 FinalizeRouting → 当成 Case A `MoeFinalizeRoutingV2`。
- 从 local loop 推导 `MoeInitRoutingV3.expanded_row_idx` 可直接喂本 op 的 `row_index`。
- 当成 Case B Combine / EP。
- 当成第二张 60、第二张 C、第四张 GEMM。
- 用 V2 文档的「950 ×」否掉 V3 MX 950 kernel；也不要把 A2 int8 当 950 SoT。
- 把 71 workspace / ops L0C 写成“融合删掉了设备上的 C”。
- 把 `group_list` 写成 restore map。
- 为 71 和 ops fused 各开一张卡。
- 现在写 Case 或 Knowledge。
