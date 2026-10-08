# Performance promotion verdict

status: **accepted**

对照：

- CANNBot @ `c4c425641d11127945d22e5a752fcad77646be97`
  - `ops/ascendc-performance-best-practices/`（P0 主扫）
  - `ops/ascendc-perf-optimize/`（判断分层，辅扫）
- Weaver 模型：[performance_judgment_contract.md](performance_judgment_contract.md)
- 已 `accepted`：Batch 1–3 Knowledge / operator-debug / correctness-debug
- 已有高阶文：GEMM K、MC2、FA、MoE、CanImplement

本文件只决定 **抽什么判断纪律、丢掉什么模板**。Skill 按 [contract](performance_judgment_contract.md) 写短路由，不拷 `.template`。

```text
CANNBot says X
    ↓
    ├─ 已是 accepted Knowledge / Case  → link
    ├─ 通用判断 / 分层顺序              → Skill 路由
    ├─ 未核收益 / A2 数字 / 与 Card 冲突 → none 或 rewrite 后当 recommendation
    └─ 各族模板 / MicroAPI 全书         → none
```

```text
debug / perf heuristic
  cannot override
accepted API / hardware contract
```

---

## 总判

`ascendc-performance-best-practices` **不是** Weaver 缺的那块“通用性能判断”。它是：

```text
按算子族组织的优化百科
  + 必须拷贝的 Softmax / Transpose 模板
  + 若干带 μs 收益表的 design.md
```

Weaver 已经有“怎么写对”和“写坏了怎么查”。缺的是：

```text
正确性过了
  → 测量
  → 判断慢在搬运 / 流水重叠 / 核利用 / tile / Vector 指令 / 其他
  → 只改有证据的一处
```

更接近这个缺口的是 `ascendc-perf-optimize` 的 **分层提问**（切分 → 卡间若 MC2 → 核间若有同步 → 单核 bound），不是 best-practices 的族表。

```text
值得留
  先正确性再优化
  先测量再改
  卡间 / 核间有适用条件，普通 Vector 可跳过
  单核：movement vs compute vs exposed wait vs scalar
  stage active ≠ exposed latency；有 overlap 时 T_total ≠ Σ T_stage
  有模板的族：先看 Weaver Case，禁止凭空重写高级机制

必须改写
  “开 double buffer 吞吐 +20–50%”
  “GM 按 512B 对齐”
  “3510 UB = 256KB / bank 表”
  “查到模板必须整份拷进工程”
  四步流水当成所有算子的门禁仪式

不要搬
  reduce Softmax 五套 .template
  matmul pingpong/swat/fullload 长文（Case 已有路径）
  conversion / transpose 手册
  broadcast NDDMA 选型当 950 fact
  SIMT / Conv / 规划中 NN
  910B 192KB UB、DMA 效率表当 950 规格
```

三份东西不要合成一本 Weaver Skill：

```text
cannbot
  performance-best-practices  ──→  各族百科 / 模板     none（Case 已覆盖的链 Case）
  perf-optimize               ──→  分层判断             rewrite 成短路由
  ops-profiling               ──→  采集手段             有目录再打开，不进 Knowledge
```

---

## 和已有资产

| CANNBot | Weaver 已有 | 处理 |
| --- | --- | --- |
| matmul pingpong / streamk / fullload | 43/66/68/73 + [GEMM K](../../../knowledge/engineering-experience/gemm-k-partition-strategies.md) | **already-covered**。guide 当索引，不吸 design 长文 |
| MC2 pipeline balancing / local matmul | [MC2](../../../knowledge/engineering-experience/mc2-compute-collective-coupling.md) | **already-covered**。隔离测试纪律可 **complement** 进优化 Skill（有数据才谈 R） |
| FA online softmax 模板 | [FA](../../../knowledge/engineering-experience/fa-pipeline-online-softmax-and-paged.md) | **already-covered** |
| elewise `InitBuffer num=2` | [TQue](../../../knowledge/ascendc-api/data-movement/ub-tque-tbuf-and-double-buffer.md) | **rewrite**：eligibility，不是收益保证 |
| elewise Counter / 中间不落 GM | [mask-repeat](../../../knowledge/ascendc-api/vector/mask-repeat-and-kernel-limits.md) + pipeline | 判断可留；API 细节不新开 Card |
| common DataCopy 优化 | [copy](../../../knowledge/ascendc-api/data-movement/memory-copy-and-alignment.md) | **冲突风险**：512B、910B 带宽表、Pad=对齐手段。不 promote |
| common tail GatherMask | tiling + copy | 尾块是 tiling 回传；GatherMask 2201 教程 **none** |
| common UB resident / bank | TQue 生命周期；hierarchy 容量 | 常驻小参数 = **recommendation**。3510 bank 表 / 256KB **none**（与 248 KiB 冲突） |
| scalar 9 条编码原则 | none | 可作以后 `coding-guidelines` **recommendation**，不是 P0 |
| tiling fallback 脚本 | [tiling-from-shape](../../../knowledge/engineering-experience/tiling-from-shape.md) | **none** 脚本；判断回传到 revise |

---

## 按目录压缩

### 1. `ascendc-performance-best-practices`

