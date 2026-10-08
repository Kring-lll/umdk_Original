# Gap Log

本文件是 **问题发现与证据账本**，不是 Agent 开发时的执行清单。

```text
记到 GAP_LOG
  ≠
已经回馈到 Weaver

OPEN
  ≠
Skill 会执行这条规则
```

**日常算子开发不要打开本文件。** 去 `skills/operator-development/` 与
`references/performance-by-construction.md` 的具名 Gate。

---

## Promotion protocol（机械闭环）

```text
真实失败
    ↓
GAP OPEN
    ↓
抽象成通用工程规则
    ↓
写入 canonical Skill / PBC
    ↓
LANDED
    ↓
另一次 clean-room 实际触发并做对
    ↓
VALIDATED          （Weaver behavior reproduced；默认停在这里）
    ↓
确认不是偶然 / 规则无副作用
    ↓
CLOSED
```

禁止：发现问题 → GAP_LOG 写一段 → CLOSED。
禁止：刚写入 Skill/PBC → CLOSED。
禁止：发现该 Gap 的同一任务 → VALIDATED。

LANDED 保持 LANDED，直到 **换算子** 的 clean-room 触发并做对。
VALIDATED 不必马上 CLOSED；保留 VALIDATED 更严谨。

### 失败分流（LANDED 之后再做错）

```text
GAP 是不是 LANDED / VALIDATED？
    否 → Weaver 还不知道：补规则，不要先怪 Agent
    是 → Agent 有没有经过具名 Gate？
            没有 → Skill routing / Gate enforcement 没执行
                   （修门禁清单 / MUST，不要重写那条知识）
            有   → Gate 条文不够硬，或 Agent 跳过
                   （收紧 checklist，不要再开一条同义 Gap）
```

### Status

### Status

| 状态 | 含义 |
| --- | --- |
| OPEN | 已观察，规则未提炼或未改 canonical 文件 |
| DISTILLED | 规则写出来了，还没落进 Skill/Knowledge |
| LANDED | canonical 文件已改；Agent 开发只走具名 Gate。**不得因“写进 Weaver”升 CLOSED** |
| VALIDATED | **另一次 isolated** clean-room 触发并做对（Weaver behavior reproduced）。发现任务本身不算。默认停在 VALIDATED |
| CLOSED | VALIDATED 之后确认非偶然、规则无副作用 |

`Status: open` 的旧条目一律视为 **OPEN**，即使正文很长。

---

## Audit board（2026-08-25）

发现任务：MoonEP Combine NPU clean-room-1。**该训练链正式暂停。**

001/002：Combine 之前已落地，且本轮 isolated generation 遵守 → CLOSED。
003–006、008、010–012：旧迭代暴露 → 规则进 Weaver → 本轮清仓重做 Combine 独立做对
= **Weaver behavior reproduced** → VALIDATED。不升 CLOSED。
007 / 009 / 013 / 014 扩展 + 015 / 016：本轮才 LANDED，保持 LANDED。

### 下一次 clean-room（transfer test）

**不要再删了重写 Combine。** Agent / repo / human docs / dataflow 太相似，只能证明“还会写 Combine”，不能证明跨算子迁移。

新任务必须换算子，且故意同时触发 LANDED 项：

| 任务需要天然具备 | 验 |
| --- | --- |
| 结构化 metadata / prefix / range | 007 |
| in-kernel ≥2 phase + 分核 remainder | 009；**020 worker-space before partition** |
| 仓内有相似 API idiom，但不是同一算法 | 014；sibling 资源上限 **path-scoped** |
| 常规 shape 能 full-H / full-row，理论上有 extreme | 015 |
| 一条大数据主路径 + 一条小而频繁 side path | 016 |
| width-change / 嵌套 Cast-Add 热路径 | 008 EXTEND：完整 operand graph，禁止口头 in-place |
| production shape 能放下 E3 却可能停在 E2 | 011 EXTEND：比较 overlap envelope |
| 非自然对齐 / 短 burst DMA | 017：API 选定后仍要参数级 footprint |
| hot loop 里 UB 上的 control traversal | 018：单遍消费不得退化成 repeated scan |
| kernel 显式不支持某 shape | 019：Host 必须有等价或更严的 launch guard |

**不要告诉 Agent 这些规则来源于 Combine。** 只走 Skill / PBC 具名 Gate。

### LANDED 验收冻结（007 / 009 / 014 / 015 / 016 + EXTEND / 017–020）

下一次 review 用这张表，不要用“感觉学会了”。

| Gap | 什么行为才算 VALIDATED |
| --- | --- |
| 007 | 从 monotonic / prefix / contiguous invariant **直接消掉扫描 / 重复计算**，而不只是拿它做 bulk DMA |
| 009 | **主动列出** phase A end-state → phase B startup-state；没有重复 Set、悬空 event、默认 event 状态 |
| 014 | **主动寻找** same-role repo idiom；选择/偏离时有 SOC、API role、pipeline、resource 比较，而非只说“API 存在” |
| 015 | 正常 production shape **直接走 full dimension hot path**；没有为假想 extreme H 在主代码塞 tile abstraction |
| 016 | **主动把 dominant + side 画到同一 timeline**，并尝试把 tiny side work 塞进主流水 bubble。`hidden 有流水 ∧ weight 有流水` **不够**；必须 `joint timeline` |
| 008 EXTEND | 每个实际运算有可定位 src/dst；width-change **没有**口头 in-place |
| 009 EXTEND | HardEvent 类型从表的 writer/reader/next-writer **推出**，不是 sibling 熟名 |
| 011 EXTEND | 比较 E1/E2/E3；更强档轻松放下时较弱档有成本理由 |
| 014 EXTEND | 不把 sibling 的 API-specific 资源上限套到不同 transport path |
| 017 | 冻结 API 后仍证明 overload / 单位 / pad / dst footprint；默认 `{}` 不是证据 |
| 018 | hot-loop 单遍消费未被 helper 写成 repeated scan / O(N²)；**EXTEND：按完整 call graph 相乘，不得只给 caller 主循环贴标签** |
| 019 | Host `TORCH_CHECK` 覆盖 kernel supported domain；**EXTEND：每一个物理 footprint 不等式都进入 Host 交集** |
| 020 | Frozen-Clause 表对上源码；physical → logical worker 先于 remainder；不得把 `GetBlockIdx()/GetSubBlockNum()` 简化成 `GetBlockIdx()` |
| 009 EXTEND-2 | **publication graph** 覆盖所有跨 mechanism / 跨 phase 边（含 SIMT→SIMT、ABI tensor、同一 AIV）；same-AIV 不省略 visibility 证明 |
| 009 EXTEND-3 | 每个 Wait/quiet/barrier 有 **distinct unsatisfied edge**；「更保险」不能保留已被后续原语覆盖的 fence |
| 011 EXTEND-2 | **每个** dominant phase 单独比 envelope；一阶段 ping-pong ≠ kernel pipeline PASS |
| 014 EXTEND-2 | 继承 cap 前填 provenance tuple；「更小更安全」不得改写 Frozen envelope；first-party vs Frozen 冲突则 STOP |

013 的句子分级仍 LANDED；随同一次 transfer 观察即可，不单独做 Combine 回归。

| GAP | 问题 | Canonical target | 已落盘？ | 另次验证？ | 状态 |
| --- | --- | --- | --- | --- | --- |
| 001 | target applicability；repo default ≠ this target | Skill：spec-reconstruction R0；op-dev Target environment | 是 | 是（Combine 冻 950/9.1/`dev_91`） | CLOSED |
| 002 | Host 不为 tiling D2H device metadata | Skill：Host/device provenance check | 是 | 是（Combine tiling 无 dup_counts D2H） | CLOSED |
| 003 | logical phase ≠ kernel boundary；搜 VEC barrier | Skill：phase/kernel boundary；P4 | 是 | 本轮 Combine 单 `__global__`+vec barrier | VALIDATED |
| 004 | num=2 ≠ pipeline；须 per-phase timeline | PBC Steady-state | 是 | Combine hidden 三流水 | VALIDATED |
| 005 | 第一根轴不够；occupancy ≠ 拆 natural unit | Skill：parallel-axis exploration | 是 | Combine 未跨核拆 H | VALIDATED |
| 006 | 先 physical UB map，后 TQue | PBC Memory Orchestration Gate | 是 | Combine manual UB | VALIDATED |
| 007 | 分核 locality + bulk DMA + 消扫描 | Skill partition locality；PBC **Locality Work-Elimination** | 原规则是；**消扫描本轮才 LANDED** | 原 bulk DMA 已见；头尾求 span **未验证** | LANDED |
| 008 | 伪码 ≠ 物理通路；DMA landing；**EXTEND：完整 operand graph / 禁止口头 in-place** | PBC Physical Realizability | 是 | 原 Cast tmp 已见；**本轮 Combine 修复后 operand graph 闭合** | VALIDATED + EXTEND **VALIDATED** |
| 009 | event 成对 + phase A→B 交接；**EXTEND：类型从拓扑推出**；**EXTEND-2：publication graph 含所有 mechanism 边** | PBC Event protocol / **Phase-transition closure** / **Publication graph** | 是 | 成对已见；拓扑推出已见；**SIMT→SIMT / ABI tensor 边仍漏检（Planning V2）** | LANDED + EXTEND **VALIDATED** + EXTEND-2 **LANDED** |
| 010 | write set；sibling 只读 | Skill Implementation / project integration | 是 | Combine 未改 Dispatch | VALIDATED |
| 011 | 闭环 ≠ 够飞；配 store credits；**EXTEND：比较 E1/E2/E3 envelope** | PBC pipeline resource depth | 是 | dual in/out 已见；**本轮 Combine 选 E3 并拒绝无成本 E1** | VALIDATED + EXTEND **VALIDATED** |
| 012 | collective participant set ≠ launch set | P4 participation-set 表 | 是 | Combine 32 AIV 全进 barrier | VALIDATED |
| 013 | human **file** 不可改；**句子**仍要分级 | Skill Input mutation；spec-reconstruction 证据分级 | 文件政策是；**句子≠ABI 本轮补进** | 未改 docs；sketch 分级 **未验证** | LANDED |
| 014 | same-role idiom 是 default；搜过 ≠ 已选；**EXTEND：资源上限 path-scoped** | PBC **Repo-native API Search Gate**；docs-search 只答合法性 | 是 | Combine `ptr`+Pad；**188 KiB 未当 barrier user cap** | LANDED + EXTEND **VALIDATED** |
| 015 | production 不要为极端 shape 给 hot path 加 H-tile | PBC **Hot-path vs Exceptional Shape Gate** | 本轮 LANDED（具名 Gate） | 否 | LANDED |
| 016 | dominant+side 必须联合 timeline | PBC **Side-path Integration Gate** | 本轮 LANDED（具名 Gate） | 否 | LANDED |
| 017 | **NEW** API 选定 ≠ 参数级合同 / 物理效果 | PBC **API Invocation Contract Gate** | 本轮 LANDED | 否（发现任务本身不算） | **LANDED** |
| 018 | 驻留 metadata ≠ 免费重复控制遍历；**EXTEND：whole-call-graph 相乘** | PBC **Hot-loop Traversal Gate**；Gate B.5 | 是 | Combine O(K²) 发现任务不算；**Planning V2 helper O(B)×O(R·E) 漏检 → 收紧手续** | **LANDED** + EXTEND **LANDED** |
| 019 | kernel 支持域 ≠ Host 准入域；**EXTEND：max-footprint 不等式进 Host 交集** | PBC **Launch Admission Gate** | 是 | Combine E3 vs H*2 发现任务不算；**Planning V2 R=1 overlay 漏检 → 收紧** | **LANDED** + EXTEND **LANDED** |
| 020 | physical launch index ≠ logical worker identity；冻结公式不得「看起来等价」后改写；remainder 证明依赖已证 worker-space | PBC **Logical Worker Topology Gate**；tiling-from-shape §3 | 本轮 LANDED | 否（Prefetch 发现 + Dispatch 迁移不算独立 VALIDATED） | **LANDED** |

**MoonEP Combine 训练链暂停。** 下一次 clean-room **换算子**，按上表把 LANDED / EXTEND → VALIDATED。发现任务本身不算。

### Addendum（2026-08-26）— last-mile closure

另一次 Combine 实现轮（Gate A 已 ACCEPTED 后再写核）暴露：大方向能对，
具体 operation / buffer / event / API 参数 / iterator / Host guard 仍会滑。
**不**再删了重写 Combine，**不**大改 operator-development 主流程。
下一轮回灌重点：Gate A 机械 proof obligation + Gate B actual-code check。

失败分流：008 / 009 / 011 已是 VALIDATED，本轮仍错 → Gate 条文不够硬，
**同号 EXTEND**，不开同义 Gap。014 的 188 KiB 误推广并进 014，不开新号。
017 / 018 / 019 是新失败类。

Combine 三个 implementation 问题修完并 Gate B PASS 后：

```text
GAP-008 EXT   VALIDATED     （operand graph：tmp 物理位；禁止口头 in-place）
GAP-009 EXT   VALIDATED     （Event 从 writer→reader→next-writer 推出）
GAP-011 EXT   VALIDATED     （比较 E1/E2/E3；生产 shape 选 E3）
GAP-014 EXT   VALIDATED     （188 KiB 不随 Prefetch helper 继承到 ptr+Pad）

GAP-017       LANDED        发现任务不算独立验证
GAP-018       LANDED
GAP-019       LANDED
```

017–019 等下一次 **不同算子** clean-room 再升 VALIDATED。不要 CLOSED。
日常开发只走 PBC 具名 Gate，不要读本文件里的 017/018/019 故事。

### Addendum（2026-08-26）— Gate B implementation attack

Planning V2 证明 Gate A / PAC 已能独立做出并挑战正确 first design
（sufficient-state hist + compact inbox；ordered cone + token-owned SIMT）。
**不开同义 Gap 复述 018/019/009。** 当时写「不开 GAP-020/021」仅指 Planning V2 漏检分流。
**2026-08-27：GAP-020 编号现用于 logical worker space**（Prefetch/Dispatch 发现），不是 Planning V2 条目。

漏检三类：

```text
helper 把 O(R·E) 乘成 O(R·E·B)
overlay lifetime 不重叠但 R=1 时 E+B > 分配
P3b SIMT 写 dst → P4a SIMT 读 dst 未进 publication graph
Gate B 自报 PASS
```

落盘：

```text
GAP-018 EXT   whole-call-graph multiplicative reconstruction
GAP-019 EXT   max-footprint closure ∩ Host domain
GAP-009 EXT-2 publication graph for every mechanism-crossing edge
NEW step      Gate B.5 Implementation Adversarial Challenge
Knowledge     implementation-adversarial-statics.md（三条稳定规则）
Eval          gate_b_static_attack（eval 已移除；必须重新找出，不是记住答案）
```

