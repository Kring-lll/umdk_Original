# CANNBot Knowledge promotion plan

本文件只决定 **归位**。不写 Knowledge 正文，不改现有 Skill，不开 Case。

```text
CANNBot
  → inventory（有哪些知识型内容）
  → gap map（和现有 7 篇 Knowledge 是否重复）
  → 本文件：
       knowledge/   |  skills/  |  workflows/  |  none
```

Provenance 纪律（本阶段强制）：

```text
CANNBot says X
    ↓
建议 / 通常更快 / 最佳实践  → recommendation，可进 Knowledge，不得写成硬件事实
API 参数合法性 / 对齐       → 能追到 asc-devkit 或官方 API 再标 official-document
buffer 容量 / 核数          → 对 ini 或 PlatformAscendC 再标 official-document
只在 cannbot 里出现的通路声称（如 L1→GM 已删除）→ 本轮不 promote 为 fact
```

CANNBot 发现知识点；**一手证据才是硬事实真源**。

---

## 0. 阶段定义

这是 **Knowledge Enrichment**，不是 Scout Source 5。

- 禁止从 CANNBot 开新 Case
- 禁止把 `SKILL.md` 整份搬进 `knowledge/`
- 禁止一篇 `ascendc-api-best-practices.md` 装所有 API
- 现有 13 个 Case 只当 tiling 文的 worked examples

建议顺序（与用户一致，且 **第一篇就是 tiling**）：

```text
P1a  tiling-from-shape          ← 先写
P1b  ascend950 memory hierarchy
P1c  API 短 Card（copy / pipe / buffer / repeat）
P2   operator-debug Skill 骨架 + 少量 pitfall Knowledge
```

---

## 1. Promote → `knowledge/`

### 1.1 P1a — 第一篇，也是本阶段主产物

**文件（建议）**

`knowledge/engineering-experience/tiling-from-shape.md`

**为什么进 engineering-experience 而不是 hardware**

教的是决策顺序，不是芯片手册。硬件数字链到 1.2。

**必须回答**（第一次设计顺序，不是不可回头的流水）

```text
problem shape
    ↓
identify semantic axes
  elementwise → equivalent axes may flatten
  reduction   → mark A/R; merge only adjacent same-role axes
  broadcast   → align ranks; mark broadcast / stride=0
  layout      → keep physically contiguous / stride-bearing axes
    ↓
collapse only semantics-preserving axes
    ↓
proposal: core split + on-chip tile + buffer lifetime
    ↓
constraint check: capacity / alignment / API legality / pipeline
    ↓
revise split / tile / buffer     ← 装不下、核不满、padding 膨胀、
                                   double-buffer 超预算、repeat 超限都回到这里
```

文章里把循环叫 **proposal → constraint check → revise**。不要让 Agent 以为一路往下填参数即可。

**从 CANNBot 抽什么**

- 四要素清单（`SKILL.md` 通用设计要素）
- elewise：展平、former/tail、ubFormer
- reduce：合轴、AR/ARA、FullLoad vs Split
- broadcast：`ubSplitAxis`、外层分核

**明确标 recommendation（不要升级成 fact）**

- 每核 ≥4KB
- `blockFormer` 对齐 512 元素
- 核不够就减小 tile

**worked examples（已有 Case，不新开 Case）**

| 通用步骤 | 已有 concrete | 不要读成 |
| --- | --- | --- |
| 选切哪根轴 | 43 MN+K | Vector 也按 MN 切 |
| 核不够 | 66/68 | 普通 Reduce 要上 StreamK |
| 全载 vs 分载 | 73 full-K A | Vector FullLoad 等于 L1 full-A |
| grouped 维 | 60 | groupList 契约 |
| 通信 chunk | MC2 Cases | tiling-design 能解释 overlap |

**不写进这篇**

- FA/MoE/MC2 机制（已有 Knowledge）
- A2 Matmul 高阶 API tiling
- 具体 `TilingData` 字段抄表

**消费方**：`skills/operator-development/` 在“Design Parallelism / Memory”之前 **必须读**。

---

### 1.2 P1b — 950 硬件，短、可核对

**文件（建议）**

- `knowledge/hardware/ascend950/core-and-memory-hierarchy.md`

先一篇，不要拆 10 个空文件。

**可 promote（核对后标 official-document）**

- 950 = DAV_3510 = arch35；A2/A3 = DAV_2201 = arch22
- 运行时 `GetCurNpuArch` / `GetCoreMemSize`，禁止硬编码
- 核对后的典型值：L1 512KB、UB 248KB、L0A/B 64KB、L0C 256KB、AIC:AIV=1:2
- 含义：tile 预算用运行时值；Case / ArchTag 常数是编译期资源假设，不是 live 平台

**本轮不 promote 为 fact**