| 内容 | 处理 | 去哪 |
| --- | --- | --- |
| 族表 + “未收录就说暂无” | **keep 口气** | Skill：没有 Case / Knowledge 就承认，不要编收益 |
| “有模板必须拷贝、保留 MicroAPI” | **rewrite** | 仅当 Weaver **已有**对应 Case/模板；禁止把 cannbot `.template` 拷进仓 |
| matmul / mc2 / reduce / conversion 全书 | **none** | 已有 Case 或超出普通 Vector |
| elewise double buffer 时间线 | **rewrite** | 重叠假说；数字全扔 |
| elewise vector efficiency | **keep 判断** | 中间结果留 UB、少碎 API；不写 Counter 教程 |
| broadcast 选型树 | **none 本轮** | NDDMA vs Pad 需一手 API；不覆盖 copy Card |
| common/ 三篇 | **几乎 none** | 与 accepted 合同冲突或 A2 经验 |
| scalar coding_principles | **later** | 不挡 P0 路由 |
| SIMT / 规划中 | **none** | |

入口把性能定义成“按族查 design.md / 拷模板”。Weaver 定义成“测量 → 分族 → 链已有文”。**不要用它的查询方式当 Skill 骨架。**

### 2. `ascendc-perf-optimize`

| 内容 | 处理 | 去哪 |
| --- | --- | --- |
| Step 1 tiling 理论建模 | **already** | tiling-from-shape；不要第二套 elewise/reduce 公式本 |
| Step 2 卡间（有通信才走） | **keep 适用条件** | 链 MC2；无 T_comm/T_compute 不猜掩盖率 |
| Step 3 核间（有核间同步才走） | **keep 适用条件** | 普通 elewise 跳过 |
| Step 4 单核 bound：scalar / memory / vec / no-bound | **rewrite** | 收成 contract 的瓶颈族；丢掉仿真图门禁 |
| MC2 MTE2 可能被通信等待污染 | **keep** | 指标 ≠ 根因 的样例 |
| 各族 tiling fallback py | **none** | |

四步是 **提问顺序**，不是每张工单都要交四份仿真报告。

### 3. 采集 / 打点（本轮只定边界）

| 来源 | 处理 |
| --- | --- |
| cannbot `ops-profiling` | 手段。有 PROF 目录再开，不进 Knowledge |
| Weaver `skills/instrumentation/` | 继续空着也行；判断 Skill 只声明“阶段 Cycle 从打点来” |
| 用户仓打点 Skill / profiling-analysis | **不搬进** AscendWeaver Knowledge |

---

## 归位

| 产物 | 本轮 | 下一动作 |
| --- | --- | --- |
| [performance_judgment_contract.md](performance_judgment_contract.md) | 判断模型 | **`accepted`** |
| 本文件 | 压缩裁决 | **`accepted`** |
| `skills/operator-optimization/SKILL.md` | 短路由 | 写 |
| 一篇 `knowledge/.../perf-judgment.md` | 仅当 Skill 仍太薄 | 默认先不写 |
| `workflows/performance-tuning/` | ⏳ | 正确性过 → 测 → 改 → 回归 → 再比；不重复 Skill |
| 各族模板 / 新 API Card | **none** | |
| P1 `ops-precision-standard` | 未开始 | 只做 tolerance 真源 |
| P2 `ascendc-docs-search` | 未开始 | retrieval，不进 Knowledge |

---

## 未来 Skill 允许留下的 Decision Rules

```text
MUST     正确性同一 contract 过了再谈性能
MUST     先测量（阶段或 kernel），再改
MUST     一次只改有证据支持的一处
MUST     改完先 correctness retest，再在同一 performance contract 下比较
         baseline identity A 与 candidate identity B
MUST NOT 把 active cycles 最大的阶段自动当成端到端瓶颈
MUST NOT 无测量报加速比
MUST NOT 用 cannbot 收益表 / A2 数字覆盖 accepted Card
MUST NOT 为普通 Vector 强制走卡间 / 核间两步
MUST NOT 把 MTE2 busy 在 MC2 上直接写成搬运问题

IF 只有“感觉慢”
THEN 只许静态假说 + 设计测量，Perf 标未测

IF 阶段显示 Copy active 高
THEN 先问它是否暴露在 critical path
THEN 暴露才把 movement 升权   （recommendation）

IF 三段几乎不重叠且 num=1
THEN overlap eligibility 升权；num=2 仍要回传 UB 预算

IF 核不满或尾核拖时间
THEN 回到 tiling revise，不要先改 Vector API

IF 无新测量还在换实现
THEN 停优化，回去补打点 / msprof
```

不保留：

```text
IF memory-bound THEN 开 double buffer，吞吐 +20–50%
IF DataCopy THEN AlignUp(offset, 512)
查到 Softmax 模板 MUST 整份拷贝进 op_kernel/
3510 UB = 256KB
```

---

## Review 清单（给下一轮写 Skill 用）

出现任一条就打回：

- [ ] 主体变成各族 design.md 目录
- [ ] 写入未核加速比或 512B / 256KB 当 950 事实
- [ ] 重写 Batch 2 API 合同
- [ ] 无测量就要求改 pingpong / Counter / NDDMA
- [ ] 普通 Add 被强制走 MC2 Step 2
- [ ] 把打点 / msprof 步骤写进主体  
- [ ] 开新的 copy / pipe / mask Card  
- [ ] 把缺 readiness 写成性能目标  
- [ ] 要求优化前后 same code identity  
- [ ] 把打点构建的数字直接当成生产加速比

---

## Sources

| Field | Value |
| --- | --- |
| CANNBot | `ops/ascendc-performance-best-practices`、`ops/ascendc-perf-optimize` @ `c4c425641d11127945d22e5a752fcad77646be97` |
| 冲突点 | common DataCopy 512B / 910B 表；ub_resident 3510=256KB vs hierarchy 248 KiB |
| 模型 | [performance_judgment_contract.md](performance_judgment_contract.md) |
| Last written | 2026-08-22 |
| status | accepted |