正向保留：sufficient-state 升为核心 distributed-design principle（已 regression 验证）；
serial island 强化为 ordered identity ≠ ordered downstream。

下一阶段重点：教 Weaver **不相信自己刚写出来的好算子**。

### Addendum（2026-08-27）— Prefetch / Dispatch distillation

两次 **不同算子** clean-room 之后回灌。**不改** Prefetch/Dispatch 实现。
只把可迁移原则写入 Skill / PBC / Knowledge / Eval。

| 发现 | 落盘 | 新号？ |
| --- | --- | --- |
| physical launch index ≠ logical worker；remainder 可在错误 ID 空间自洽 | **GAP-020 LANDED**；PBC Logical Worker Topology Gate | 是（与 GAP-012 不同：012=collective 到齐，020=work ownership ID） |
| Frozen-Clause 表存在但不穷举 → PASS | Gate A Extraction + Gate B Traceability（手续，不开同义 Gap） | 否 |
| 一条 SHMEM 路径的 resource cap 继承到另一条；「更小更安全」改写 Frozen envelope | **GAP-014 EXTEND-2** provenance tuple | 否 |
| 一阶段 ping-pong 给整 kernel pipeline PASS | **GAP-011 EXTEND-2** per-phase envelope | 否 |
| extra quiet/fence「更保险」 | **GAP-009 EXTEND-3** sync minimality | 否 |
| 整数审计只搜乘法 | Gate B Integer Arithmetic Audit | 否 |
| Golden ≠ 绝对真理 | Knowledge golden-delta-review | 否 |

GAP-014 的 path-scoped 规则已在 Combine **VALIDATED**，Dispatch 仍把 GET 路径 cap 套到 PUT。
失败分流：规则已 LANDED，Gate 不够硬 → **同号收紧**，不开 021。
EXTEND-2 本身 **LANDED，不是 CLOSED，也尚未独立 VALIDATED**。

GAP-020 = **LANDED，不是 CLOSED。** 发现任务本身不能 VALIDATED。

Eval：增加不绑定 MoonEP 常量的 L1 题。（eval 已移除）

---

## 分类（先问：Agent 为什么不会？）

| 类型 | 典型表现 | 补什么 |
| --- | --- | --- |
| 行为缺失 | 知道知识，但不知道什么时候用 | Skill |
| 事实缺失 | Skill 要求检查 X，但不知道 X 的合同 | Knowledge |
| 机制缺失 | 现有 Case/Knowledge 解释不了新机制 | Case（仍要过 Case 门） |
| 证据缺失 | 不知道去哪里确认 API / 头文件 | docs-search / source |
| 语义缺失 | 无可靠设计文档，参数/数据流未知 | Skill：`operator-spec-reconstruction` |
| 不是知识问题 | 已经知道，但执行错了；或一次随机失手 | **NO-ACTION**（workflow / task / 不补） |

```text
Knowledge = 是什么
Skill     = 什么时候做、为什么、做完下一步是什么
GAP_LOG   = 这条 Skill 规则从哪次失败来
```

最高价值的 Skill gap：

```text
Agent 知道 X，但不会在 Y 时使用 X
```

例如已经有 copy Card，仍看到 507035 就改 `blockLen` → 补的是 debug 时机，不是再写一篇 DataCopyPad。

提炼进 Skill 时写 **通用 Gate 句**，不要写「MoonEP Combine 的 weight 循环」。

---

## 补什么

```text
已有 accepted 资产能解释吗？
        YES                         NO
         │                           │
    没调用 / 没执行？            缺事实？
     YES        NO              YES        NO
      │          │               │          │
    Skill    Workflow/        Knowledge   新机制？
             NO-ACTION                     │
                                      YES      NO
                                       │        │
                                     Case*    Source /
                                              evidence
```

\* Case 门（已冻结）：existing pattern 解释不了 **且** live path 有机制证据 **且** 产生新的工程判断。否则 Knowledge absorbed，不开新 Case。

**NO-ACTION：** 模型随机失手（资产已写清）；只出现在不可迁移的特例；只是文案不漂亮但 Agent 已会用；新算子细节已被现有 Case 覆盖。

禁止：一条原则一个 Markdown。`active ≠ exposed` 若已在 optimization Skill 里，不要再开 `performance-judgment.md`。

---

## 条目模板

```text
## GAP-00N
### Observation            失败证据（可含具体算子名）
### Context
### Classification         Skill | Knowledge | Case | evidence | NO-ACTION
### Existing assets
### Missing behavior
### Action                 补什么 / 明确不补
### Promotion
    Status:              OPEN | DISTILLED | LANDED | VALIDATED | CLOSED
    Classification:      Skill | Knowledge | Case | No-action
    Canonical target:    可定位路径（skills/... 或 PBC 文件）
    Landed rule:         具名 Gate / section（Agent 执行接口）
    Validation:          pending | <transfer-task>
    Origin:              发现该 Gap 的任务（不是验证任务）
```

Observation 可以保留 Combine 细节。Landed rule 必须是 Agent 不读本文件也能执行的 Gate 名。

---

## Log

## GAP-001

### Observation

在 MoonEP Combine 的环境冻结阶段，
Agent 因 target repo 默认 build 为 A2、
sibling kernel 使用 A2 配置，
将目标环境从 unknown 直接冻结为
Ascend910B / CANN 8.5。

实际需求目标为 A5 / Ascend950。

Phase 1 正确地把 hardware / CANN 保持 unknown，只记录「工程默认 A2、可选 Ascend950」。下一阶段突破了这条 evidence boundary。

### Context

operator-development
Design Proposal 前的 target-environment resolution

### Classification

Skill

### Existing assets

operator-spec-reconstruction 已要求 R0 记录
target SOC / CANN，并在证据不足时保持 unknown。

### Missing behavior

Agent 不知道：

repo default / sibling environment
只能说明「工程当前已有支持」，
不能证明「本次 operator target」。

目标部署环境属于 task contract。
缺少 authoritative evidence 时不得由 repo default 推断。

### Action

在 operator-development / spec reconstruction
的 Target Context gate 增加环境来源优先级和 blocking rule。

不补新 Knowledge。

### Promotion

```text
Status:              CLOSED
Classification:      Skill
Canonical target:     skills/operator-spec-reconstruction/SKILL.md :: R0 Target environment
skills/operator-development/SKILL.md :: Target environment resolution
Landed rule:         repo default / sibling env ≠ this operator target
Validation:          MoonEP Combine clean-room froze Ascend950 / CANN 9.1 / cam_wangyibo_dev_91
Origin:       MoonEP Combine env freeze
```

---

## GAP-002

### Observation

MoonEP Combine Design Proposal 将 device tensor
`dup_counts[0:2]` 拉回 Host，
用于填写 `CombineTilingData` 和决定 `block_dim`。

这样可能引入 D2H synchronization，
破坏异步执行和 pipeline。

### Context

operator-development
Design Proposal 的 Host tiling / launch 轴

### Classification

Skill

### Existing assets

tiling-from-shape（tile 从 shape 来，没有 provenance）
performance-by-construction（P5 禁止无理由反复 GM load，
没有禁止 Host 为了 tiling 做 D2H）

### Missing behavior

Host tiling field 必须说明 provenance：

- host shape / attr
- compile-time constant
- derivable host value
- device runtime value

不得为了生成 tiling，
无理由把 device runtime metadata 同步回 Host。

Agent 不知道：

device dynamic metadata
  → 保持 device-resident → kernel consume

不要：

device metadata → D2H → Host tiling → 再 launch device

### Action

operator-development 的 Design Proposal 增加
**Host/device provenance check**。

每个 tiling field 必须回答：
“这个值在 launch 前 Host 怎么得到？”

若来源是 device tensor：
默认保持 device-resident，由 kernel 消费；
除非 task contract 明确允许 host sync。

不补新 Knowledge。

### Promotion

```text
Status:              CLOSED
Classification:      Skill
Canonical target:     skills/operator-development/SKILL.md :: Host/device provenance check
Landed rule:         device runtime metadata stays device-resident; no D2H for tiling
Validation:          Combine tiling has no dup_counts D2H
Origin:       MoonEP Combine Design D2H dup_counts
```

---

## GAP-003

### Observation

MoonEP Combine 把逻辑阶段机械变成 kernel 边界：

```text
host stage
  → kernel 0: dup prologue
  → rank barrier
  → kernel 1: remote consume
```

correctness 上自然，性能上把整个 prologue latency
暴露在 critical path：所有 rank 先做完 prologue，
再 barrier_all，才开始 consume。

Skill 原先只问阶段是什么、dependency 是什么、
barrier 保护谁，没有问：这个阶段是否必须成为
独立 kernel。

### Context

operator-development
Design Proposal 的 phase / launch / synchronization 轴
MoonEP Combine k0 + `aclshmemx_barrier_all_on_stream` + k1

### Classification

Skill

### Existing assets

performance-by-construction P4 只约束 kernel 内
PIPE_ALL / 过宽 HardEvent，不审查 launch 边界。

pipeline / buffering 轴只问 `none|single|double|other`，
不问 phase 是否必须拆成两次 launch。

### Missing behavior

逻辑 phase 不得自动映射为 kernel boundary。

每个 kernel boundary 必须说明：

- 为什么不能 fuse，或 fuse 后仍要什么同步
- launch boundary 提供了什么必要语义
- 是否制造 exposed global barrier
- 是否阻止 producer/consumer overlap
- 是否存在 chunk / wave pipeline 方案

```text
phase decomposition
  ≠
kernel decomposition

kernel boundary
  requires justification
```

不得把目标收成「必须合成一个 kernel」。
一个 kernel 里仍然 `prologue → barrier_all → consume`，
性能结构没有本质变化。

真正要问的是：

```text
global phase serialization
  →
能否变成 pipeline
  （chunk 0 reduce → publish ready → peer consume chunk 0
    同时 chunk 1 local reduce …）
```

### Update (2026-08-25) — fused operator launch topology

原句太弱。Agent 可以把「我比较过了，两个 kernel 更合理」写进 Proposal，
再把 Host `aclshmemx_barrier_all_on_stream` 的方便性当成拆 launch 的理由。
v3 Design 选 KD0（prologue kernel → host stream barrier → consume kernel），
Implementation 忠实实现了这个错误选择。GAP-003 没有挡住。

更糟的是 **mechanism space 没搜完**：把「单 kernel」基本等同于
「必须 MIX + device Barrier」，因此提前淘汰 fused VEC。
目标仓既有 Combine 反例是 **一个** `__global__` + 一个 `*_kernel_do`，
内部 `ProcessPrologue → (sync) → ProcessCombine`，其中出现过
`aclshmemx_barrier_all_vec()`。这证明：

```text
prologue → 跨 rank barrier → consume
  ≠
两次 device kernel launch

single fused VEC launch
  + in-kernel / VEC-side phase transition
```

是一等候选，必须重新验证。

**不要**把该反例里的 `PipeBarrier<PIPE_ALL>()` 抄成 vNext 合同。
PIPE_ALL 仍按 P4 审查。反例的价值是 launch 拓扑，不是那条 barrier 原语。

Host wrapper 有几个 `Launch*` 函数 ≠ device 有几次 launch。
数 `__global__` 和 `*_kernel_do`，不要数 Host 封装。

```text
For tightly coupled phases belonging to one fused operator:
  single device launch is the default candidate.

A logical phase boundary must NOT become a separate kernel
merely because a Host stream barrier is convenient.
```

拆成多次 device launch 之前，必须证明下列之一：

1. 目标硬件 / API **不能**表达所需的 in-kernel 同步；
2. correctness **要求** launch 边界；
3. **测量**表明 split launches 更好。

淘汰 fused 候选时必须枚举目标环境下的同步机制，至少包括：

```text
scoped HardEvent / TQue
scoped PipeBarrier（非 PIPE_ALL）
VEC-side SHMEM barrier（如 aclshmemx_barrier_all_vec / aclshmem_barrier_all）
Host stream barrier（aclshmemx_barrier_all_on_stream）
MIX + device Barrier
per-PE / per-slot ready（chunk/wave）
```

禁止在未查目标版本 SHMEM/Vector barrier 头文件、也未对照仓内成功先例的情况下，
因「跨 rank barrier 必须 MIX/AIC」而拒绝 fused VEC。

### Action

Design Proposal **phase / kernel boundary** 轴：default = 单 launch；
拆 launch 必须过上面 1/2/3，并列出 mechanism 搜索结果（查过哪些 API / 先例）。
P4 / PERF-SMELL-05：Host stream barrier 方便、或未搜 VEC barrier 就拆 kernel
→ unjustified。

不要求无脑 2→1。不把 PIPE_ALL 写成融合合同。
不补新 Knowledge。

### Promotion

```text
Status:              VALIDATED
Classification:      Skill
Canonical target:     skills/operator-development/SKILL.md :: phase / kernel boundary
skills/operator-development/references/performance-by-construction.md :: P4
Landed rule:         single device launch is default; enumerate VEC barrier before split
Validation:          this Combine: one __global__ + aclshmemx_barrier_all_vec (not a second task)
Origin:       MoonEP Combine k0+host barrier+k1
```

---

## GAP-004

### Observation

Agent 看到 `TQue num=2` 就声称 double buffer / pipeline，
没有用 statement ordering 验证 steady-state 是否重叠。

MoonEP Combine 证据：

1. k0 `inQue_ num=2`，但循环是
   `AccumulateTile(current)` 之后才 `CopyInHidden(next)`。
   下一块 CopyIn 在当前 Compute 结束后才发出。
   资源是双缓冲，调度是串行。
2. k0 / k1 `outQue_ num=1`，且
   `Cast → EnQue → DeQue → CopyOut → Free` 立刻 drain。
   即使改成 `num=2`，这个 schedule 仍然可能基本串行。
3. Gate B 把 PERF-SMELL-04 标成 absent，依据是
   「用了 num=2」，不是 timeline。

k1 hidden 路径才是真正的 one-step prefetch：
先 `CopyInPeer(next)` 再 `AccumulateTile(current)`。

### Context

operator-development Gate A P3 / Gate B PERF-SMELL-04
TQue Card（官方：`num=2` 开 double buffer = 分配）

### Classification

Skill

### Existing assets

TQue Card 已写 `num ≠ depth`，并写 `num=2` 只给第二块
storage、「才有可能」重叠。没有强制 Gate 用
statement ordering 画两个 iteration。

P3 要求填写 `none|single|double`，Agent 填 double
即可过。PERF-SMELL-04 问的是「未分析就强制串行」，
「我分析了，我有 num=2」会被当成分析完成。

### Missing behavior

```text
double buffer allocation
  ≠
double-buffered execution
```

Performance Gate 不能只检查「有没有 num=2」。
声称 pipeline / double buffer / 通信掩盖时必须回答：

- 第 n+1 个 DMA 是否在第 n 个 compute 完成前发出？
- 第 n 个 CopyOut 是否与第 n+1 个 compute overlap？

必须按实际语句顺序画至少两个 iteration 的 timeline。
画不出来，不准叫「形成流水」。

