# Operator-debug promotion verdict

status: **accepted**

对照：

- CANNBot @ `c4c425641d11127945d22e5a752fcad77646be97`
  - `ops/ascendc-precision-debug/`
  - `ops/ascendc-runtime-debug/`
  - `ops/ascendc-crash-debug/`
- Weaver 诊断模型：[operator_debug_contract.md](operator_debug_contract.md)
- 已 `accepted` 的 Batch 1–2 Knowledge（不重写）

本文件只决定 **从三套流程里抽什么、改写成什么、丢掉什么**。Skill 按 [contract](operator_debug_contract.md) 写短路由，不在这里摊开手册。

```text
debug heuristic
  cannot override
accepted API contract
```

```text
CANNBot says X
    ↓
    ├─ 已是 accepted Knowledge     → link，不 promote
    ├─ 调试排序 / 手段              → Skill 路径或 references
    ├─ 把假说写成了根因             → rewrite 后再用
    └─ 工具手册 / 缺省容差 / SIMT   → none（本轮）
```

---

## 总判

CANNBot P2 的价值是 **症状分流 + 尝试纪律**，不是又一套 API/硬件事实。

```text
值得留
  先分桶再动手
  先最小复现
  错误码当入口
  hang 与 crash 手段不同
  无信息增益就换策略

必须改写
  症状表里的“可能原因”
  507035 的“根因分类”
  “全 0 → 加 EnQue”
  “>7 次”
  DataCopyPad.blockLen 必须 32B（与 copy Card 冲突）

不要搬
  DumpTensor 7 步、mssanitizer 安装、plog 脚本
  缺省 rtol 表、SIMT 精度、直调 7 Agent
```

三份 Skill **不要**做成 Weaver 的三个并列 Skill。压成一个路由器，四条路径。

```text
cannbot
  precision-debug  ─┐
  runtime-debug    ─┼─→  skills/operator-debug/SKILL.md
  crash-debug      ─┘         ├─ runtime-error
                              ├─ wrong-result
                              ├─ hang
                              └─ crash
```

`ascendc-precision-debug` 是 **wrong-result 路径的输入**，不决定 Weaver 的路径名。  
crash-debug 已经比较像路由器；precision-debug 是一本手册，默认膨胀源。

```text
wrong result
  ≠
precision problem
```

---

## 和 contract 的对齐

| contract 要求 | CANNBot 现状 | 处理 |
| --- | --- | --- |
| symptom ≠ root cause | precision 症状表把全 0 写成同步/对齐/SetValue | rewrite：排序 |
| error code ≠ root cause | runtime 把 507035 写成“高置信度 = Pad 未 32B” | rewrite：假说树 |
| hypothesis ≠ Knowledge fact | “DataCopy 后必须 EnQue/DeQue”写成唯一合法实现 | **rewrite**：跨流水 dependency 必须显式表达；TQue 是默认范式，HardEvent 是另一条合法机制 |
| fix ≠ validation | precision 流程第 7 步有验证；入口树经常停在“加上就好” | 强制 retest |
| blind retry ≠ debugging | 有“>7 切二分”，但是计数器 | 改成信息增益 |
| hang ≠ crash | 塞在同一个 Skill | 拆路径 |
| 复用 Batch 2 | 三套流程各自重讲 copy/pipe/buffer | Skill 只链 Card |

---

## 按 Skill 压缩

### 1. `ascendc-precision-debug`

喂给 Weaver 的 **`wrong-result` 路径**，不要建成 `precision` 路径。  
主体是 workflow。能独立存在的规律很少，而且 Batch 2 已经写过。

