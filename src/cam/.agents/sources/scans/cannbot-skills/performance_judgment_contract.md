# Performance-judgment contract

status: **accepted**

本文件只钉 **正确性通过之后，Agent 该怎么判断慢在哪**。不搬 CANNBot 各族模板，不再开基础 API Card。

```text
Batch 1–2  怎么写对
Batch 3    写坏了怎么查
本文件     对了以后怎么有纪律地判断性能
    ↓
skills/operator-optimization/SKILL.md   ← 短路由
```

消费方：`operator-optimization`。  
测量手段接已有能力：`instrumentation`（阶段 timing / Cycle / 边界点）、msprof / profiling-analysis（已有目录则分析，没有则可设计采集）。  
不要另开一条孤立的“性能常识”线。

证据种类：`official-document` | `source-code` | `recommendation` | `unknown`。  
性能排序、优先改哪一层一律是 `recommendation`。没上板数字不得写成硬件事实。

---

## 先问什么

```text
1. 正确性是否已经按同一 failure / test contract 过了？
2. baseline / candidate 各是哪一版代码？
3. 手里有阶段耗时 / msprof / 打点，还是只有“感觉慢”？
4. 哪个阶段 active 很久？其中哪些暴露在 critical path 上？
5. 下一个改动能不能被这次测量否定？不能 → 还不是优化，只是再写一遍。
```

不要先问“开 double buffer 会不会快 30%”，也不要先抄 Softmax / pingpong 模板。

---

## Rule

```text
operator-optimization 是路由器，不是各族性能百科

必须先 correctness，再 measure，再改
  没测量
    ≠
  已定位瓶颈

诊断循环
  correctness passed
    → measure
    → localize stage
    → classify bottleneck family
    → ranked hypotheses
    → smallest evidence-backed change
    → correctness retest
    → compare under the same performance contract
      （baseline identity A vs candidate identity B）

瓶颈族（普通 Vector 默认）
  movement / copy
  pipeline overlap / exposed wait
  core occupancy / load balance
  tile / buffer footprint
  vector instruction efficiency
  other（scalar / host / comm / cube）

stage active cycles
  ≠
exposed latency
  ≠
critical-path contribution

存在 overlap 时
  T_total
    ≠
  Σ T_stage

code identity must be known
  ≠
code identity must be the same

missing required ordering
  → correctness

extra / exposed ordering cost
  → performance

same number / same unit
  ≠
same API contract
  （“512B 更快点”不能覆盖 DataCopy 32B 合同）

debug / perf heuristic
  cannot override
accepted API / hardware contract
```

---

## 诊断循环

```text
correctness passed     ← 同一 semantic test contract；蓝区未测就标未测
        ↓
measure
  阶段 Cycle（instrumentation）
  和 / 或 msprof / 已有 profiling 目录
  MC2 才谈隔离 T_comm / T_compute
        ↓
localize stage
  CopyIn / Compute / CopyOut / Comm / Sync / idle / Host
  先记 active cycles，再问它是否暴露在 critical path
        ↓
classify bottleneck family
  只分族，不写“应该开 pingpong”
  优先瞄准 exposed latency / stall，不是 active 最大的阶段
        ↓
ranked hypotheses
  每条链到 accepted Knowledge 或 Case
  或一条可检验的测量
        ↓
smallest evidence-backed change
  一次一处；改完先正确性，再比性能
```

```text
“感觉慢”
  ≠
bottleneck

MTE2 busy 高
  ≠
一定是搬运没对齐
  （MC2 上还可能是通信等待污染）

num=2
  ≠
一定更快
  （TQue Card：只是 overlap eligibility）
```

---

## 六条边界

### 1. 慢的感觉 ≠ 瓶颈

没有阶段或 kernel 测量，只许做 **静态假说**，不许报加速比，不许把模板收益表当结论。

### 2. 指标 ≠ 根因

| 观察 | 允许写成 | 禁止写成 |
| --- | --- | --- |
| MTE2 active / busy 高 | 搬运 / 小 DMA / 等待污染 **假说入口** | “改 Pad 对齐到 512B” |
| 该时间已确认暴露在 critical path | 才把 movement / copy **升权** | 只因 CopyIn Cycle 最大就改搬运 |
| Vector 占比高 | 指令效率 / repeat / 中间 GM 往返 **假说入口** | “上 Counter 模式一定快” |
| 核空闲 / 尾核慢 | occupancy / tail / 切分 **假说入口** | “每核 4KB 是硬件事实” |
| 开了 `num=2` 仍串行 | tile 太小 / 过度同步 / overlap 没暴露 **假说** | “再加一层 PipeBarrier”；当成缺 readiness |
| 无 NPU | 只能设计检查；Runtime / Perf **未测** | 用 cannbot 收益表填数 |