- L1→GM / GM→L0 通路删除
- 1.4/1.6 TB/s、54T 双发
- Kirin / 全代际表
- SIMT 128KB register 等经验值

**核对动作（写文时做，本 plan 不假装已做）**

1. 读本机 `${ASCEND_HOME_PATH}/*/data/platform_config` 里 DAV_3510 ini  
2. 对照 `platform_ascendc.h` 枚举  
3. 对不上就写 unknown，不抄 cannbot 表

---

### 1.3 P1c — API 短 Card

目录已经在。按检索粒度拆，不按 cannbot 文件名拆。

| 建议文件 | 吸收 | 不吸收 |
| --- | --- | --- |
| `knowledge/ascendc-api/data-movement/memory-copy-and-alignment.md` | DataCopy vs Pad、32B、Get/SetValue 作生产搬运的 **recommendation**、Ext params（recommendation） | stride 百科、所有示例算子；不要写“Get/SetValue 禁止使用” |
| `knowledge/ascendc-api/synchronization/pipeline-events-and-ordering.md` | 异步 DMA；EnQue/DeQue vs PipeBarrier；CopyIn/Compute/CopyOut | 完整 double-buffer 模板长文（可链 tiling 文） |
| `knowledge/ascendc-api/data-movement/ub-tque-tbuf-and-double-buffer.md` | TQue vs TBuf；`num`≠`depth` | Cube `A1/A2/B1` 全表（GEMM Case 已有） |
| `knowledge/ascendc-api/vector/mask-repeat-and-kernel-limits.md` | repeat≤255；禁 std:: / new；Compare 256B | 每个算术 API 说明书 |
| （可选稍后）`vector/cast-and-mixed-precision.md` | Cast、半精度升精度 | 与 elewise tiling 重复的 UB 公式 |

`api-matmul.md` / `api-gmm.md` / `api-gathermask.md`：**none**（2201）。在 copy 或 hierarchy 文用一行负例：“950 MatMul 不走这篇”。

LoadData2D / MX：等有人写非 CATLASS Cube 再补。53 已够 MX layout。

---

### 1.4 可附在 Knowledge 的 P2 碎片（短，或并进 1.3）

不要为 debug 各开长文。

| 碎片 | 放哪 |
| --- | --- |
| 全 0 / 随机优先查同步与对齐，不是公式 | `pipeline-events-and-ordering.md` 末尾 Implications |
| 507035 → 访存/buffer **假说**（再对 plog / 地址 / copy shape） | debug Skill，不要写成确定映射 |
| Alloc/Free、EnQue/DeQue 必须成对 | pipeline 文 + crash Skill |

容差数字：等读 `ops-precision-standard` 再写；现在不要把 cannbot 缺省 rtol 写成标准。

---

## 2. Promote → `skills/` / `workflows/`

现有 `skills/operator-debug/SKILL.md` 是空的。P2 三份 Skill 的主体放这里，**不要**先填进 Knowledge。

**建议（仍不在本 commit 改 Skill 正文，只定方向）**

`skills/operator-debug/SKILL.md` 做成路由器：

```text
编译/环境     → runtime-error 前两支（本轮不单开 env Skill）
返回码非 0    → runtime-error（161 / 361 / 507035 / 561 只是假说入口）
输出错且跑完  → wrong-result（precision/dtype 只是其中一族）
hang          → hang
crash         → crash
```

细则留 `skills/operator-debug/references/` 或 `workflows/correctness-debug/`。

**必须留下的 Decision Rules（从 CANNBot 抽，改写成 Weaver 口气）**

```text
MUST 固定最小复现后再改
MUST NOT 凭直觉改
IF 输出不变 THEN 清 cache
IF 连续猜测修改没有新信息 THEN 停试错，改二分 / Dump / 打点
   （cannbot 的“>7”是 heuristic，不保留神奇数字）
IF 全 0/随机 THEN 先把同步与对齐当作优先假说，不是唯一原因
主 Agent 蓝区无 NPU：禁止要求 npu-smi 通过才能设计
  （这是对 cannbot 直调 Step1 的改写，见 cannbot_analysis.md §8.5）
```

`workflows/new-operator/`：只加“读 tiling-from-shape，填四要素清单”。不要搬 7 Agent。

`operator-optimization`：本轮 **不** 从这六个 Skill 填充。等下一轮 `ascendc-performance-best-practices`。

---

## 3. → `none`