| 内容 | kind | 处理 | 去哪 |
| --- | --- | --- | --- |
| 先固定最小复现再改代码 | workflow | **keep** | Skill Decision Rules |
| 禁止凭直觉改 | workflow | **rewrite** | 禁止的是无证据修改，不是禁止排假说 |
| 改完输出不变 → 清 cache | workflow | **rewrite** | stale-binary **假说**；不是第 0 步仪式 |
| 全 0 / 随机 → EnQue → 32B → Get/SetValue | rule | **rewrite** | 与 pipeline Card 同序的 **recommendation** |
| `PIPE_ALL` 变对 ⇒ 就是同步，改为 EnQue | workflow | **rewrite** | readiness 假说升权；拆掉 `PIPE_ALL`；回到显式跨流水合同（优先 TQue，HardEvent 也可） |
| CopyIn→CopyOut 不计算 | workflow | **rewrite** | 只给地址 / copy 参数 / effective·aligned / 基本搬运降权；**不**给 MTE2→V / V→MTE3 readiness 降权 |
| FP32 过、FP16/BF16 挂 | rule | **rewrite** | 中间精度 / Cast / Cast 后同步 / castBuf **排序** |
| BF16 过、FP16/FP32 挂 | recommendation | **keep（瘦）** | fallback 路径 vs 阈值，仍是假说 |
| 无信息增益后切二分 / Dump | workflow | **keep（改判据）** | 不要 “>7” |
| 容差：Plan > 官方 > 缺省 1e-3/1e-5 | rule | **none 本轮** | Batch 4 `ops-precision-standard` |
| DumpTensor 7 步 / PRINTF 手册 | template | **none → 以后 references** | 不进 SKILL.md |
| SIMT DCache / Simt::printf | workflow | **none** | 出本轮默认树 |
| Cast RoundMode 口诀 | rule | **none 本轮** | 未对官方文档前不当 fact；需要时另开 Card |
| 数值稳定（exp 减 max、epsilon） | recommendation | **none 本轮** | 通用数值建议，不是 AscendC 缺口 |
| case-studies / summary 模板 / evals | template | **none** | |

入口决策树（第 0 / 0.5 / 1 / 2 / 3 步）**不要原样搬进 Skill**。它把“清 cache、多 dtype、搬运、对文档、看误差形态”排成一条必须走完的仪式。Weaver 按 classify 之后的假说选下一步。

### 2. `ascendc-runtime-debug`

几乎全是 workflow + 错误码表。Skill 只留 **入口**，表进 references。

| 内容 | kind | 处理 | 去哪 |
| --- | --- | --- | --- |
| 161 / 361 / 561 分桶 | cannbot-secondary | **keep 作 routing hint** | 未核官方 error-code 文档前，不要标 official-document，也不要写成跨版本 taxonomy |
| 先 `aclGetRecentErrMsg`，再 plog | workflow | **keep** | runtime-error **手段**，不是主体教程 |
| 507035 = 向量核异常信号，经 stream sync 上报 | routing hint | **keep 作入口** | classify：runtime error，不是 crash。官方族义另核 |
| 507035 → Pad 32B / UB overflow（高/中/低置信度） | workflow | **rewrite** | 只保留“访存 / buffer / 对齐 / copy shape”假说树。**丢掉置信度等级和“Pad.blockLen 必须 32B”** |
| 561002 → TilingKey / TilingFunc | workflow | **keep（作假说）** | runtime-error |
| 561003 → 安装 / vendor / SEL / SOC | workflow | **keep（作假说）** | 并写一句 ≠ [delivery](../../../knowledge/engineering-experience/custom-op-delivery-call-and-selected-path.md) 选路 |
| 507046 ≠ 507035 | routing hint | **keep 作分流** | 507046 先按 hang。官方族义另核 |
| 完整 error_codes 表 | fact | **references only** | 不进 SKILL.md |
| kernel 二进制 / SEL / 多版本 / opParaSize | workflow | **references** | load 支路需要时打开 |
| `parse_plog.py` / 打屏环境变量 | template | **手段** | 不拷脚本进 Knowledge |
| 依赖 `ascendc-ut-generator` / env-check | orchestration | **none** | Weaver 无这两 Skill；需要时人手写最小复现 |

#### MUST NOT promote：507035 那段 copy 教案