### 3. stage duration ≠ exposed bottleneck

阶段 Cycle 回答“这个阶段做了多久”，不自动回答“它暴露在 critical path 上多久”。

```text
double buffer 例
  tile N:     Compute ────────────────
  tile N+1:   CopyIn ──────────

CopyIn active cycles 可以很大
  只要被 Compute 隐藏
  就未必是端到端瓶颈

存在 overlap 时
  T_copyin + T_compute + T_copyout
    ≠
  kernel latency
```

优化优先瞄准 **exposed latency / stall / critical-path contribution**，不要机械选 active cycles 最大的阶段。  
打点能测 stage，不代表 stage 最大的就是 optimization target。

### 4. 优化假说 ≠ Knowledge fact

Batch 2 已经冻结的合同，性能文不得改写：

```text
DataCopy 32B 长度 + UB 起址；Pad 吃非对齐；Ext.blockLen 是字节
跨流水 dependency 必须显式表达；TQue 是默认范式，不是唯一机制
TQue.num = backing blocks；depth = queue protocol
950 片上容量用 GetCoreMemSize，不要抄 192KB / 248KB 进 kernel
```

CANNBot 的 “20–50%”“512B GM 对齐”“3510 UB=256KB” 保持 `unknown` / `recommendation`，对不上 accepted hierarchy 就丢掉。

### 5. 改一处 ≠ 验证

```text
编译过 / 看起来更顺
  ≠
更快

这一条 shape 快了
  ≠
performance contract 整体好了

baseline identity A
  → change →
candidate identity B
```

改完：先按 [correctness-debug](../../../workflows/correctness-debug/README.md) 回归正确性，再在 **同一 performance contract** 下比较 A 与 B。身份必须都记下来，但优化比较时身份必然不同。

### 6. 无证据重写 ≠ 优化

没有新测量就换一套实现（从零重写 Softmax、把 TQue 改成手写 flag、换 Matmul 模板）是盲试。  
连续步骤无信息增益：停改代码，回去补测量（打点 / msprof / 隔离测试）。

---

## Performance contract

和 Batch 3 的 failure contract 对应：正确性固定“哪次失败”；性能固定“比的是哪两版、什么负载、怎么测”。

```text
Correctness:  failure contract
Performance:  performance contract
```

| 块 | 至少固定 |
| --- | --- |
| **workload** | shape / dtype / input 特征；batch / token / seq（若有）；rank / world size（若分布式） |
| **environment** | SOC / CANN；critical env；device topology |
| **measurement protocol** | warmup；iterations；metric / statistic；instrumentation / profiling 开还是关 |
| **code** | baseline identity；candidate identity |
| **observation** | latency / cycles / throughput；若有，再附 stage evidence |

```text
instrumented build
  ≠
production performance build
```

阶段打点若改变了 kernel，只用于 **localization**。报收益时回到可比较的 benchmark configuration（同一 protocol，打点开关一致，通常是关打点或同一套开销）。

路径因跨域变化时：记录等价 command，不得静默改 workload / metric。

---

## 瓶颈族 → 先读什么

不要为性能再写 API Card。假说指回已有文。