Gate B 不得只接受 Agent 自我总结。

### Update (2026-08-25) — phase coverage

Combine v3 consume 已吸收 allocation ≠ schedule：
画出 `MTE2(k+1) || V(k)`、`MTE3(t) || V(t+1)`。

prologue 却只留下 `in ping/pong + acc + single out`，
没有 group `g` / `g+1` 的 startup / steady-state / drain。
跳过理由是「prologue 比较小」，不是「它没有独立 pipeline」。

```text
double buffer allocation
  ≠
pipeline schedule          # 原结论，仍成立

main-phase timeline
  ≠
operator pipeline review   # 本轮新结论
```

每个包含以下任意两个 pipe 的阶段，都必须独立审查：

```text
MTE2 | Vector | MTE3 | communication
```

必须回答：startup / steady state / drain / next-iteration overlap / buffer reuse。
prologue、epilogue、metadata stage 只要本身有 meaningful pipeline，就不能因为「不是主路径」自动跳过。

### Action

performance-by-construction 增加
**Steady-state execution review**（P3 + Gate B 共用）。
TQue Card 补一句：`num=2` 是 allocation，不是 execution。

2026-08-25：Steady-state 增加 **per-phase coverage**。
不得只对 dominant/main phase 画 pipeline。

不补新文件。不新开 Gap 编号。不根据 smell 报加速比。

### Promotion

```text
Status:              VALIDATED
Classification:      Skill
Canonical target:     performance-by-construction.md :: Steady-state execution review
Landed rule:         allocation ≠ schedule; per-phase 2-iteration timeline
Validation:          this Combine hidden prefetch + independent store ping/pong
Origin:       MoonEP Combine TQue num=2 serial schedule
```

---

## GAP-005

### Observation

Agent 找到一个 correctness-safe ownership 轴
（k0 = dup group，k1 = original token）后停止搜索，
没有评估第二个独立并行轴 H。

结果：`S` / `n_groups` 小时大量 AIV idle，
大 H 仍在单 core 内 `for h0` 串行。

H tile 写 disjoint address，不需要 merge。
Proposal 已承认 splitting H 不需要汇合，实现没用。

### Update (2026-08-24) — occupancy 过度修正

Combine v2 学到了「不能因 S < AIV 就无视 H」，但学过头：
把 `Consume work = token × H-tile` 当 baseline，Host 用

```text
n_h_occ = ceil(aiv / S)
```

主动拆 token 去填满 AIV。`S=4, AIV=56` 时 token 0 的 H0/H1/… 分到不同 core。

代价：每个 `(token, hTile)` 重做 `LoadDstK`、dst decode、rank/loff；
同一 token 拆 8 个 H tile 就把本应对 token 算一次的 metadata 放大 8 倍；
remote row `[0:H]` 碎成多个小 DMA。

```text
first valid partition
  ≠
sufficient parallelism          # 原结论，仍成立

maximum occupancy
  ≠
optimal partition               # 本轮新结论
```

`work_items < AIV` 是要调查的 smell，**不是**制造更多 work items 的指令。
active AIV 更多 ≠ 整体更高效。并行度必须和 locality / metadata reuse /
DMA granularity 一起判断。

### Context

operator-development Design Proposal
ownership / parallelization 轴
Gate B PERF-SMELL-10
MoonEP Combine v2 `ChooseCombineTiles` / `ConsumeWork`

### Classification

Skill

### Existing assets

第一轮已把 ownership 轴改成 parallel-axis exploration，
并把 `work_items << AIV` 且不回答第二轴收成 PERF-SMELL-10。

Skill 原文把「写 disjoint 的 H-tile」写成 occupancy 不够时的
**默认候选**。Agent 把它执行成 occupancy 自动策略。

### Missing behavior

Primary ownership should preserve the natural semantic/locality unit.

For Combine:

```text
default ownership = one token per AIV
                  = one group per AIV/chunk

Secondary H partition is NOT the default.
```

Only consider splitting H when:

1. primary work items are insufficient
2. extra active AIV 的收益超过 duplicated metadata load、
   repeated dst decode、repeated address calc、fragmented GM/remote DMA、
   loss of token-local reuse
3. shape-specific cost analysis or measurement supports it

一般规则：

```text
enumerate candidates
  → cost model（parallelism vs locality / metadata / DMA）
  → preserve natural unit
  → justify choice

IF work_items << AIV
THEN 必须审查第二轴，并给出切或不切的 cost model
答不出审查 → PERF-SMELL-10 present + unjustified

IF 为填满 AIV 默认拆 natural unit，而无 2/3
THEN PERF-SMELL-10 同样 unjustified
```

不强制任何 shape 都 2D。也不强制 occupancy 不够就 2D。

### Update (2026-08-25) — candidate heuristic ≠ baseline policy

Combine v3 不再因 `S < AIV` 拆 token（small S 且 `H=128` 时 idle）。
随后又写成：

```text
if S < AIV and H >= 512:
    split H
```

`512` 不是硬件 contract，也不是 profiling。Agent 把「H 看起来比较大」硬化成 Host/device 自动策略。

```text
work_items < cores
  → trigger investigation     # 仍成立

candidate heuristic
  ≠
baseline policy               # 本轮新结论
```

并行决策应停在：

1. `work_items < cores` → 调查
2. 找出 secondary axes
3. 估计 extra parallelism vs metadata duplication / locality loss / DMA fragmentation / decode repetition
4. **没有 workload 证据或硬约束时，不得把启发式阈值写成 baseline 自动策略**
5. optional specialization 保持 candidate / optimization hook，直到测量证明该激活

### Action

扩展 parallel-axis exploration：H-tile 是候选，不是 default occupancy policy。
收紧 PERF-SMELL-10：occupancy 不足不审查、以及 occupancy 自动拆 natural unit，
都是 unjustified。

2026-08-25：再收紧 — 未经测量的阈值（`H >= 512`）不得进入 baseline `if`。
它只能写在 Design Proposal 的 optimization candidate 里。

不补新 Knowledge。本条不解决「伪码 ≠ 物理通路」（那是 GAP-008）。

### Promotion

```text
Status:              VALIDATED
Classification:      Skill
Canonical target:     skills/operator-development/SKILL.md :: parallel-axis exploration
Landed rule:         enumerate axes + cost model; occupancy ≠ split natural unit
Validation:          this Combine: no cross-core H-split
Origin:       MoonEP Combine occupancy H-split
```

---

## GAP-006

### Observation

MoonEP Combine baseline 默认用 TPipe / TQue / TBuf
管理全部 UB：

```text
VECIN queue × 2
VECOUT queue × 1
acc TBuf
tmp TBuf
metadata TBuf
```

k0 / k1 都是「框架式 AscendC」：

```text
要搬数据 → TQue<VECIN>
要算     → TBuf
要输出   → TQue<VECOUT>
要双缓冲 → num=2
```

代码合法、生命周期不易错、容易编译。
但这种抽象把 UB lifetime 从 Init() 起固化：
ping/pong 由 queue 主导，跨阶段 reuse 难表达，
Agent 把 `num=2` 当成硬件最优流水。

TQue 本身可以生成很好的代码。问题不是「TQue 慢」，
而是 TQue exists ≠ TQue is the right orchestration。

### Update (2026-08-24) — 思考顺序仍反了

第一轮 Memory Orchestration Gate 要求「先选 strategy，交 UB map」。
Combine v2 报告 hybrid implemented，真实代码仍是：

```text
TPipe pipe_;
TQue<VECIN, 1> inQue_;
TBuf acc / tmp / out / meta;
pipe_.InitBuffer(...) × 5
outBuf 再切 ping/pong
pipe_.AllocEventID × 4
```

本质控制权仍在 TPipe allocator。Agent 自己写：
「全部 TPipe::InitBuffer。无绝对 UB[offset]。」

```text
需要考虑 manual UB
  ≠
先设计 physical UB layout
```

`TEventID` 本身不一定有显著额外成本。真正的问题是
`TPipe.AllocEventID()` 让 event / UB / pipeline protocol
都变成 allocator incidental state，而不是可 review 的静态资源。

performance-critical kernel 应直接表达：

```text
UB_ping / UB_pong address
event ping_mte2_v / pong_mte2_v
event ping_v_mte3 / pong_v_mte3
```

具体 AscendC event primitive 跟 A5 / CANN 9.1 已验证 API；
原则是：流水资源是显式设计对象。

不得写成 must manual UB。TQue 仍可用于 correctness baseline
或动态 lifetime。但不得从 TPipe/TQue/TBuf 开始设计。

### Context

operator-development Gate A
Memory Orchestration Gate
MoonEP Combine v2 `kernels/combine.cpp` InitBuffer
目标仓高性能 sibling 若普遍 manual UB，那是 convention 证据

### Classification

Skill

### Existing assets

TQue Card：合同，不回答该不该用。
Memory Orchestration Gate 第一版：先选 A/B/C，交 conceptual UB map。
Agent 把「选 hybrid + TBuf 切片 + AllocEventID」当成已满足 GAP-006。

### Missing behavior

对于 performance-critical kernel，思考顺序必须是：

```text
算法
  → 真实 lifetime
  → physical UB map（地址 / 大小 / 不重叠 reuse）
  → pipeline schedule
  → event ownership（显式 ping/pong，不是 allocator 顺带分配）
  → 最后才决定要不要用 TQue/TBuf 帮助表达其中一部分
```

禁止：

```text
我要 Input  → TQue
我要 acc    → TBuf
我要 output → TBuf
最后再分析 lifetime
```

若目标仓高性能实现普遍 manual UB，默认遵循 project convention，
再 justify 为何这次例外用 TPipe。

```text
TPipe.InitBuffer + AllocEventID
  ≠
physical UB orchestration

hybrid
  ≠
把一个 TBuf 切成 ping/pong
```

### Action

重开并强化 Memory Orchestration Gate：先 physical map，后 allocator。
PERF-SMELL-12：未先设计 physical layout 就默认 TPipe 族 → unjustified。

不禁止 TQue。不补新 API Card。不把「must manual UB」写成绝对规则。

### Promotion

```text
Status:              VALIDATED
Classification:      Skill
Canonical target:     performance-by-construction.md :: Memory Orchestration Gate
Landed rule:         physical UB map before TQue/TPipe
Validation:          this Combine: manual UB map
Origin:       MoonEP Combine TPipe-first InitBuffer
```

---

## GAP-007

### Observation

Combine v2 按 flattened work-item round-robin 分核：

```text
for (work = coreIdx; work < workItems; work += coreNum)
    g or t = work / n_h
    hTile  = work % n_h
```

每个 core 拿到的是跳跃的 group/token，不是 contiguous chunk
（core 0: groups [0, G0)，core 1: [G0, G1)，…）。

Prologue 每个 `PrologueWork(g, hTile)` 都 `LoadGroupMeta(g)`：

```text
primary  = dupGroupsGm_.GetValue(g * 3)
dupStart = dupGroupsGm_.GetValue(g * 3 + 1)
dupN     = dupGroupsGm_.GetValue(g * 3 + 2)
Copy dup_loffs[dupStart : dupStart+dupN] → UB
```

两层放大：

1. `dup_groups[g0:g1, 0:3]` 本是连续 GM，可 bulk DMA 进 UB 后连续消费；
   现在每 group 三次 scalar GM GetValue。
2. H-split 后同一 group 的 `dup_groups` / `dup_loffs` 被每个 hTile 再读一遍。
   `n_h_tiles = 8` 就是 8 倍 metadata amplification。

这不是小 locality 问题。连续 ownership 的目的是创造可批量搬运和 reuse 的机会。

`dup_groups` 顺序不保证 `dup_start` 对相邻 group 严格连续，
不能假设整表可一次搬完。可对一个 batch 看

```text
min(dup_start) .. max(dup_start + dup_n)
```

span 密则一次 bulk load `dup_loffs` span；稀则缩小 batch。

更合理的 prologue：

```text
core owns contiguous group range
  → bulk load dup_groups batch
  → density-aware bulk load dup_loffs span
  → metadata outer, H inner
  → 每个 group 一次 metadata，再做完整个 H / H tiles
```

### Context

operator-development Design Proposal
ownership 之后的 **partition mapping**
MoonEP Combine v2 `Prologue` / `LoadGroupMeta`
GAP-005（H-split 放大了本条）

### Classification

Skill

### Existing assets

GAP-005 管「切哪根轴 / occupancy vs locality」。
P1/PERF-SMELL-01、06 管 scalar GetValue，不要求先问
contiguous chunk vs cyclic。
PERF-SMELL-02 管反复 GM load，但不问分核形状如何制造反复。

现有资产不要求比较：

```text
contiguous chunk
vs
round-robin
vs
2D split
```

对 GM transactions / metadata reuse / load balance 的影响。

### Missing behavior

Core partition 不能只分析 write conflict 和 occupancy。
还必须分析：

- contiguous ownership
- GM spatial locality
- metadata reuse
- DMA aggregation opportunity
- repeated metadata amplification

```text
Prefer contiguous chunks on the natural ownership axis
unless load balance evidence requires cyclic partition.

Before choosing strided/cyclic partition, compare:
  contiguous chunk vs round-robin vs 2D split
for:
  GM transactions, metadata reuse, load balance

禁止未经分析的 flattened work-item round-robin。

metadata outer → H inner
  ≠
flatten (g, h) then re-read metadata per h
```

不补「必须按 group 连续分核」的绝对规则。要的是比较和 justification。

### Update (2026-08-25) — metadata staging capacity

v3 已吸收 contiguous + bulk DMA，但又写死：

```text
kPrologueGroupBatch     = 32
kPrologueLoffCapBytes   = 4096
```

每个 core：32 groups → DMA → Wait → scan → loffs DMA → Wait → compute，
再下一个 32。group DMA 后立刻 `MTE2_S` Wait。

既有实现把 stage 放到 core-local range 量级（约 1024 groups / 1024 loffs）：
typical 每 AIV 约 800 groups 时 **一次 stage**；超出才 window-forward。
处理的是整段 contiguous group range 上尽可能大块的 dup_groups + 连续 loff window，
再反复 UB local consume。

```text
batching exists
  ≠
batch size is sensible
```

Metadata batch / window 必须从这些量推出来：

- core-local work range
- UB headroom
- metadata footprint
- alignment
- expected reuse
- DMA / Wait 事务次数

禁止没有 capacity/cost 理由的保守魔法 batch。
typical core-local metadata 能进 UB → 优先 one-shot staging。
否则 monotonic / windowed staging，或 metadata ping-pong。
不要把「我做了 bulk DMA」当成 GAP-007 已满足。

### Action

Design Proposal 增加 **partition locality** 轴。
Gate B：未经比较的 cyclic flattened work、每 group 三次 GetValue
且可 bulk DMA、H-split 重复读同一 group metadata
→ 用现有 PERF-SMELL-02/06/10 记 unjustified，不新开 smell 编号。