`debug_workflow.md` 把 507035 的“高置信度根因”写成：

```text
DataCopyPad 的 blockLen 必须 32B 对齐
FP16: blockLen 是 16 的倍数
然后给 Pad 填 alignedBlockLen
```

这和已 `accepted` 的 [memory-copy-and-alignment](../../../knowledge/ascendc-api/data-movement/memory-copy-and-alignment.md) **冲突**：

```text
DataCopy        长度 32B + UB 起址 32B
DataCopyPad     就是为非对齐存在的
Pad Ext.blockLen 单位 = 字节
Pad 填的是 effective；UB 行偏移 / InitBuffer 才用 aligned
```

Agent 若吸收这段，会把 copy Card 的核心合同冲掉。  
507035 路径只许说：

```text
先读 copy Card 核对
  这条调用是 DataCopy 还是 DataCopyPad
  blockLen 的单位
  UB 起址 / 行偏移是不是 32B
  GM 写回用的是 effective 还是 aligned footprint
再查 UB 预算是否回传到 tiling
不要从 507035 反推“Pad 必须按 DataCopy 的合同填”
```

这就是 contract 里的 `same number ≠ same API contract`，以及：

```text
debug heuristic
  cannot override
accepted API contract
```

#### 错误码证据门

写 `references/runtime-error.md` 时：

```text
161xxx / 361xxx / 507035 / 507046 / 561002 / 561003

只见 CANNBot
  → debug routing hint

核到该 CANN 版本官方 error-code 文档
  → 才标 official error-family meaning

不要把“数字看起来稳定”写成跨版本永久 taxonomy
```

当前目标是路由假说，不是冻结错误码知识。

### 3. `ascendc-crash-debug`

结构最接近 Weaver 路由器。拆成 hang / crash 两条，不要再合成一本。

| 内容 | kind | 处理 | 去哪 |
| --- | --- | --- | --- |
| Segfault/Abort → coredump + GDB | workflow | **keep** | crash 路径 |
| hang → plog + Alloc/Free + EnQue/DeQue | workflow | **rewrite** | hang 的 **优先假说**，不是“卡死 = 没 Free” |
| 偶发 / 踩踏 → mssanitizer | workflow | **keep（手段）** | hang/crash references；安装步骤不进主体 |
| AIV hang → CrossCore Set/Wait | rule | **out of default** | 仅 AIC+AIV / 融合算子；链 MC2，不进普通 Vector 树 |
| drain hang → AIV→AIC flag | rule | **同上** | 同上 |
| aic error → 越界 / 非对齐 | workflow | **rewrite** | 仍是假说；先分清它是 runtime 上报还是进程死了 |
| `parse_plog.py` / memcheck 脚本 | template | **none 拷贝** | 需要时引用本机工具，不入库 |

Alloc/Free 配对、以及跨流水 dependency 是否被表达，事实在 TQue / pipeline Card。hang/crash 路径只问“合同在不在”，不把 EnQue/DeQue 写成唯一合法实现，也不重讲 `num`≠`depth`。

---

## 归位

| 产物 | 本轮 | 下一动作 |
| --- | --- | --- |
| [operator_debug_contract.md](operator_debug_contract.md) | 诊断模型 | **`accepted`** |
| 本文件 | 压缩裁决 | **`accepted`** |
| `skills/operator-debug/SKILL.md` | 短路由 | 写 |
| `skills/operator-debug/references/{runtime-error,wrong-result,hang,crash}.md` | ⏳ | 每条路径一页假说树 + 手段指针 |
| `workflows/correctness-debug/` | 一页闭环 | **`accepted`** |
| 新 Knowledge | **不开** | 陷阱事实已在 Batch 2 |
| 工具脚本 / 容差表 / SIMT | **none** | Batch 4 或以后 |

`README` 里 operator-debug 还写着“多卡通信问题”。本轮默认树 **不收** 通信 hang；只留一句路由到 MC2 Knowledge。