| 内容 | 原因 |
| --- | --- |
| 把六个 SKILL.md 拷进 Weaver | 混了 A/B/C；License；平台默认偏 A2 |
| 从 CANNBot 开 Case | 本阶段禁止 |
| FA cannbot 设计树并进 FA Knowledge | 已有更高证据的文 |
| A2 Matmul/GMM/GatherMask 教程 | 教错 950 Agent |
| Kirin / 310 全家桶 | 范围外 |
| 带宽/算力宣传数字 | 无一手实测 |
| 未核对的通路删除声称 | 硬事实门槛 |
| DumpTensor / mssanitizer / parse_plog 脚本 | 工具，不是 Knowledge |
| Architect/Developer/Reviewer | orchestration |
| Random/Conv 规划中章节 | 空 |
| `ascendc-task-focus` | 本轮未扫；且当前 cannbot `ops/` 可能未发布 |

---

## 4. 与现有 7 篇 Knowledge 的边界（写新文时必须遵守）

新文开头用和现有文一样的句式：先问什么，不覆盖什么。

| 新文 | 可以链 | 禁止并进去 |
| --- | --- | --- |
| tiling-from-shape | 43/60/66/68/73 当例子 | GEMM K 选择树、FA KV policy、MC2 chunk |
| hierarchy | CanImplement（硬编码反例） | 73 的 L1A 公式全文 |
| pipeline/API | precision Skill | MoE materialization、MC2 visibility |
| operator-debug | delivery 文（561003 ≠ 选路） | vLLM custom op 表面 |

---

## 5. 建议的落地批次（仍先等人点头再写正文）

**Batch 1** ✅ complete。`knowledge/engineering-experience/tiling-from-shape.md` 为 `status: accepted`。

1. tiling-from-shape（语义轴规范化；proposal → check → revise；Host contract 可派生；split ≠ partial）
2. 在 `operator-development` **不改结构** 的前提下注明：并行/Memory 节应指向这篇（真正改 Skill 另开任务）

**Batch 2** ✅ complete。平台资源 → tiling 回传 → 搬运/对齐 → ordering → UB storage → Vector API limits。不再横向补 API Card，除非 Skill 使用时出现明确缺口。

3. `knowledge/hardware/ascend950/core-and-memory-hierarchy.md` — **`accepted`**
4. `knowledge/ascendc-api/data-movement/memory-copy-and-alignment.md` — **`accepted`**
5. `knowledge/ascendc-api/synchronization/pipeline-events-and-ordering.md` — **`accepted`**
6. `knowledge/ascendc-api/data-movement/ub-tque-tbuf-and-double-buffer.md` — **`accepted`**
7. `knowledge/ascendc-api/vector/mask-repeat-and-kernel-limits.md` — **`accepted`**

**Batch 3** ✅ complete。P2 补“写坏以后怎么有纪律地定位”；四个 `references/*.md` **按需**，现在不补。

8. `sources/scans/cannbot-skills/operator_debug_contract.md` — **`accepted`**
9. `sources/scans/cannbot-skills/operator_debug_promotion_verdict.md` — **`accepted`**
10. `skills/operator-debug/SKILL.md` — **`accepted`**
11. `workflows/correctness-debug/README.md` — **`accepted`**

**Batch 4**  audit / design。不再横向补 API Card。

P0  `ascendc-performance-best-practices` + `ascendc-perf-optimize` → 通用性能判断（服务 `operator-optimization`）
12. `sources/scans/cannbot-skills/performance_judgment_contract.md` — **`accepted`**
13. `sources/scans/cannbot-skills/performance_promotion_verdict.md` — **`accepted`**
14. `skills/operator-optimization/SKILL.md` — **`accepted`**（不另开 `perf-judgment.md`）

P1  `ops-precision-standard` → 只做 correctness tolerance 真源，不扩 precision-debug  
15. `sources/scans/cannbot-skills/tolerance_source_contract.md` — **`accepted`**
16. `sources/scans/cannbot-skills/tolerance_promotion_verdict.md` — **`accepted`**
17. `knowledge/engineering-experience/tolerance-source.md` — **`accepted`**

P2  `ascendc-docs-search` → retrieval / evidence，不进 Knowledge 正文  
18. `sources/scans/cannbot-skills/docs_search_contract.md` — **`accepted`**
19. `sources/scans/cannbot-skills/docs_search_promotion_verdict.md` — **`accepted`**
20. `skills/docs-search/SKILL.md` — **`accepted`**（薄检索；不是改代码门禁）

---

## 6. 完成标准（写完 Batch 1–2 后应能做到）

Agent 面对 **Add / ReduceSum / 简单 Broadcast** 时，不打开 MoE/FA/GEMM Case 也能：

1. 判断算子族  
2. 写出核怎么分、tile 多大、UB 里几块 buffer  
3. 选 DataCopyPad 还是 DataCopy  
4. 在 Compute 前同步  
5. 处理 tail  
6. 承认：没上板就不报加速比  

这就是本阶段相对前四个 source 的差别：前四个给 **高级机制的 concrete**；CANNBot 给 **普通 kernel 的系统设计**。