魔法 metadata batch（如无容量账的 `32`）且 typical range 本可 one-shot
→ PERF-SMELL-02/11 视情况 unjustified。

**Implementation enforcement：** Design 已选 one-shot / monotonic windowed staging 之后，
Implementation 不得在 overflow 路径静默退回逐元素 GM `GetValue`。

```text
If Design selected metadata staging/windowing,
Implementation must not silently fall back
to per-element GM GetValue on an overflow path.

Any fallback changing residency/locality class
must reopen the relevant performance Gate.
```

one-shot 放不下 → monotonic loff/group window，仍从 UB 消费。
`dupLoffsGM.GetValue` 这类 residency 降级 = Gate B `present + unjustified`。

### Update (2026-08-25) — locality evidence must eliminate work

v5 已吸收 contiguous + one-shot / windowed bulk DMA：`dup_groups` 进 UB 了。
求 `loff0` / `loff1` 却仍遍历整个 `g_win` 做 `min(dup_start)` / `max(dup_start+dup_n)`。
装不下再 `g_win--` 从头重扫，最坏 `O(G²)`。某个 group 不在 staged span 时还按组补 CopyIn。

NPU planning producer 已被 Contract 证明 compact + monotonic：

```text
dup_start[0] = 0
dup_start[g+1] = dup_start[g] + dup_n[g]
```

contiguous `[g0, g1)` 的 loff span 是边界推导，不是扫描：

```text
loff0 = groups[g0].dup_start
loff1 = groups[g1-1].dup_start + groups[g1-1].dup_n
```

window 太大时 `end(g) = dup_start[g] + dup_n[g]` 仍单调，应推导 / 二分最大 `g_end`，
不是从尾部逐个 `--` 再整段重扫。按 invariant 正确切窗后，per-group restage
不是 production path。

```text
contiguous ownership
  ≠
bulk DMA only                    # 原结论，仍成立

proven locality / producer invariant
  →
must eliminate work              # 本轮新结论
```

已经证明 compact / monotonic / prefix 之后还要问：

- 能否去掉扫描，只读头尾？
- 能否从 prefix / monotonic 推导 range？
- 能否 O(1) / O(log G) 定 window boundary？
- 能否减少 scalar metadata 分支？

「做了 bulk DMA」仍不等于 GAP-007 已满足。
bulk DMA 只是把数据搬进 UB；invariant 还应该消掉 UB 上的重复扫描。

不补新 Knowledge。不新开编号。

### Promotion

```text
Status:              LANDED
Classification:      Skill
Canonical target:     skills/operator-development/SKILL.md :: partition locality
performance-by-construction.md :: P5 Locality Work-Elimination
Landed rule:         proven compact/monotonic eliminates scan; bulk DMA is not enough
Validation:          pending (Combine still min/max scans loff span)
Origin:       MoonEP Combine cyclic GetValue; later magic batch=32; v5 window scan
```

---

## GAP-008

### Observation

MoonEP Combine v3 已会走

```text
ownership → locality → UB map → pipeline
```

但 Design Proposal 把概念运算写成可实现通路：

```text
Add(acc, Cast(in))
```

physical UB map 只有 input / acc / output / metadata，
没有 bf16→fp32 Cast 的 `tmp_fp32`。
Agent 知道 fp32 是 4B；它不知道 **算法伪码 ≠ 可实现数据通路**。

同一份 map 把 weight gather 写成 `wgt = K * 4B` 一次 Store。
source irregular / dest compact 的方向对，但没有问：
每一次 MTE2 的 destination base 是否合法。
logical 4B payload 被当成 compact UB landing。

Copy Card 已写 DataCopyPad **UB dest 仍要 32B 对齐**，
以及 `logical payload ≠ local storage footprint`。
Agent 没有把这条用进 UB-map Gate。

### Context

operator-development Gate A
Memory Orchestration / Design Proposal 热路径伪码
[copy Card](../knowledge/ascendc-api/data-movement/memory-copy-and-alignment.md)
docs-search（fused / mixed-dtype API 是否存在）
MoonEP Combine v3 consume Cast+Add；L4 4B weight Pad

### Classification

Skill

Knowledge **conditional**：现有 copy Card 已覆盖 dest UB 对齐、
transfer granularity、effective vs aligned。
本轮 **不补新 Knowledge、不开 GAP-009**。
若日后发现 Card 确实没有某条 DMA landing 约束，再补 Card，而不是再开 Gap。

### Existing assets

P2 问中间结果住哪（GM vs UB），不问表达式里的临时值
是否对应一条 concrete API。
Memory Orchestration 要 region / size / lifetime，
不要求每个伪码中间值落到 region。
copy Card 已有 dest 对齐；没有 Skill 规定
「每个 DMA edge 必须填 physical landing footprint」。

### Missing behavior

```text
abstract dataflow
  ≠
implementable dataflow
```

每个 performance-critical operation 必须落到：

```text
source dtype/layout
  → concrete API / instruction class
  → destination dtype/layout
  → required intermediate storage
  → producer/consumer pipeline
  → lifetime
```

例如：

```text
bf16 GM → DataCopy → bf16 UB
       → Cast     → fp32 tmp UB
       → Add      → fp32 acc UB
       → Cast     → bf16 out UB
       → DataCopy → GM
```

伪码里每个中间值必须回答「它物理上存在哪里？」。

```text
Add(acc, Cast(in))
Reduce(Cast(x))
Mul(Add(x,y), z)
quant(dequant(x))
```

除非 docs-search 已证明存在对应 fused / mixed-dtype API，
否则每个嵌套表达式都要拆开，并给中间结果留 physical region。

每个 DMA edge 额外验证（Physical buffer legality）：

```text
source base alignment
destination base alignment
transfer length / block granularity
effective payload
physical landing footprint
```

```text
logical buffer size
  ≠
legal DMA landing layout

wgt: K*4 bytes                 # 不够
logical payload:      K*4
DMA landing layout:   K × 32B-aligned slots  (example)
physical reserved:    ?
```

### Action

performance-by-construction 增加 **Physical Realizability Gate**（Gate A）。
Design Proposal 增加 physical realizability 轴。
copy Card 只在「先问什么」补 DMA landing 检查句，不新开文件。

不把 Cast+Add、4B gather 写成两条 Gap。
不根据本条报加速比。

### Promotion

```text
Status:              VALIDATED
Classification:      Skill
Canonical target:     performance-by-construction.md :: Physical Realizability Gate
Landed rule:         pseudocode ≠ implementable path; DMA landing footprint required
Validation:          this Combine: Cast tmp + K×32B weight slots
Origin:       MoonEP Combine Add(acc, Cast(in)) / wgt=K*4
```

### Update (2026-08-26) — complete operand graph

Gate A 已跑 Physical Realizability，实现仍出现：

```text
logical:   bf16 in → Cast fp32 tmp → Add(acc, tmp)
physical:  in0, in1, acc     // tmp 不存在
```

检查还停在「主要 tensor 有没有物理位置」。
`or in-place promote` 一类口头别名把 width-change 混过去。

```text
Logical intermediate
  ≠
physically realized operand graph
```

每个 **实际运算**（不是每个 named buffer）缺一项标 **未回答**：

```text
Operation
  src0 physical object
  src1 physical object   # 若二元
  dst  physical object
  dtype / bytes
  lifetime
  overlap / alias legality
  API legality
```

全部必须可定位到 UB map 上的 region，或证明的寄存器 / 同 pipe 临时。

width-change（`Cast(bf16→fp32)`、`Cast(fp32→bf16)`、量化）必须自动问：

```text
dst 在哪？
能否 alias src？
alias 有 target-version API 证据吗？
```

禁止：

```text
or in-place promote
Cast into the input buffer
sizeof(bf16) 升级成 fp32 覆盖 in_*
```

除非 docs-search / 头文件已证明该 Cast overload 允许 in-place **且**
元素宽度不变，或硬件合同明确允许覆盖。bf16→fp32 默认 **不能** alias。

本条收紧 008，不开新号。Gate B 必须从 **实际语句** 重建 operand graph，
不能只确认源码出现过 `tmp_fp32` 字样。

### Promotion (extension)

```text
Status:              VALIDATED
Canonical target:     performance-by-construction.md :: Physical Realizability Gate
Landed rule:         every op has locatable src/dst objects; width-change forbids verbal alias
Validation:          this Combine repair: Cast dst is tmp_fp32; no in-place promote
Origin:       Combine Gate A ran; first kernel still omitted tmp_fp32
```

---

## GAP-009

### Observation

MoonEP Combine v3 在修完 Cast tmp / DMA landing / per-phase pipeline 之后，
event **table** 已经声明：

```text
MTE2 → V       EV_PIN[0/1]
V → MTE2 reuse EV_PIN_FREE[0/1]
```

prologue statement schedule 却是：

```text
CopyIn(in[(i+1)%2], row[i+1])
SetFlag<MTE2_V>(...)
WaitFlag<MTE2_V>(current)
SetFlag<V_MTE2>(EV_PIN_FREE[current])
```

`i=1` 要覆盖 `in[0]` 时，没有 `WaitFlag<V_MTE2>(EV_PIN_FREE[0])`。
consume 同一条 WAR 是闭合的。声明了 dependency，schedule 没消费。

`*_FREE` Wait 旁注 “nop-initialized as free”，
9.1 `SetFlag`/`WaitFlag` 头文件没有把未 Set 的 event 定义成 ready。
Implementation 很容易 `WaitFlag(EV_FREE)` 等一个从未 Set 的 id。

weight 路径实际是 `MTE2 → S pack → MTE3`，
event table 只覆盖了 `MTE2_S`。缺 `S_MTE3` 与 `S_MTE2` reuse。

```text
declared dependency graph
  ≠
closed event protocol
```

### Context

operator-development Gate A P4
手写 `SetFlag`/`WaitFlag`（非 TQue EnQue/DeQue）
[pipeline Card](../knowledge/ascendc-api/synchronization/pipeline-events-and-ordering.md)
CANN 9.1 `kernel_event.h` `HardEvent`（含 `S_MTE2` / `S_MTE3` / `MTE2_S` / `MTE3_S`）
sibling `kernels/dispatch.cpp`：loop 前 `SetFlag<MTE3_MTE2>(ID0/ID1)` 建立 initial free token

### Classification

Skill

不补新 Knowledge 文件。pipeline Card 已有 Set/Wait 是跨流水原语；
缺的是「每条声明边必须在 statement 里成对出现 + startup 必须可证明」。
Card 只加 pairing / first-Wait 一句，不开新百科。

### Missing behavior

每个手工管理的 UB buffer 必须显式：

```text
1. Producer pipeline
2. Consumer pipeline
3. Reuse producer
4. RAW edge
5. WAR / reuse edge
6. Set site（statement）
7. Wait site（statement）
8. startup state
9. steady-state transition
10. drain state
```

```text
Every declared event edge must appear as a matched Set/Wait pair
in the actual statement schedule.

Every Wait-before-first-Set requires an explicit startup rule:
  first-use skips Wait
  OR
  kernel init SetFlag establishes a legal initial free token.

9.1 header does not document WaitFlag succeeding without SetFlag.
```

TQue `EnQue`/`DeQue` 走框架合同，不强制填这张 10 项表。
本条只约束 **manual HardEvent** 路径。

### Action

P4 增加 **Event Protocol Closure**。
Design Proposal 对每条 manual UB region 列表：producer / consumer / RAW / reuse /
Set / Wait / first-use / drain。
声明在 table、不在 schedule → Gate 不通过。

不并进 GAP-004。不根据本条报加速比。

### Update (2026-08-25) — phase-transition closure

startup / steady / drain 只覆盖 **一个** phase 的闭环。
单 `__global__` 里 `phase A → in-kernel barrier → phase B` 时，还要冻结
phase A **结束瞬间**每个 Event ID 的状态，以及 phase B 如何接手。

Combine v5：Prologue `StartupInOutCredits()` 给 `V_MTE2` ID0/1 发 FREE token；
最后一次 Acc 再 `SetFlag<V_MTE2>`，phase-end 只 `WaitFlag<MTE3_V>`。
Consume 再次 `StartupInOutCredits()`，对同一对 **已经 signaled/free** 的
`V_MTE2` 做 `SetFlag`。9.1 头文件不把 double-Set 定义成合法。

```text
startup / steady / drain
  ≠
phase A → barrier → phase B closure

SetFlag of a FREE/WAR event
  requires the previous token to have been Waited
  (or the ID to be unused in phase A)
```

每个跨 phase 的 Event ID 必须回答：

```text
phase A end state:   consumed | signaled/free | in-flight
phase B action:      inherit | Wait then re-init | disjoint event set
```

禁止无证明地重新 Set 已处于 signaled/free 的 `(HardEvent, eventID)`。
output 侧若 phase-end 已 Wait 完 `MTE3_V`，phase B 再 Set 是合法 re-init；
input 侧若 leftover FREE 未 Wait，再 Set 不是合法 re-init。

不并进 GAP-004。不新开编号。

### Promotion

```text
Status:              LANDED
Classification:      Skill
Canonical target:     performance-by-construction.md :: Event protocol closure / Phase-transition closure
Landed rule:         matched Set/Wait + phase A end state / phase B action
Validation:          pending (Combine Consume re-Sets signaled V_MTE2)
Origin:       MoonEP Combine unpaired V_MTE2; v5 phase-boundary Set
```

### Update (2026-08-26) — derive event type from topology

Agent 会填 phase-transition table，第一版仍写成：

```text
MTE2 writes input → V reads input → MTE2 overwrites input
HardEvent used:    MTE3_MTE2
```

熟悉的 sibling 名（Dispatch / Prefetch 的 `MTE3_MTE2`）被套到
**从未经过 MTE3 的 input stage**。解释后贴，不是从拓扑推出。

```text
Event type must be derived from the actual
last-writer / next-reader / next-writer graph,
never selected by sibling familiarity.
```

机械规则：

```text
reuse producer  =  last pipe that consumed this buffer
RAW             =  writer_pipe → reader_pipe
WAR / reuse     =  last_reader_pipe → next_writer_pipe
```

Gate A **先**生成并冻结这张表，**然后**才允许代码里出现 Event 枚举：

```text
Buffer   Writer   Reader   Next writer   RAW      Reuse WAR
in0      MTE2     V        MTE2          MTE2_V   V_MTE2
out0     V        MTE3     V             V_MTE3   MTE3_V
```

表未完成 → 不得冻结 HardEvent。
Gate B 从 **实际 SetFlag/WaitFlag 模板实参** 反推，对不上表 → P4 失败。
名字里有 ping-pong / 对上了 EVENT_ID 数字、类型却是 sibling 熟名 → 同样失败。

本条收紧 009，不开新号。

### Promotion (extension)