蓝区无 NPU：沿用 [cannbot_analysis.md](../../../cannbot_analysis.md) §8.5。不吸收“npu-smi 不过就不能调试思考”。

---

## 未来 Skill 允许从 CANNBot 留下的 Decision Rules

已经改写成 Weaver 口气。原文带确定映射的，全部降级。

```text
MUST 先 classify + 最小复现，再改代码
MUST NOT 凭症状表或错误码下根因
MUST NOT 无证据同时改同步、对齐、公式、tile

每轮只走一个 primary route；新证据允许重新 classify

IF 没跑完或返回码非 0
THEN 不进 wrong-result

IF 全 0 / 随机
THEN readiness → 对齐 → Get/SetValue   （recommendation）

IF 507035
THEN 访存 / buffer / 对齐 / copy-shape 假说树
THEN MUST 读 copy Card，MUST NOT 套“Pad.blockLen=32B”

IF 561003
THEN 安装 / SEL / vendor / SOC
THEN MUST NOT 写成 vLLM 选路

IF 改完输出不变
THEN 先检验二进制是否更新

IF 连续步骤无新观察
THEN 停试错，切二分 / dump / instrumentation
```

不保留：

```text
IF 快速尝试 > 7     （heuristic，不是边界）
IF 507035 THEN 改 Pad 对齐到 32B
精度门槛 = FP16 1e-3 / FP32 1e-5
主 Agent 必须 npu-smi 通过才能继续
```

---

## 明确不进 Knowledge

这些在三套 Skill 里看起来像“知识”，本轮都不单独立卡：

| 看起来像知识 | 实际是 | 已有归处 |
| --- | --- | --- |
| 全 0 先查同步 | 调试排序 | pipeline Card Implications |
| DataCopy 异步 | API 事实 | pipeline Card |
| 32B / Pad | API 事实 | copy Card |
| Alloc/Free 成对 | API 事实 | TQue + pipeline |
| repeat 截断导致 silently 错 | API 事实 | mask Card |
| 161/361/561 族 | 错误码入口 | Skill references |
| 507035 机制句 | 信号，不是根因 | Skill runtime-error |
| DumpTensor 怎么插 | 手段 | 以后 references |

再写一篇 `knowledge/.../precision-pitfalls.md` 只会把 Card 重讲一遍。

---

## Review 清单（给下一轮写 Skill 用）

写 `SKILL.md` 时对照 contract，出现任一条就打回：

- [ ] 主体超过“循环 + 边界 + 分桶 + 链 Knowledge”  
- [ ] 把 wrong-result 整条路径命名或收成 precision  
- [ ] CopyIn→CopyOut 对就给 MTE2↔V / V↔MTE3 readiness 降权  
- [ ] 把 EnQue/DeQue 写成 DataCopy 后唯一合法同步  
- [ ] classify 写成一次定终身  
- [ ] 出现“全 0 = 同步”或“507035 = 没对齐”  
- [ ] 把 DataCopy 的 32B 合同写到 DataCopyPad.blockLen 上  
- [ ] hang / crash 合成一棵树  
- [ ] 写入缺省 rtol 或 “>7”  
- [ ] 把 DumpTensor / GDB / mssanitizer / plog 步骤写进主体  
- [ ] 重写 Batch 2 Card 已覆盖的 API 事实  
- [ ] 要求无 NPU 也先过 npu-smi  
- [ ] 未核官方文档就把错误码标成 official-document taxonomy

---

## Sources

| Field | Value |
| --- | --- |
| CANNBot | `ops/ascendc-{precision,runtime,crash}-debug` @ `c4c425641d11127945d22e5a752fcad77646be97` |
| 冲突点 | cannbot `debug_workflow.md` §507035 vs Weaver copy Card |
| 模型 | [operator_debug_contract.md](operator_debug_contract.md) |
| 计划 | [knowledge_promotion_plan.md](knowledge_promotion_plan.md) §Batch 3 |
| Last written | 2026-08-22 |
| status | accepted |