| 族 | 典型观察 | 读 | 不要在优化里再发明 |
| --- | --- | --- | --- |
| movement / copy | Copy **暴露**在 critical path；小块 DMA；effective/aligned 膨胀 | [copy](../../../knowledge/ascendc-api/data-movement/memory-copy-and-alignment.md) | “512B 才是 DataCopy 合同”；CopyIn Cycle 最大就改搬运 |
| pipeline overlap / exposed wait | 合法合同下仍过度同步、tile 太小叠不上、可见 stall | [pipeline](../../../knowledge/ascendc-api/synchronization/pipeline-events-and-ordering.md)、[TQue](../../../knowledge/ascendc-api/data-movement/ub-tque-tbuf-and-double-buffer.md) | 把缺 readiness 当性能目标；“`num=2` 一定快 30%” |
| occupancy / tail | 核不满；尾核拖时间 | [tiling-from-shape](../../../knowledge/engineering-experience/tiling-from-shape.md) | 新的切分手册 |
| tile / buffer | UB 装不下 / 装太浅；double-buffer 没回传 | tiling + [hierarchy](../../../knowledge/hardware/ascend950/core-and-memory-hierarchy.md) | 把 Case 里的 248KB 写进 kernel |
| vector efficiency | Compute 占主导且指令碎、中间落 GM | [mask-repeat](../../../knowledge/ascendc-api/vector/mask-repeat-and-kernel-limits.md) | “Level-0 一拍一定 256B” |
| cube / K 切分 | GEMM MN 不够核、长 K | [GEMM K](../../../knowledge/engineering-experience/gemm-k-partition-strategies.md) + 43/66/68/73 | 再吸一份 A2 `MatmulImpl` |
| comm overlap | 通算融合 | [MC2](../../../knowledge/engineering-experience/mc2-compute-collective-coupling.md) | 把 MTE2 busy 直接当搬运 |
| FA / MoE | 已有机制 | FA / MoE Knowledge | cannbot Softmax 模板全书 |

分析顺序可以借用 cannbot `ascendc-perf-optimize` 的分层，但不要做成必须走完的仪式：

```text
先问切分 / occupancy 是否离谱
  → 若是 MC2：卡间 overlap（有 T_comm / T_compute 再谈）
  → 若有核间同步：核间等待
  → 再看单核：movement vs compute vs overlap vs scalar
```

普通 Vector 经常直接进单核四族。不要为 Add 先走一遍通算 Step 2。

```text
missing required ordering   → 回 operator-debug（correctness）
extra / exposed wait        → 本 Skill（performance）
```

---

## 和测量工具怎么接

```text
operator-optimization
  决定测什么、假说怎么排、改哪一处

instrumentation
  产出阶段 timing / Cycle / 边界点
  有 overlap / wait 证据时：用于判断 hidden vs exposed
  只有各阶段独立 duration 时：只能 localize active cost，
    不能直接推出 critical-path contribution
  不负责解释该开 pingpong
  打点构建默认不当最终收益数字

msprof / profiling-analysis
  已有采集目录 → 分析现有数据
  没有 → 可以设计 / 请求下一轮采集
  指标仍只是 hypothesis evidence，不是补丁

correctness-debug
  跨域搬运 + code identity + 先正确性再比速度
```

蓝区无 NPU：可以 classify 静态假说、设计要在内网跑的测量。Perf 标 **未测**。

---

## Skill 长什么样

最终形态：

```text
skills/operator-optimization/SKILL.md
  循环 + 六条边界 + performance contract + 瓶颈族 + 链 Knowledge / Case
    ↓
  不塞：msprof 命令百科、Softmax 五套模板、matmul swat 长文
```

`workflows/performance-tuning/` 若要写，只串“正确性过 → 测量 → 改一处 → 回归 → 再比”，不要第二份瓶颈百科。

---

## What NOT To Generalize

| 不要当成 contract | 为什么 |
| --- | --- |
| 把 performance-best-practices 整族搬进 Knowledge | 模板 + A2 数字 + 未核收益 |
| 再写 DataCopy / TQue / mask Card | Batch 2 已冻结 |
| 无测量报加速比 | 与仓库原则冲突 |
| 3510 UB=256KB、GM 必须 512B | 与 hierarchy / copy 冲突或未核 |
| 普通 Vector 强制四步流水仪式 | Step 2/3 有适用条件 |
| SIMT / Conv / 规划中的 NN | 出本轮 |
| 用性能文覆盖 correctness Skill | 慢和错不是一条路 |
| active 最大的阶段 = 端到端瓶颈 | overlap 时 T_total ≠ Σ T_stage |
| 优化前后必须同一 code identity | 要比的是 A vs B，合同其它字段相同 |

---

## Sources

| Field | Value |
| --- | --- |
| 发现来源（非真源） | cannbot `ops/ascendc-performance-best-practices` + `ops/ascendc-perf-optimize` @ `c4c425641d11127945d22e5a752fcad77646be97` |
| 已冻结复用 | Batch 2 Cards、tiling、GEMM K、MC2、FA/MoE、correctness-debug |
| 测量 | Weaver `instrumentation` 计划；用户侧 profiling-analysis / 打点 Skill |
| 归位 | [performance_promotion_verdict.md](performance_promotion_verdict.md) |
| Last written | 2026-08-22 |
| status | accepted |