```text
Status:              VALIDATED
Canonical target:     performance-by-construction.md :: Event protocol closure
Landed rule:         RAW/WAR derived from writer/reader/next-writer; sibling HardEvent names are not evidence
Validation:          this Combine: in MTE2_V/V_MTE2, out V_MTE3/MTE3_V from topology; not MTE3_MTE2
Origin:       Combine input lifecycle used Dispatch MTE3_MTE2
```

### Update (2026-08-26) — publication graph beyond shared workspace

Gate B 已能重建 shared local GM：

```text
UB writer → MTE3 → MTE3_MTE2 → SyncAll → MTE2 reader
```

但仍会漏掉：

```text
SIMT GM store (ABI dst) → SIMT GM load
same AIV, disjoint slice, immediately next phase
```

理由常是：ABI tensor / 两端 SIMT / 同一 AIV 消费自己的 slice。
ownership 可以去掉跨核 SyncAll，**不能**去掉 visibility 证明义务。
`V_MTE2` 名等待 Vector→MTE2，不是 VF→VF GM visibility。

```text
Every producer→consumer edge crossing an execution mechanism
or phase boundary requires a visibility/completion proof.
```

本条收紧 009，不开新号。

### Promotion (extension-2)

```text
Status:              LANDED
Canonical target:     performance-by-construction.md :: Publication graph
Landed rule:         same-AIV / ABI tensor never omit producer→consumer visibility proof
Validation:          pending (gate_b_static_attack；eval 已移除)
Origin:       Planning V2 P3b SIMT dst write → P4a SIMT dst read
```

### Update (2026-08-27) — synchronization minimality

publication graph 问边有没有 visibility。下一层：每个同步原语是否关闭了
**一条尚未被后续更强原语覆盖的边**。

典型失败：Drain WaitFlag 已等 last MTE3，world barrier 文档已声明前序 RMA 可见，
仍插入 extra quiet / PIPE_ALL「更保险」。

```text
Every synchronization primitive must justify
one distinct unsatisfied dependency edge.

extra for safety
  ≠
justification
```

连续原语声称同一 completion → 标 review。不要自动删除同步。
本条收紧 009，不开新号。

### Promotion (extension-3)

```text
Status:              LANDED
Canonical target:     performance-by-construction.md :: Synchronization minimality
Landed rule:         each Wait/quiet/barrier maps to one unsatisfied edge; extra-for-safety is insufficient
Validation:          pending (next isolated clean-room after landing; Prefetch/Dispatch discovering tasks do not count)
Origin:       Dispatch Drain + quiet + barrier_all_vec without a leftover distinct edge
```

---

## GAP-010

### Observation

为实现 MoonEP Combine，Agent 改动了无关 sibling：
`3rdparty/catccos` 为空时，CMake 用 stub 替换 `allgather_matmul` 的链接符号，
并改了既有 kernel library 的 SOURCES 集合。Combine 编译过了，
AllGatherMatmul 的可运行行为被顺手改掉。

这不是「共同 API 必须动一处」。这是 **change-scope containment** 失败：
任务是 Combine，write set 却碰到 sibling operator。

```text
task-local implementation
  ≠
permission to modify sibling operators
```

### Context

operator-development Implementation / Gate B
目标仓已有 NotifyDispatch / Dispatch / AllGatherMatmul
Combine v3 `CMakeLists.txt` + `kernels/allgather_matmul_stub.cpp`

### Classification

Skill

### Existing assets

`project integration` 轴要求沿用 sibling 接入路径、禁止平行 scaffold。
它 **鼓励** 改 CMake / binding，没有禁止改无关算子实现。
「IF 目标仓已有 sibling THEN 改那些文件」会被读成可以改 sibling **行为**。

P-gates / PERF-SMELL 不审查 git 路径。

### Missing behavior

写核前必须声明 **write set**。

Allowed by default:

- 新目标算子文件
- 必要的 registration / build / binding / 本算子测试接入
- 确属 shared infrastructure，且有必要性证据

Read-only by default:

- 无关的既有算子实现
- 无关测试
- 无关行为

write set 之外的编辑必须同时有：

1. necessity evidence
2. blast-radius analysis
3. explicit justification

并回答：为什么不能只改 shared helper？为什么必须改变 sibling operator？

```text
为了 Combine 能编译
  → 顺手改 AllGatherMatmul
```

禁止。即使「顺手修共同 API」，也必须先走上面三条。

Gate B：`git diff --name-only` 对照声明的 write set。
未解释的 sibling-operator 修改 = Gate 失败。

### Action

operator-development **Implementation Gate**（不只是 performance-by-construction）：
Design Proposal 的 project integration 声明 write set；
Gate B 用 `git diff --name-only` 核对。

不补新 Knowledge。不新开 PERF-SMELL 编号。

### Promotion

```text
Status:              VALIDATED
Classification:      Skill
Canonical target:     skills/operator-development/SKILL.md :: project integration / Implementation write set
Landed rule:         declare write set; sibling operators read-only
Validation:          this Combine: did not modify Dispatch
Origin:       MoonEP Combine AllGatherMatmul stub
```

---

## GAP-011

### Observation

Combine v3 prologue 的 HardEvent **闭环了**（GAP-009 过了）：
不同 `HardEvent` 类型上复用 `EVENT_ID0/1`，input 有 ping/pong。

初始化实际是：

```text
SetFlag<V_MTE2>(ID0/ID1)     # in ping/pong free
SetFlag<MTE3_V>(ID0)         # single out free
```

hidden 行：`MTE2(next) || V(current)` 存在。
输出却是单 stage：

```text
WaitFlag<MTE3_V>(ID0)
Cast(out, acc)
SetFlag<V_MTE3>(ID0)
WaitFlag<V_MTE3>(ID0)
CopyOut
SetFlag<MTE3_V>(ID0)
```

既有实现把：

```text
ID0 / ID1  → hidden input ping/pong
ID2 / ID3  → output store ping/pong credits
```

分开。每个 group 算完用 `store_ev = ID2/ID3` 发 CopyOut，
**不在每个 group 结束时 drain**，整段 prologue 结束才 drain 两个 store stage。
下一 group 的 accumulate 可以继续，上一 group 的 store 在后台 drain。

```text
v3:   MTE2 ping/pong → Vector → single MTE3 store
old:  MTE2 ping/pong → Vector → MTE3 store ping/pong
```

不能把问题说成「只用了两个硬件信号量」。
同号 ID 在不同 HardEvent 枚举上是独立池。
真正缺的是 **intended concurrent stages 的 buffer / credit 数量**。

```text
closed event protocol
  ≠
sufficient pipeline resources
```

### Context

operator-development Gate A P3 / P4
GAP-004（statement order / overlap）
GAP-009（Set/Wait pairing / startup / drain）
MoonEP Combine v3 prologue vs 既有单 kernel prologue store credits

### Classification

Skill

### Existing assets

GAP-004 问 CopyIn 是否在 Compute 前发出、是否只画了 main phase。
Agent 可以画出 `MTE2 || V`，输出仍单缓冲，P3 仍填 `other` / double-in。

GAP-009 问每条边是否成对。单 out 的 RAW/WAR 可以完全闭合，
仍然无法 `V(n) || MTE3(n-1)`。

PERF-SMELL-04 的正当理由包含「无法叠」。
Agent 会把「我选了 single out」当成已经分析过，而不推导资源是否够飞。

### Missing behavior

正确的 Set/Wait pairing 只证明 correctness。
它不证明有足够独立的 buffer / event credit 去承载声称的 overlap。

对每个 performance-critical loop，从 **想同时飞行的 stage 数** 反推资源：

```text
stage
pipeline
buffer count
in-flight operations
event credits
reuse edge
```

不要把「尽量少用 Event ID」当成目标。

若意图是：

```text
MTE2(n+1) || V(n) || MTE3(n-1)
```

则物理 buffer 和 event credit 必须让这三件事同时发生，且没有假依赖。
input ping/pong + single out 在声明上可以闭环，仍会把 Cast-to-out
卡在上一次 CopyOut 的 WAR 上。

层次：

```text
pipeline topology        GAP-004     语句顺序会不会叠
        ↓
event correctness        GAP-009     Set/Wait 成不成对
        ↓
resource depth/credits   GAP-011     闭环之后资源够不够飞
```

三条不能混。不根据本条把旧代码的 ID2/ID3 写成绝对规则；
要的是按 intended concurrency 配资源，并 justification。

### Action

P3 / Steady-state 增加 **pipeline resource depth**：
每个 performance-critical loop 列出 concurrent stages → buffers → credits。
声称 `MTE2(n+1)||V(n)||MTE3(n-1)` 但只有一套 out buffer / 一个 store credit
→ PERF-SMELL-04 present + unjustified（除非写出该 WAR 不在 critical path 的 cost model）。

不并进 GAP-004 / GAP-009。不补新 Knowledge。不新开 smell 编号。

### Promotion

```text
Status:              VALIDATED
Classification:      Skill
Canonical target:     performance-by-construction.md :: Pipeline resource depth
Landed rule:         closed events ≠ enough credits for claimed overlap
Validation:          this Combine: dual in + dual out store credits
Origin:       MoonEP Combine single-out prologue
```

### Update (2026-08-26) — compare affordable overlap envelopes

Agent 证明了 `2×input + acc` 能跑 `MTE2 || V`，就停。
没有问：多给 output buffer，当前 production shape 能否得到
`MTE2 || V || MTE3`。H=7168 下 dual-in + dual-out + acc + tmp = 114688 B
放得进 253952，也放得进所选路径的 user 区。

```text
enough to run the chosen schedule
  ≠
best reasonable overlap envelope under the resource budget
```

Gate A 必须比较（名称可本地化，层次不能少）：

```text
E1  serial                 1×in + acc[+tmp]
E2  MTE2(next) || V        2×in + acc[+tmp]；store 若 alias in_* 则 MTE3 不在内环
E3  MTE2 || V || MTE3      2×in + 2×out + acc[+tmp]
```

每档：payload bytes、能否放进 **本次** UB 预算、overlap 少了哪根 pipe。
production shape 能轻松放下 E3 时，选 E2 **必须**写成本理由
（省下的字节实际买到了 metadata / 语义 / 另一条热路径）。
「2×in 已经能 pipeline」不是理由。

Gate B 从实际 UB map 重建 envelope，不能只数 ping-pong 单词。

本条收紧 011，不开新号。不把 E3 写成所有算子的绝对规则。

### Promotion (extension)

```text
Status:              VALIDATED
Canonical target:     performance-by-construction.md :: Pipeline resource depth
Landed rule:         compare E1/E2/E3 under production UB; weaker envelope needs cost reason
Validation:          this Combine: E3 selected at H=7168; E1 aliasing rejected (28 KiB not needed)
Origin:       Combine first Design stopped at dual-in without costing dual-out
```

### Update (2026-08-27) — per-phase overlap envelope

011 已要求比较 E1/E2/E3。Agent 仍可能：

```text
phase A has ping-pong
  →
kernel pipeline PASS
```

而 phase B（local expand / epilogue）在物理上放得下 E2，却用单 buffer 串行，
Gate 把 forced serial 标成 present+justified。

```text
one phase ping-pong
  ≠
kernel pipeline PASS
```

每个 dominant phase 必须自己回答 best affordable vs selected。
本条收紧 011，不开新号。不把某一 phase 的 E3 写成绝对规则。

### Promotion (extension-2)

```text
Status:              LANDED
Canonical target:     performance-by-construction.md :: Overlap envelope comparison / Steady-state per-phase
Landed rule:         envelope comparison is phase-local; one-phase pipeline ≠ kernel PASS
Validation:          pending (next isolated clean-room after landing; Dispatch discovering task does not count)
Origin:       Dispatch scatter E2 vs epilogue serial despite dual 64 KiB stages fitting
```

---

## GAP-012

### Problem

device collective 的 participant set 被当成 launch set。

Agent 冻结了：

```text
block_dim = 32
all launched AIV must call aclshmemx_barrier_all_vec()
```

公共头文件只写 “Barrier of all VEC cores”。

这 Ambiguous：

```text
A. 本次 kernel launch 的 32 个 AIV
B. 当前 PE / SKU 的全部物理 Vector cores
```

64 AIV SKU 上 launch 32、32 个进入 barrier：
若 API 实际等 64 个物理核 → 永久 hang。

“每个 launched core 都进 barrier” 不够。
launch geometry 必须与 collective 要求的 participation set 一致。

### Context

operator-development Gate A P4 / phase-kernel boundary
MoonEP Combine v4：Ascend950 SKU AIV 8/16/56/64/72；Combine `block_dim=32`
CANN 9.1 SHMEM `aclshmemx_barrier_all_vec`
first-party：`shmem/src/device/gm2gm/shmemi_device_cc.h`
  `block_dim = GetBlockNum() * GetTaskRation()`
  ring `% block_dim`
  `ACLSHMEM_MAX_AIV_PER_NPU = 48`

### Classification

Skill

不补新 Knowledge。SHMEM 版本合同会变；要的是 Design 必须证明
scope，而不是把 header 一句 “all VEC cores” 升 fact。

### Existing assets

GAP-003 问能不能单 launch + in-kernel barrier。
P4 问 sync 是不是 scoped。
两者都不问 **谁必须到齐**。

“all launched cores call it” 会被当成 Gate 已过。

### Missing behavior

每个 device collective 必须冻结：

```text
participant scope
participant count
scope kind:     physical cores | launched cores | team members
launch block_dim
idle-core behavior
rank participation
```

证据必须来自目标版本 first-party header、实现注释、或成功调用模式。
公共一句话不够。

```text
A collective cannot pass Design Gate merely because
"all launched cores call it".

launch geometry must be proven compatible with
the collective's required participation set.
```

SKU 支持范围也要写清（例如 ≥32 AIV），不要泛称整个芯片代际。

### Action

P4 / phase-kernel boundary：每个 device collective 交 participation-set 表。
证明不足 → Gate A 不得把 “header said ALL VEC” 标 PASS。

operator-spec-reconstruction：Contract 不持有会被 Design 关掉的
implementation-state（barrier 选哪条、block_dim 多少）。

### Promotion

```text
Status:              VALIDATED
Classification:      Skill
Canonical target:     performance-by-construction.md :: P4 participation-set table
Landed rule:         collective participant set must be proven; launch set ≠ default
Validation:          this Combine: 32 AIV all enter barrier; Host rejects <32
Origin:       MoonEP Combine barrier_all_vec vs SKU AIV count
```

---

## GAP-013

### Problem

Agent 把 **人工输入** 当成可改的任务产物。

write set 原先主要防：

```text
Combine
  → 不准改 AllGatherMatmul / Dispatch
```

没有区分：

```text
Human-authored input
Generated task artifact
Implementation output
```

于是设计和实现不一致时，Agent 会：

```text
把设计文档改成和实现一致
```

人工设计是 **输入约束 / 人工决策**。实现应该满足它。
不能实现完以后反过来篡改输入，让自己看起来「符合设计」。

### Context

operator-development Implementation / Gate B
`docs/*`、用户指定的 architecture/spec、已 review 的 semantic contract
`.ascendweaver/design-proposal.md` 才是可变任务产物

### Classification

Skill

不补新 Knowledge。这是 ownership / mutation 政策，不是 API 合同。

### Existing assets

GAP-010 保护 sibling operator。
它不保护人工设计资料。
Contract vs Design owner 拆分只解决 implementation-state 双写，
不解决「改 docs 消冲突」。

### Missing behavior

开工前分类每个相关文件：

```text
A. immutable human input
B. mutable Agent task artifact
C. generated implementation output
```

默认不可变：

```text
docs/* human design
user-provided architecture / spec
reviewed operator-contract (semantic freeze)
reference diagrams
```

可变：

```text
.ascendweaver/design-proposal.md
.ascendweaver/development-status.md
generated reports
```

```text
Agent MUST NOT edit an immutable human input
unless the user explicitly authorizes that file.

If implementation evidence conflicts with an immutable design:
  1. report the conflict
  2. cite implementation / API evidence
  3. propose a design-document patch separately
  4. stop structural implementation if the conflict is blocking

Implementation must converge toward the design,
not rewrite the design to match the implementation.
```

Implementation 前必须打印 READ-ONLY INPUTS / WRITEABLE DESIGN ARTIFACTS /
IMPLEMENTATION WRITE SET。

### Action

operator-development 增加 **Input mutation policy**。
Gate B：`git diff --name-only` 对照三类名单，改了 immutable human input
且无用户点名授权 → Gate B fail。

### Promotion

```text
Status:              LANDED
Classification:      Skill
Canonical target:     skills/operator-development/SKILL.md :: Input mutation policy
skills/operator-spec-reconstruction/SKILL.md :: 证据分级
Landed rule:         immutable file ≠ every sentence is ABI; grade fact vs sketch
Validation:          pending for sentence-grading; file-not-edited observed on Combine
Origin:       MoonEP Combine rewriting docs to match implementation
```

---

## GAP-014

### Problem

Agent 会停在「这个 API 合法、目标 CANN 存在」。

性能工程还要问：

```text
这个仓库在同类数据路径上实际上怎么做？
```

典型失败：remote GM→UB 只因为

```text
shmem_ptr 能翻译远端地址
DataCopyPad 能搬
```

就冻结，而不比较仓内已成功的 SHMEM get/put idiom
（以及 `aclshmemx_mte_get_nbi` / `aclshmem_getmem` 等候选）。

合法 generic 实现自动赢过 project-proven idiom。

### Context

operator-development Design Proposal / Gate A
性能关键：GM↔UB、remote SHMEM、barrier/signal、atomic、
vector reduction、format conversion、comm/compute overlap
GAP-008：abstract/legal API ≠ physically realizable
（例如 `aclshmemx_mte_get_nbi` 可能占用
`ACLSHMEM_INTERNAL_UB_BUF_START_ADDR = 188 KiB`）

### Classification

Skill

不补新 Knowledge。仓库 idiom 随目标仓变化；要的是搜索与比较义务。

### Existing assets

docs-search / 头文件能回答 API 是否存在。
GAP-008 问落地 footprint，不问「本仓怎么写」。
GAP-010 说 sibling 只读，容易被读成「不要看 sibling」。
要看 idiom，不要抄整个 sibling 架构。

### Missing behavior

冻结性能关键 API 前搜 target-repo 成功实现。证据排序：

```text
1. same repo + same SOC + same CANN
   + same transport direction / pipeline role
2. same repo + same hardware generation + similar dataflow
3. target-version first-party example
4. API documentation
5. generic implementation invented from API legality
```

每个候选比较：nonblocking、address translation、是否占 SHMEM internal UB、
completion、能否与 Vector overlap、event/handle、granularity、
本仓成功场景、目标 SOC。

```text
A legal generic implementation must not automatically win
over a project-proven idiom.

If the selected API differs from the strongest local pattern,
Design Proposal must explain why.

Reuse the implementation idiom,
not the entire operator architecture.
```

换 API 若改变 UB scratch / event / completion，重开 UB + pipeline Gate。

### Action

operator-development 增加 **Performance-critical mechanism selection**。
Gate A：性能关键路径缺 sibling idiom 搜索表 → 未回答。
偏离最强本地模式无 justification → 不得冻结。

### Update (2026-08-25) — strongest local idiom is the default baseline

v5 Design §9.1 搜过 `aclshmemx_mte_get_nbi`，仍冻结 mapped `aclshmem_ptr` + `DataCopyPad`。
搜索完成被当成 GAP-014 已过。真正缺的是 **default priority**，不是「有没有搜」。

```text
search complete
  ≠
idiom selected

same repo + same SOC + same CANN
+ same data direction + same pipeline role
  → that successful idiom is the default baseline
```

偏离必须证明至少一项：

- API contract 不适用（方向 / landing / completion 对不上）
- 多了资源或同步成本（silent ub_limit、internal UB bounce）
- pipeline envelope 更差
- 目标版本实现确证等价 **且** 走更低层路径有具名收益

只写「搜过、比过、我仍选 generic Pad」不够，除非上面有一条成立。

匹配的是 **transport role**，不是 sibling 算子名：

```text
remote PUT UB/GM→peer GM     → Dispatch `mte_put_nbi`
remote GET peer GM→this UB→V → GET-class ops（prefetch gm2gm / grad_reduce ub2gm）
local GM↔UB                  → mapped Pad 可能就是 idiom
```

不要因为「Dispatch 用 Pad 做本地 hidden」就把 Combine 的 **remote GET** 写成 Pad。
也不要把「9.1 某 overload 内部也是 Pad」自动当成可以忽略 GET idiom；
那只证明 **这一版** GET 可能等价，等价本身仍要写进偏离证据。

不补新 Knowledge。不新开编号。

### Promotion

```text
Status:              LANDED
Classification:      Skill
Canonical target:     performance-by-construction.md :: Repo-native API Search Gate
skills/docs-search/SKILL.md :: 选 API ≠ 本 Skill 的结论
Landed rule:         same-repo/SOC/role idiom is default baseline; search ≠ selected
Validation:          pending (Combine still mapped Pad for remote GET)
Origin:       MoonEP Combine legal Pad without role-matched idiom default
```

### Update (2026-08-26) — path-scoped resource restrictions

Agent 看见 Prefetch 有 `[188 KiB, 190 KiB)` reserved /
「user UB 停在 188 KiB」，推广成所有 SHMEM kernel 的法律。
后来才证明：所选路径 `aclshmem_ptr` 是地址翻译、`DataCopyPad` 用 caller UB、
`aclshmemx_barrier_all_vec` 走 GM sync，**没有**吃这块 internal scratch。

```text
Same-repo idiom transfers positive implementation evidence,
not automatically all of its resource restrictions.

Resource constraints must be scoped to the exact API/path
that consumes the resource.
```

```text
Prefetch 用 gm2gm mte helper     → 188 KiB bounce 纪律
  ≠
Combine 用 aclshmem_ptr + Pad    → 也 188 KiB user cap
```

sibling 的 UB 上限、scratch 洞、`ub_limit` 只有在 **同一 API 被调用**
时才是证据。换路径必须重新证明谁消费那块资源。

188 KiB 误推广并进本条，不开新 Gap。参数级 footprint 见 GAP-017。

### Promotion (extension)

```text
Status:              VALIDATED
Canonical target:     performance-by-construction.md :: Repo-native API Search Gate
Landed rule:         sibling resource caps are path-scoped; do not inherit unused scratch laws
Validation:          this Combine: ptr+Pad+barrier_all_vec; 188 KiB is hole hygiene, not Prefetch user cap
Origin:       Combine almost inherited Prefetch 188 KiB onto ptr+Pad+barrier_all_vec
```

### Update (2026-08-27) — resource-restriction provenance tuple

path-scoped 已 VALIDATED（Combine 没把 GET helper cap 套到 ptr+Pad）。
另一次算子仍把 GET 路径的 user cap 套到 PUT + barrier 路径，并用「更小更安全」
覆盖 Frozen Design 的更大 target-specific envelope。Gate B 把较小 cap 判 MATCH。

失败分流：014 已 LANDED/VALIDATED，Agent 经过 Gate 仍错 → **收紧同号**。

继承限制前必须填：

```text
SOC + CANN + exact overload + direction
+ src/dst address space + transport + pipeline role + phase/lifetime
```

tuple 不一致不得继承。
「另一条 SHMEM 路径用过这个 cap」不是证据。
「更小更安全」不是证据。
first-party 与 Frozen Design 冲突 → STOP，不得静默改 Frozen。

分层：physical hardware ≠ compiler allocatable ≠ framework budget
≠ API-internal reservation ≠ caller-owned resource。

本条收紧 014，不开新号。不把任何具体 KiB 写成通用常量。

### Promotion (extension-2)

```text
Status:              LANDED
Canonical target:     performance-by-construction.md :: Kernel Resource Budget / Repo-native API Search Gate
Landed rule:         inherit resource cap only when provenance tuple is compatible; safer-smaller is not proof
Validation:          pending (next isolated clean-room after this landing; Dispatch discovering task does not count)
Origin:       Dispatch inherited GET-path user cap onto PUT+barrier despite Frozen larger envelope
```

---

## GAP-015

### Observation

MoonEP Combine v5 production path 引入了算法维 H tile：

```text
ChooseHTile(H, ub_cap)
for (h_off = 0; h_off < H; h_off += h_tile)
```

任务合同 / typical shape 是 `H=7168`。该 H 的 dual-in/out pipeline 远小于
3510 典型 248 KiB UB。`h_tile == H`，循环只走一次。

同一文件的 `IssueCopyPadGmToUb` 已经按 `DataCopyPad` 单次上限切 DMA。
因此 Pad 65504 B **不是** 把算法 H 抽象成 tile 的理由。

Agent 的失败模式：发现理论上可能存在超长 H / 罕见 dtype / 超大 K，
就把所有 common-case 代码 tile 化。exceptional shape 污染 hot path。

同一实现还有：

```text
if (wgt_bytes + 32 > meta_bytes)
    with_weight = false;
```

调用方要求 gather 权重，kernel 因 UB 紧悄悄改语义。

```text
exceptional-shape generality
  ≠
baseline hot-path structure

DMA max-burst chunking
  ≠
algorithm-axis tile

resource overflow fallback
  ≠
silent semantic downgrade
```

### Context

operator-development Design Proposal / Implementation
tiling / physical realizability
MoonEP Combine v5 `ChooseHTile` / consume `with_weight` 静默关闭
GAP-005（跨核 occupancy H-split；本条是 intra-core hot-path 抽象）

### Classification

Skill

不补新 Knowledge。tiling-from-shape 已有 proposal→capacity→revise；
缺的是「common-case 过容量时不要先引入 tile 变量」。Card 只加一句指针。

### Existing assets

GAP-005 禁止为填满 AIV 拆 token/H 到别的核。Agent 可以遵守 GAP-005，
仍在单核内给每个 group/token 套 `h_tile` 循环。

copy Card / GAP-008 要求 DMA 切块合法。Agent 把切块升成算法调度对象。

P-gates 没有「fallback 不得改 Contract 语义」的 Implementation 检查。

### Missing behavior

若任务合同 / production shapes 在资源约束下可以 full inner axis：

```text
baseline = full-H (or full natural inner axis)
no h_tile field in hot path
no h_off loop
no H-tile scheduling
```

只有同时满足才设计 H-chunk / tile **fallback**（独立路径，不是把主路径 tile 化）：

1. 已知 H 超过 UB（缩完 metadata window 之后仍装不下）
2. 或任务合同明确要求超长 hidden / 该 inner axis

DMA helper 内部 while-chunk 是 copy adapter，不出现在算法控制流里。

同类：不要为未提出的 tail / rare dtype / 超大 K 给 hot path 加抽象。

```text
If with_weight / optional output / required dtype is in the Contract
THEN resource pressure may:
  shrink a window
  revise the physical map
  Host-reject unsupported shape
MUST NOT silently drop the feature
```

### Action

Design Proposal 增加 **hot-path vs exceptional shape** 轴。
Gate B：common-case 可 full-H 仍引入 `h_tile`/`h_off` → PERF-SMELL-10 unjustified
（与 occupancy 拆核分开记账：本条是 intra-core 抽象污染）。
fallback 静默改 Contract 语义 → Implementation Gate 失败，不是 smell 能 justify 的。

不并进 GAP-005。不补新 Knowledge 文件。

### Promotion

```text
Status:              LANDED
Classification:      Skill
Canonical target:     performance-by-construction.md :: Hot-path vs Exceptional Shape Gate
Landed rule:         production full-axis hot path; DMA chunk ≠ algorithm tile; fallback must not drop semantics
Validation:          pending
Origin:       MoonEP Combine v5 ChooseHTile / silent with_weight=false
```

---

## GAP-016

### Observation

MoonEP Combine v5 consume 的 hidden 路径已是真 prefetch + 独立 store ping/pong。
weight 却在每个 token 的 hidden `StoreTile` **全部结束之后** 才：

```text
for k: remote 4B DataCopyPad
Wait MTE2_S
Scalar pack
Wait S_MTE3
CopyOut weights
Wait MTE3_MTE2
```

timeline：

```text
===== hidden token t =====
MTE2 / V / MTE3
                         ===== weights t =====
                         tiny MTE2 × K
                         Scalar pack
                         MTE3
                                              token t+1
```

weight 是小而频繁的 side path（每 token K 次 4B），hidden 是大 payload + 长 Vector。
Gate 分别审查两条流水都可能 PASS；联合 timeline 上 weight 整段暴露在 critical path。

`Wait MTE3_MTE2` 还把 weight store 完成挡在下一个 token 的 hidden MTE2 前面，
即使 buffer 并不 alias。

```text
independent phase pipeline review
  ≠
cross-subpath schedule

dominant path complete
  then
side-path epilogue
  ≠
size-interleaved pipeline
```

### Context

operator-development Gate A P3 / Steady-state
MoonEP Combine v5 `ProcessConsume` hidden then weight
GAP-004（per-phase coverage；本条是 dominant∥side 联合排程）

### Classification

Skill

不补新 Knowledge。不新开 PERF-SMELL 编号；用 PERF-SMELL-04 记联合 timeline 缺失。

### Existing assets

GAP-004 要求 prologue/epilogue 只要含两 pipe 就独立画 timeline。
Agent 可以给 weight 单独画一条串行 timeline 并标 `present + justified`（irregular gather）。
缺的是 **和 hidden 画在同一张图上**，问能否进 V-heavy 气泡。

P3 / GAP-011 问 buffer/credit 够不够飞 hidden 三流水，不问 side-path 占用哪个气泡。

### Missing behavior

算子同时有 dominant payload path 和 small high-frequency side path 时：

```text
do not review their pipelines only independently.
Build a cross-subpath timeline.
```

典型：

```text
dominant: hidden row movement + Vector reduction
side:     weight gather / scale / metadata / flag
```

必须审查 side work 能否进 dominant 的 idle slot：

```text
MTE2 side traffic during V-heavy periods
Scalar side work during V / MTE
MTE3 side output during next item's MTE2 / V
```

MTE2 不能同时发 hidden 和 weight。要求不是「两路 DMA 真并行」，
而是 **不要在 hidden 全做完以后再集中追加一串 tiny MTE2**。

side path 默认串在 dominant 之后 → 必须有 cost model 或 profiling。
无 justification → PERF-SMELL-04 unjustified。

### Action

P3 / Steady-state 增加 **cross-subpath timeline**。
Design Proposal pipeline 轴：dominant + side 必须联合作答，缺则 **未回答**。

不并进 GAP-004。不根据本条报加速比。

### Promotion

```text
Status:              LANDED
Classification:      Skill
Canonical target:     performance-by-construction.md :: Side-path Integration Gate
Landed rule:         dominant + small high-frequency side path need a joint timeline
Validation:          pending
Origin:       MoonEP Combine v5 weight serial after hidden
```

---

## GAP-017

### Observation

MoonEP Combine 已能判断 remote GET 该用哪类 API（GAP-014），却写出：

```text
DataCopyPad(
    slot[k * 8],
    src,
    DataCopyParams{1, 4, ...},
    DataCopyPadParams{}
)
```

4B weight source。Agent 知道 API **选对了**，没有立刻证明：

- 4B source → destination footprint 到底多少？
- 是否真的 pad 到 32B？
- 下一个 slot `+32B` 是否安全？
- 默认 `DataCopyPadParams{}` 的语义是什么？

CANN 9.1 `CopyGmToUbufAlignV2`（dav_3510）occupancy 是
`AlignUp(dstStride*32 + burstLength + padLength, 32)`。
额外 `rightPadding` 可以把 occupancy 抬到 64B 并撞下一个 slot。
默认 `{}` 不是证据。

```text
correct API
  ≠
correct call

API selection
  ≠
parameter-level legality / physical effect
```

### Context

operator-development Gate A（Repo-native API Search 之后）/ Gate B
copy Card（dest 对齐 / effective vs aligned）
MoonEP Combine 4B weight `DataCopyPad` + `DataCopyPadParams{}`
GAP-008（landing footprint）；GAP-014（选哪套 API）

### Classification

Skill

不补新 Knowledge。copy Card 已有 dest alignment。
缺的是「冻结 API 之后必须证明 **这一次调用**」。

### Existing assets

GAP-014 问这条路径默认用哪套 API。Agent 可以答对 API 名，仍填错
`blockLen` 单位、pad 参数、dst stride。

GAP-008 问 DMA landing layout。Agent 可以在 Design 写 `K×32B slots`，
实现仍用 `DataCopyPadParams{}` 而不证明 occupancy。

### Missing behavior

对性能关键或非自然对齐的数据搬运，冻结 API 后还必须验证：

```text
concrete overload
units (bytes vs elements)
blockLen / burstLength
src/dst stride
padding / pad count
destination footprint
alignment
event / completion / sync_id
UB limit consumed by this call
```

尤其下面这些必须一级证据（target-version header / 实现 / 成功调用）：

```text
bytes vs elements
uint16 vs uint32
pad count
dst footprint
alignment
sync_id
UB limit
```

`DataCopyPadParams{}` / 默认构造 **不是** 一级证据。

### Action

performance-by-construction 增加 **API Invocation Contract Gate**（Gate A，
紧跟 Repo-native API Search）。Gate B 从实际实参重建 footprint。
PERF-SMELL-12 记「landing 声称与 Pad 参数不符」。
copy Card 只补 invocation ≠ selection 原则，不展开 occupancy 公式。

不并进 GAP-014。不根据本条报加速比。

### Promotion

```text
Status:              LANDED
Classification:      Skill
Canonical target:     performance-by-construction.md :: API Invocation Contract Gate
knowledge/ascendc-api/data-movement/memory-copy-and-alignment.md :: Invocation ≠ selection
Landed rule:         API selection ≠ parameter-level legality / physical effect
Validation:          pending (next non-Combine transfer; discovering task does not count)
Origin:       MoonEP Combine 4B weight DataCopyPad + default PadParams
```

---

## GAP-018

### Observation

Combine Consume 的 Design 语义天然是：

```text
for k in K:
    if positive:
        consume
```

理论上 O(K)。实现写成：

```text
先扫描 K 统计 n
for r in positive_count:
    再从 k=0 扫描找到第 r 个 positive
```

变成 O(K²)。没有增加 GM traffic；每次 `GetValue` 都在 UB。
普通「不要重复 GM 访问」抓不到。

数据已经驻留 UB，不代表重复遍历免费。
repeated Scalar traversal 还会推迟下一个 MTE2 的 issue，破坏 pipeline lead time。

```text
Resident metadata
  ≠
free repeated control work

Traversal intent
  ≠
iterator complexity
```

### Context

operator-development Gate B / P5
MoonEP Combine `CombineRemotePrimarySrc` count-then-rescan
GAP-007（消扫描是 producer invariant → span；本条是 **hot-loop iterator**）

### Classification

Skill

不补新 Knowledge。不是 locality / bulk DMA 的同义复述。

### Existing assets

P5 / PERF-SMELL-02/08 盯 GM reload 和 loop-invariant recompute。
Agent 可以把 K 个 weight 一次搬进 UB，然后用 helper 抽象把单遍消费
写成「先数再从头找第 r 个」。

GAP-007 消的是 **可由头尾推导的 span 扫描**。本条消的是
**逻辑上单遍的 control traversal 因 iterator 退化成 repeated scan**，
即使没有 compact/prefix invariant。

### Missing behavior

对 hot loop 中的 metadata / control traversal，Gate B 必须重建
**实际访问复杂度**。若逻辑数据只需单遍消费，implementation 不应因
helper abstraction 退化成 repeated scan / O(N²)，即使数据已在 UB。

```text
logical consume:     one pass over K
implementation:      count pass + r × scan-from-0
  → Gate B fail unless the extra passes are proven necessary
```

正当理由必须写明：为何不能单调 cursor / 单次 walk；额外 Scalar 工作
如何不推迟下一发 MTE2。

### Action

PBC 增加 **Hot-loop Traversal Gate**。Gate B 从实际循环嵌套重建
复杂度。PERF-SMELL-08 记 repeated UB scan / O(N²) helper。

不并进 GAP-007。不根据本条报加速比。

### Promotion

```text
Status:              LANDED
Classification:      Skill
Canonical target:     performance-by-construction.md :: Hot-loop Traversal Gate
Landed rule:         resident metadata ≠ free repeated control traversal
Validation:          pending (next non-Combine transfer; discovering task does not count)
Origin:       MoonEP Combine CombineRemotePrimarySrc O(K²) rescan
```

### Update (2026-08-26) — whole-call-graph multiplicative reconstruction

原规则抓住了「helper 把单遍变成 rescan」。Gate B 仍可能：

```text
看 ProcessP2 主体
→ 给一个复杂度标签 O(R·E)
```

而 actual code 是 `for d<R: for g<E: Helper()`，Helper 自己 `for b<B`。
Traversal intent ≠ traversal complexity。本条升级成 **Gate B 可执行手续**：
对每条 hot-path callsite 沿完整可达 helper 链相乘。

本条收紧 018，不开新号。

### Promotion (extension)

```text
Status:              LANDED
Canonical target:     performance-by-construction.md :: Hot-loop Traversal Gate / Gate B.5
Landed rule:         outer × helper × nested × helper-of-helper; not caller-body labels
Validation:          pending (gate_b_static_attack；eval 已移除)
Origin:       Planning V2 MembershipPrefetch O(B) inside O(R·E)
```

---

## GAP-019

### Observation

Design 已说 production full-H only、large H unsupported。
Kernel 也真的不再有 H tile。Host 若仍只检查：

```text
H * 2 <= 128 KiB
```

（Dispatch 的 row cap），而不检查 E3：

```text
dual-in + dual-out + acc + tmp + metadata  <=  supported UB
```

则「unsupported」没有闭环：

```text
Kernel:  I don't support this shape.
Host:    Come on in.
```

最后仍是 kernel OOB。

```text
Kernel support domain
  ≠
Host launch-admission domain
```

### Context

operator-development Design Proposal / Implementation Host
Hot-path vs Exceptional Shape（Host reject 选项）
MoonEP Combine full-row E3；Host 曾可能只沿用 Dispatch `H*2` cap
GAP-015（hot path 不 tile；本条是 **launch 边界必须可执行**）

### Classification

Skill

不补新 Knowledge。适用于 FP4、特殊 H、K 上限、对齐、通信 rank 数。

### Existing assets

GAP-015 允许 exceptional 走 **Host reject**。Agent 可以在 Design 写
「large H unsupported」，Host 却只做 sibling 的弱检查。

tiling field provenance 问 Host 怎么得到字段，不问 Host 检查是否
**覆盖 kernel 假设**。

### Missing behavior

每个 kernel 显式声明 supported domain；Host validation 必须是该 domain
的等价或更严格实现。任何「不支持而非 fallback」的设计决策，都必须在
launch boundary 有 **executable guard**。

```text
Kernel assumption:
  full-row E3 footprint <= UB

        ↓ must map to

Host:
  TORCH_CHECK(E3Footprint(H, K, ...) <= supported_ub)
```

不得只写在 Design 文档里。不得用 sibling 的更弱 cap 代替本 kernel 的域。

### Action

performance-by-construction 增加 **Launch Admission Gate**。
Design 列出 kernel domain → Host check 对照表；Gate B 核对
`TORCH_CHECK` / 等价断言与 kernel 假设一致或更严。

不并进 GAP-015。不根据本条报加速比。

### Promotion

```text
Status:              LANDED
Classification:      Skill
Canonical target:     performance-by-construction.md :: Launch Admission Gate
Landed rule:         kernel capability domain must have an executable Host guard
Validation:          pending (next non-Combine transfer; discovering task does not count)
Origin:       MoonEP Combine full-row E3 vs Host H*2 sibling cap
```

### Update (2026-08-26) — max-footprint closure

原规则抓住了「Design 写 unsupported、Host 用 sibling 弱 cap」。
下一层：lifetime 不重叠的 overlay，Host 仍可能只按 phase A 尺寸分配。
phase B 用 `E+B`、物理 region 是 `R * hist_stride` 时，R=1 越界。

```text
Lifetime compatibility is necessary but not sufficient.
Host admissible domain
  = semantic ∩ API ∩ physical-layout ∩ temporary-footprint
```

每一条实际物理不等式都必须出现在 Host。本条收紧 019，不开新号。

### Promotion (extension)

```text
Status:              LANDED
Canonical target:     performance-by-construction.md :: Launch Admission / Memory Orchestration
Landed rule:         allocated_extent >= max(all phase footprints) over Host domain
Validation:          pending (gate_b_static_attack；eval 已移除)
Origin:       Planning V2 hist UB as cu_seqlens[E+B] when R=1
```

---

## GAP-020

### Observation

Frozen Design 给出了正确的 **logical AIV worker** 公式。
第一版实现把 worker 简化成物理 `GetBlockIdx()`。

随后 remainder 公式在 **错误的 worker-space** 里证明了 cover / union / idle。
数学自洽，所有权仍错。

```text
physical launch index
  ≠
logical worker identity

correct Frozen Design
  ≠
faithful implementation

a partition proof is invalid
until the worker-space model is proven
```

Prefetch 具体形态：frozen Design 已有

```text
AivWorkerIndex() = GetBlockIdx() / GetSubBlockNum()
```

Design reconstruction 用「`core_ratio(0,1)` ⇒ `GetSubBlockNum()==1`」把它**证明为等价**后改写成 `GetBlockIdx()`。实现忠实执行了简化后的 freeze。Gate B 只在错误 worker-space 里证明了 `task % worker_count` 覆盖，标 PASS。

这不是「不知道该除 SubBlockNum」。这是：

```text
正确的 target-specific 公式
        ↓
看起来等价，改写成更短的 GetBlockIdx()
        ↓
Gate B 假设 worker_id 已正确
        ↓
在错误模型里完成漂亮证明
```

Dispatch 迁移了同一类失败：remainder 在物理 block 空间自洽，Frozen remainder 在 logical AIV-worker 空间。
另一次算子实现显式保留 logical mapping，说明规则可迁移；
**发现任务 + 同训练链下一次算子 ≠ 独立 VALIDATED。**

成熟实现保留 `AivWorkerIndex`。同一轮 Oracle-delta：Weaver 在 Host admission、uint64 乘法、`ACLSHMEM_INTERNAL_UB_BUF_START_ADDR` static_assert 上优于成熟实现；两边共享 int32 ceil-div overflow。不得因此把 Golden 整份复读回来。

### Context

operator-development Gate A 分核 / remainder；Gate B / Design Proposal ownership
PBC P1 只问 Scalar/Vector/SIMT，不问 launch→logical ID
GAP-012 问 collective **谁必须到齐**，不问 **谁拥有 work item**
tiling-from-shape §3 core partition
target-specific：`core_ratio`、`GetSubBlockNum`、`GetTaskRation`、`ASCEND_IS_AIC/AIV`
operator-spec-reconstruction 拓扑公式改写

### Classification

Skill（何时检查、Gate B 顺序）+ Knowledge（physical ≠ logical worker）。
不补新 Knowledge 卡片讲某 SoC 的 block 公式。合同随 CANN / 核类型变化；
要的是 **先重建 topology 再写分核**，以及 **禁止把冻结公式合理化简**。

### Existing assets

GAP-012 participation-set：launch set ≠ collective participant set。
Agent 可以填「32 个 launched core 都进 barrier」，仍用 `GetBlockIdx()` 当 logical worker。
Gate B 已有 PERF-SMELL / whole-call-graph / Gate B.5 IAC。
仍可在未验证的 worker-space 里把 stripe 数学标 PASS。
parallel-axis exploration 问切哪根轴，默认 worker_id 已存在。
tiling-from-shape §3 讲核间切轴，不讲 `GetBlockIdx` vs `GetSubBlockNum`。

### Missing behavior

分核公式之前必须重建：

```text
physical launch
  → execution topology
  → subblock topology
  → engine role
  → logical worker identity
  → logical worker count
  → work ownership
```

禁止仅因 Host `<<<N>>>` 写 `worker_id = GetBlockIdx()`。

必须证明：每个 participating logical worker 一个 ID；ID 构成所有权域；
每条 work 恰好一次；idle 合法；collective identity 与该 topology 一致（交叉 GAP-012）。

Design 的 FROZEN / MUST / exact / selected / do not replace 必须逐条对到源码。
mismatch 且无本 launch first-party 等价证明 → Gate B FAIL。
对照成熟实现必须走 Oracle-delta 分类，禁止 `different from Golden → change to Golden`。

### Action

1. PBC **Logical Worker Topology Gate**；Gate B 从 actual `GetBlockIdx` / `GetSubBlockNum` 重建 logical ID
2. Skill `operator-development` Gate B：先 Frozen-Clause Traceability，再 worker topology，再 smell / IAC
3. Knowledge `tiling-from-shape` §3：physical launch index ≠ logical worker
4. Skill `operator-spec-reconstruction`：禁止无 first-party 证据把 topology 公式证明等价后改写
5. Eval：`logical_worker_topology`；Prefetch expected `worker_topology`；Oracle-delta（eval 已移除）
6. 本条 GAP

不并进 GAP-012。不把某一核的除法公式写成绝对规则。
不补独立 Case。这是工序失败，不是新机制。
发现任务本身不算 VALIDATED。

### Promotion

```text
Status:              LANDED
Classification:      Skill + Knowledge
Canonical target:     performance-by-construction.md :: Logical Worker Topology Gate;
                      operator-development SKILL.md :: Gate B Frozen-Clause / worker topology / integer audit;
                      tiling-from-shape.md :: §3;
                      operator-spec-reconstruction SKILL.md
Landed rule:         reconstruct logical worker space before partition math;
                     `worker = GetBlockIdx()` because Host launched N blocks → FAIL;
                     frozen target-specific formulas must not be simplified
                     without first-party equivalence proof
Validation:          pending (next isolated transfer; Prefetch discovery + Dispatch transfer do not count)
Origin:       MoonEP Prefetch AivWorkerIndex simplified to GetBlockIdx; Dispatch remainder-in-wrong-space
```

**GAP-020 = LANDED, not CLOSED.**
Prefetch 发现与 Dispatch 迁移都发生在 canonical landing **之前**，不能升 VALIDATED / CLOSED。

---

## GAP-021

FusedDeepMoe D2 batch-completion 第一次静态实现能编译，NPU 上 **等 completion 收不齐卡死**。
上板迭代修了两处协议/流水缺口，算子才真正跑通并有收益。
发现任务本身不算 VALIDATED。

### Observed

初版已冻结：

- 删 per-token flag / per-token publication `PIPE_MTE3`
- group-end 写 `DONE[src][e][c]`
- 循环后 `PipeBarrier<PIPE_MTE3>` 再复用 `destCount`
- recv 用 `Σ DONE == expected`

仍 hang。NPU 上证明的缺口：

1. **inner-loop 源 UB**  
   `for dest: SetValue(donePub); DataCopyPad(remote, donePub)`  
   只在循环 **后** 等 MTE3。`donePub` 在迭代间被 `SetValue` 覆盖，issued copy 读到脏/空字。对端 `sum != expected` → 死等。看起来像“flag 协议错了”，根因是 **Scalar vs MTE3 的 loop-carried reuse**。

2. **publish 粒度 ≠ wait 准入**  
   DONE 按 **整个 (src, e) bucket** 发布。初版用 recv 切片 `[start,end)` overlap 决定要不要等这个 src。bucket 跨两个 recv 核、或 overlap 坐标假阴性时，该等的核 skip，或不该用切片语义去筛 group 信号。上板改成：进入该 expert 的 recv 核，对所有非空 bucket 等整桶 `expected`。

测试 golden 另有一条（不并进本 GAP 的 API 规则）：small-op 参照必须按 fused `smooth → MX` 顺序自算；内建 dispatch MX 分支不带 per-expert smooth。否则会把 **参照错** 判成 kernel 错。

### Missing behavior

```text
DataCopyPad issued
  ≠
source UB reusable for next SetValue

closed outer buffer MTE3
  ≠
closed every alias used as copy source in the issue loop

consumer slice overlap
  ≠
admission to wait a group-grain completion
```

Gate / debug 必须能问：

- 每条 issued `DataCopyPad` 的源 UB，下一次 Scalar/Vector 写之前有没有 `MTE3_S`（或换槽）？
- waiter 的 `expected` 单位是否就是 publisher 写出去的那一粒？

### Action

1. Knowledge `pipeline-events-and-ordering`：loop-carried `DataCopyPad` 源寿命
2. Skill `operator-debug` hang：远程 flag 收不齐先拆 publish-corruption vs grain-mismatch
3. Skill `operator-development` MUST NOT 两条（GAP-021）
4. 本条 GAP

不把“一定要用 per-token flag”写成规则。不把 D2 方案本身升成 Knowledge。

### Promotion

```text
Status:              LANDED
Classification:      Skill + Knowledge
Canonical target:     pipeline-events-and-ordering.md :: loop-carried DataCopyPad source UB;
                      operator-debug SKILL.md :: hang remote-completion;
                      operator-development SKILL.md :: MUST NOT GAP-021
Landed rule:         inner issued copy 的源 UB 必须在下一写之前完成；
                     waiter expected 粒度必须等于 publisher 粒度
Validation:          pending (this FusedDeepMoe discovery does not count)
Origin:       FusedDeepMoe D2 first-gen hang on NPU; MTE3_S + whole-bucket wait
```

**GAP-021 = LANDED, not CLOSED。** 发现任务不得升 VALIDATED。

## GAP-022

FusedDeepMoe 上板对话的核心缺口 **不是不懂算子、也不是不懂 AscendC**，而是没有固化的 **NPU Operator Iteration**（实验科学）：

```text
改代码 → 猜 → 改 → 编 → 装 → 跑很大的 e2e → 再猜
```

后半段才摸到：隔离 FusionOp、2 卡再 4 卡、无 profiler 的决定性实验。前半段是在临时发明实验。核协议缺口仍归 GAP-021。本条收 **Dev Loop / 状态机**。

发现任务本身不算 VALIDATED。不开同义 GAP-023。

### Observed

1. **没有固定闭环**  
   安装路径 / `ASCEND_CUSTOM_OPP_PATH` / torchair / CANN 路径摸很久；编过、import 过，仍不确定运行时加载的是不是这份包。

2. **测试对象没分层，归因错**  
   完整测试挂了 → 怀疑新特性 → 怀疑 HCCL → 怀疑 PR。507015 出在 synchronize 后就被当成 DONE hang；baseline 同样失败才洗清。有的错误发生在 SmallOps，FusedDeepMoe 还没执行。

3. **CPU 式读 Python 栈**  
   `synchronize` 报 507015 被当成那一行有问题。blocking 才看到 AI Core 340。Python stack ≠ fault location。

4. **实验变量一次改太多；装置本身坏了还推硬件结论**  
   `python -c` + `mp.spawn` 不能 pickle，却推出 “HCCL 不认 visible devices”。推测 `aicNum=64` 推出 FALLBACK，probe 后是 32，整条根因作废。

5. **先 patch 后找证据**  
   静态看出 `SetValue`/`DataCopyPad` 复用就连出方案 A/B、对齐回退，再发现 `destCountTensor[dest]` 的 4B 偏移。缺少 Hypothesis / Observable / Expected A/B。

6. **harness 没冻结**  
   两棵树、两份测试脚本、smooth 路径不同、warm/iter 入口不同。同时迭代 operator + harness + env。

7. **正确性 / 稳定性 / 性能搅在一起**  
   大 bs + 20+100 + msprof 第一趟，超时被当成死锁。同配置无 profiler 跑通后才证明至少不是逻辑死锁。

8. **上板卫生**（仍有效，不是主条款）  
   占卡、抄 `HCCL_BUFFSIZE=15000`、timeout 残留、`/tmp` 满、未 admitted 就改协议。

### Missing behavior

```text
用户说「改了 kernel，测一下」
  ≠
允许 build + 目标 e2e + msprof

完整 e2e
  ≠
第一定位工具

synchronize / 507014 / 507015
  ≠
fault location 或「可以怀疑 kernel」

推测的 aicNum / 洞
  ≠
runtime fact

静态“看起来有竞态”
  ≠
可以 patch

profiler
  ≠
correctness debugger
```

Agent 必须先能回答：这个结论是哪一个 A/B、哪一层 FIRST BAD 证明的？

硬 invariant（卡字段，不是百科）：

```text
BINARY     BUILD ID / OP COMMIT / TEST COMMIT / LOADED OPP
           任一项变 → L2+ PASS 全部失效，回到 Install → Load

DIAGNOSTIC 证据卡后只改一个主变量
           device | world_size | shape | feature flag | binary | profiler
           改两个及以上 → NON-DIAGNOSTIC，只许探索，不许根因
```

### Close condition（看行为，不看文档是否写全）

**禁止**因“Skill/Knowledge 写完了”升 VALIDATED / CLOSED。  
发现任务（本 FusedDeepMoe 对话）不算。

至少 **2～3 个不同真实算子问题** 的 clean-room 回放，Agent **主动**出现下面全部行为，才可 VALIDATED；再确认无副作用才 CLOSED：

```text
1  用户一句「改了 dispatch，帮我测」
   → 先建 ITERATION 卡，不直接跑目标测试
2  L6 失败且 L3–L5 缺失
   → 回退补证据，不猜 HCCL / 卡 / kernel
3  synchronize 报 507xxx
   → 不把 Python 栈当 fault site
4  aicNum / tiling key / offset / admitted
   → 先 dump，再推导
5  怀疑 kernel
   → 先 Hypothesis / Observable / Minimal test / Expected A/B
   → 无 Observable 不 patch
6  改核 / 换 OPP
   → PHASE 退回 Install → Load；旧 L2+ PASS 不跨 BINARY 继承
7  correctness 未过 L8
   → 拒绝把 msprof 当定位手段
8  一次改两个及以上主变量
   → 卡标 NON-DIAGNOSTIC，不拿来下根因
```

第一次违规的地方记回本条 Observed，收紧 Skill checklist，**不开同义 Gap**。

下一步：拿真实任务压状态机，不要再往里塞 AscendC / HCCL 百科。

### Action

1. Skill `operator-iteration`：状态机 + ITERATION 卡 + BINARY / one-variable
2. Knowledge `npu-on-device-validation`：L0–L9、异步定位、runtime facts、证据卡、F/S/P
3. `operator-development` / `operator-debug` / `operator-optimization` 路由与 MUST NOT
4. 本条 GAP（升级，不开 023）

不把 D2 / DONE 方案写成规则。不把 `npu-smi` 写成蓝区思考门槛。不把 env_check.sh 当成必须新建的仓内文件。

### Promotion

```text
Status:              LANDED
Classification:      Skill + Knowledge
Canonical target:     skills/operator-iteration/SKILL.md;
                      npu-on-device-validation.md;
                      operator-development / operator-debug / operator-optimization :: GAP-022
Landed rule:         上板走状态机；e2e 不是第一 locator；
                     fact 先 dump；无证据卡不改核；
                     BINARY 变则 L2+ 作废；
                     多变量 run = NON-DIAGNOSTIC；
                     profiler 不是 correctness 第一步
Validation:          pending — 需 2～3 个不同算子 clean-room 满足 Close condition 1–8
                     （本发现任务不算）
Origin:       FusedDeepMoe NPU Agent log + user replay: 短板是实验科学
```

**GAP-022 = LANDED, not CLOSED。** 文档写完 ≠ 可关。行为没复现之前不要升 VALIDATED。

## GAP-023

FusedDeepMoe Primary-Dedup Hybrid 第一次静态实现能编译，**第一发往往能跑完**，从第二发起 Recv 核在 completion 自旋里卡死。

这不是 GAP-021（同一次 launch 源 UB 被下一轮 `SetValue` 踩掉），也不是 GAP-022（上板实验科学）。本条收 **跨 launch leftover / 假就绪**。

发现任务本身不算 VALIDATED。不开同义 GAP-024。

### Observed

初版已冻结：同一次 launch 内 handshake overlay 在后级 payload 上（lifetime 不重叠 + max-footprint 过了）；one-shot 完成字不 consume、不双 bank；第一发 overlay 多为 0，ready 窗对不上 `0.0`。

仍 hang。NPU 上证明的缺口：

1. **下一发把 leftover 认成 ready**  
   完成字叠在 Combine / 输出窗。第一发 Combine 把激活写进同一物理字节。第二发 waiter 按 `dataState` 的 FP32 `1.0`/`2.0` ± 0.5 采样，leftover 比特掉进窗 → **假就绪**。

2. **假就绪会跳过真 publish**  
   waiter 在 ready 时推进 ordinal / 重建。垃圾 meta 写入错误 slot 或跳过真 one-shot → `remaining` / expected 永远到不了 → 看起来像 flag 协议 hang。

3. **第一发 PASS ≠ leftover-safe**  
   只跑一次会漏检。`dataState` 翻转或后级 smash 之后的第二发才是这条协议的最小稳定性。

4. **plan count vs 静默丢 publish**（次一级，第一发也可能挂）  
   dest 按 plan 等待；publisher cap 超了就 `return`。planned wait ≠ actually published。

### Missing behavior

```text
one-launch lifetime non-overlap
  ≠
cross-launch leftover-safe overlay

handshake / completion word
  ≠
payload a later phase of the same kernel will smash

published one-shot flag
  ≠
consumed, bank-flipped, or zeroed before the next launch samples it

value in this launch's ready window
  ≠
identity of this launch's publish

第一发 PASS
  ≠
flag 协议 leftover-safe
```

Gate / debug 必须能问：

- 下一发 waiter 采样前，这平面是 consume、双 bank，还是显式清零？
- 同 kernel 后级会不会写同一物理字节？
- leftover 编码会不会掉进下一发 ready 窗？
- 假就绪会不会让 waiter 跳过真 ordinal？
- dest 按 plan 等时，publisher 会不会静默丢 one-shot？

### Action

1. Knowledge `implementation-adversarial-statics` §8：handshake leftover
2. Knowledge overlay 第四合取：leftover identity
3. Skill `operator-debug` hang：第三条假说 leftover / false-ready
4. Skill `operator-development` MUST NOT 三条（GAP-023）
5. PBC Memory Orchestration / IAC 负向搜索 / PERF-SMELL-12 leftover
6. L8 / iteration：新 flag 至少 2 launch
7. 本条 GAP

不把「必须 per-token flag / 必须双 bank / 禁止 scratch overlay」写成规则。不把 Primary-Dedup 方案升成 Knowledge。

### Promotion

```text
Status:              LANDED
Classification:      Skill + Knowledge
Canonical target:     implementation-adversarial-statics.md :: §8 leftover identity;
                      operator-debug SKILL.md :: hang leftover/false-ready;
                      operator-development SKILL.md :: MUST NOT GAP-023;
                      npu-on-device-validation.md :: L8 ≥2 launch
Landed rule:         下一发看到的 handshake 字必须是这一发的 publish；
                     同 kernel 后级 payload 不得把 leftover 踩成 ready；
                     第一发 PASS 不是 leftover-safe
Validation:          pending (this FusedDeepMoe discovery does not count)
Origin:       FusedDeepMoe Primary-Dedup Hybrid first-gen hang on NPU;
              first launch OK, subsequent Recv remaining never hits 0
```

**GAP-023 = LANDED, not CLOSED。** 发现任务不得升 VALIDATED。

