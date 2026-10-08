# AscendWeaver 内容建设清单与自动化执行规范

## 0. 目标

AscendWeaver 的建设目标不是收集尽可能多的 AscendC 代码和文档，而是把真实算子开发中的高价值信息加工成 **AI Agent 可以快速理解、检索、组合和执行的工程知识**。

任何内容进入 AscendWeaver 前，都应回答：

* [ ] 这份内容能帮助 Agent 做什么？
* [ ] 它代表的是一种新的设计模式，还是已有内容的重复？
* [ ] 它应该进入 `cases/`、`knowledge/`、`skills/`、`workflows/` 还是 `templates/`？
* [ ] 它的适用硬件是什么？
* [ ] 它的结论来自官方文档、真实源码、实际测试，还是推断？
* [ ] Agent 看完之后能否形成明确行动，而不是只获得背景知识？
* [ ] 是否值得占用 Agent Context？

核心原则：

> 少而精，优先收集能够代表一种工程模式的内容。

---

# 1. 建议补充的仓库结构

现有 AscendWeaver 基础上建议增加：

```text
AscendWeaver/
├── README.md
├── AGENTS.md
├── CURATION_PLAN.md
│
├── assets/
│
├── sources/
│   ├── README.md
│   └── registry.yaml
│
├── skills/
├── knowledge/
├── cases/
├── workflows/
├── templates/
│
├── tools/
│   ├── scan_repo.py
│   ├── lint_metadata.py
│   ├── build_catalog.py
│   └── check_links.py
```

其中：

```text
sources/
```

用于管理外部知识来源。

不要把来源信息散落在各个 Markdown 中。

---

# 2. 建立 Source Registry

创建：

```text
sources/registry.yaml
```

所有准备研究的仓库首先登记。

建议第一批来源：

```yaml
sources:

  - name: asc-devkit
    type: official
    organization: cann
    focus:
      - ascendc
      - basic-api
      - data-movement
      - best-practice
      - hardware-pattern
    priority: P0

  - name: catlass
    type: official
    organization: cann
    focus:
      - matmul
      - gemv
      - gmm
      - cube
      - layout
      - pipeline
    priority: P0

  - name: ops-transformer
    type: official
    organization: cann
    focus:
      - transformer
      - attention
      - moe
      - communication
      - compute-communication
    priority: P0

  - name: ops-nn
    type: official
    organization: cann
    focus:
      - normalization
      - activation
      - neural-network
    priority: P1

  - name: ops-math
    type: official
    organization: cann
    focus:
      - math
      - elementwise
      - reduce
      - basic-compute
    priority: P1

  - name: sgl-kernel-npu
    type: community-production
    focus:
      - llm
      - attention
      - moe
      - quantization
      - deep-ep
    priority: P0

  - name: vllm-ascend
    type: community-production
    focus:
      - llm
      - custom-op
      - inference
      - integration
    priority: P1

  - name: RecSDK
    type: official-domain
    focus:
      - gather
      - scatter
      - jagged
      - sparse
      - irregular-memory
    priority: P1

  - name: DrivingSDK
    type: official-domain
    focus:
      - sparse
      - scatter
      - irregular-memory
      - vision
    priority: P1

  - name: cannbot-skills
    type: agent-skill
    focus:
      - skill-design
      - workflow
      - templates
      - ascendc-development
    priority: P0

  - name: awesome-ascend-skills
    type: agent-skill
    focus:
      - skill-design
      - optimization
      - workflow
    priority: P1

  - name: NPUKernelBench
    type: evaluation
    focus:
      - eval
      - kernel-generation
      - correctness
      - performance
    priority: P1
```

每个 Source 后续补充：

```yaml
url:
branch:
commit:
license:
last_reviewed:
hardware:
maintainer:
notes:
```

---

# 3. 来源分类原则

每份外部资料首先判断属于哪种类型。

## A. Case Source

典型：

```text
CATLASS
ops-transformer
ops-math
ops-nn
sgl-kernel-npu
RecSDK
DrivingSDK
```

主要提炼到：

```text
cases/
knowledge/
```

---

## B. Skill Source

典型：

```text
cannbot
awesome-ascend-skills
```

主要提炼到：

```text
skills/
workflows/
templates/
```

不要作为算子 Case 使用。

---

## C. Documentation Source

例如：

```text
AscendC 官方 API
硬件架构资料
性能优化指南
```

主要进入：

```text
knowledge/
```

---

## D. Evaluation Source

例如：

```text
NPUKernelBench
```

主要用于：

```text
评估（evaluation）
```

---

# 4. 扫描一个新算子仓库的标准流程

Agent 获得一个新仓库后，不允许直接开始复制源码。

必须执行以下过程。

## Stage 1：Repository Reconnaissance

首先理解：

* [ ] Repository 主要解决什么问题
* [ ] 是否真正包含 AscendC Kernel
* [ ] 目标硬件有哪些
* [ ] Kernel 主要集中在哪些目录
* [ ] Build System 是什么
* [ ] Test 在哪里
* [ ] 是否有 benchmark
* [ ] 是否有性能说明
* [ ] 是否存在废弃实现
* [ ] 是否有多个硬件版本

输出：

```text
repository_summary.md
```

---

# 5. 自动寻找 AscendC Kernel

Agent 可以首先用静态搜索缩小范围。

推荐搜索：

```bash
rg -l '__aicore__' .
rg -l 'AscendC::' .
rg -l 'LocalTensor<' .
rg -l 'GlobalTensor<' .
rg -l 'TQue<' .
rg -l 'TBuf<' .
```

进一步识别 Memory 操作：

```bash
rg -l 'DataCopy|DataCopyPad|Copy' .
```

识别 Pipeline / Synchronization：

```bash
rg -l 'SetFlag|WaitFlag|PipeBarrier' .
```

识别 Cube：

```bash
rg -l 'Matmul|Mmad|L0A|L0B|L0C' .
```

识别通信：

```bash
rg -l 'Hccl|AllToAll|AllGather|ReduceScatter|Send|Recv' .
```

再重点检查目录：

```text
op_kernel/
kernel/
kernels/
src/
examples/
```

注意：

> 搜索结果只是候选文件，不代表值得进入 AscendWeaver。

---

# 6. 建立 Case Candidate Catalog

首先不要复制代码。

建立：

```text
cases/catalog.md
```

或者：

```text
cases/catalog.yaml
```

推荐字段：

```yaml
- id:
  name:
  source:
  source_path:
  source_commit:

  category:
  algorithm:

  hardware:
  dtype:
  shape_characteristics:

  compute_engine:
  memory_pattern:
  pipeline_pattern:
  communication_pattern:

  key_techniques:

  representative_value:
  optimization_value:
  reusability:
  implementation_quality:
  hardware_relevance:
  uniqueness:

  total_score:

  status:
  notes:
```

---

# 7. Case 分类标签

## category

只使用：

```text
compute
communication
compute-communication
```

---

## compute_engine

允许：

```text
vector
cube
vector+cube
simt
mixed
```

---

## memory_pattern

允许多选：

```text
continuous
strided
transpose
gather
scatter
broadcast
reduce
reuse
double-buffer
irregular
workspace
shared-window
```

---

## pipeline_pattern

例如：

```text
copy-compute-copy
double-buffer
multi-stage
vector-cube-overlap
communication-compute-overlap
producer-consumer
```

---

## communication_pattern

例如：

```text
none
send-recv
allreduce
allgather
reduce-scatter
alltoall
dispatch
combine
```

---

# 8. Case 价值评分

所有候选 Case 统一评分。

满分 100。

## 8.1 Pattern Representative — 25

这个实现是否代表一种重要算子设计模式？

```text
5：非常典型且很难被其他 Case 替代
4：有较强代表性
3：有一定价值
2：与已有 Case 高度类似
1：几乎没有新的模式
```

权重：

```text
score × 5
```

---

## 8.2 Optimization Value — 20

是否包含值得 Agent 学习的真实优化技巧？

例如：

```text
Double Buffer
Pipeline
GM Access Optimization
UB Reuse
Layout Optimization
Cube Scheduling
Communication Overlap
Synchronization Reduction
```

---

## 8.3 Hardware Relevance — 20

特别关注目标平台：

```text
Ascend 950
A5
A3
```

950 实现优先级应明显提高。

---

## 8.4 Reusability — 15

Agent 学习这个 Case 后，是否能迁移到其他算子？

---

## 8.5 Implementation Quality — 15

检查：

```text
结构是否清晰
是否生产级代码
是否存在完善测试
是否有 benchmark
是否存在明显历史包袱
```

---

## 8.6 Documentation Quality — 5

有没有：

```text
README
设计文档
注释
测试
性能数据
```

---

# 9. Case 扣分规则

以下情况需要扣分。

重复模式：

```text
-20
```

明显旧硬件、旧 API：

```text
-10 ~ -30
```

代码极度耦合，无法独立理解：

```text
-10
```

没有测试或正确性无法判断：

```text
-10
```

License 不清晰：

```text
直接暂停收录
```

只有算法价值，没有 AscendC 工程价值：

```text
不进入 cases/
```

---

# 10. Case 入选标准

建议：

```text
>= 80：
强烈建议进入

70 ~ 79：
候选

60 ~ 69：
只有填补模式空缺时考虑

< 60：
不进入
```

同时必须满足：

```text
不是已有 Case 的简单重复
```

---

# 11. 30 个 Case 不按“算子名字”选择

最终目标是覆盖约 30 种工程模式。

建议目标池：

## 基础数据流

* [ ] Elementwise
* [ ] Broadcast
* [ ] Reduce
* [ ] 多维 Reduce
* [ ] Transpose
* [ ] Layout Transform

## Vector

* [ ] Softmax
* [ ] LayerNorm / RMSNorm
* [ ] Activation Fusion
* [ ] Quantization
* [ ] Dequantization
* [ ] FP8 / FP4 Quantization

## Irregular Memory

* [ ] Gather
* [ ] Scatter
* [ ] Index Select
* [ ] Jagged Tensor / Sparse Access

## Cube

* [ ] GEMM
* [ ] GEMV
* [ ] Batched Matmul
* [ ] Grouped Matmul
* [ ] Matmul + Epilogue Fusion

## Complex Fusion

* [ ] Attention
* [ ] Flash Attention
* [ ] KV Cache / Paged Attention 类操作
* [ ] Complex Vector + Cube Fusion

## MoE

* [ ] TopK / Routing
* [ ] MoE Dispatch
* [ ] MoE Combine

## Communication

* [ ] AllToAll
* [ ] AllGather / ReduceScatter

## Compute + Communication

* [ ] AllGather + Matmul
* [ ] Matmul + ReduceScatter
* [ ] Communication / Compute Overlap

最终数量不是严格 30。

如果只有 24 个真正优秀的案例，就保留 24 个。

不要为了数量凑 30。

---

# 12. Case 正式目录格式

一个 Case 入选后：

```text
cases/
└── compute/
    └── gemm/
        └── ascend950-basic-matmul/
            ├── case.md
            ├── metadata.yaml
            └── reference/
```

目录按 `cases/<category>/<algorithm>/<case-name>/` 组织。来源仓库和 example 编号写在 `metadata.yaml` 的 `source` 里，不要编进路径。

其中：

```text
case.md
```

是最重要内容。

Agent 默认先读取 `case.md`，而不是源码。

---

# 13. Case Metadata

推荐：

```yaml
id: case-compute-gemv-catlass

name: GEMV CATLASS

source:
  repository: catlass
  commit:
  path:

category: compute

hardware:
  - ascend950

compute_engine:
  - cube
  - vector

memory_pattern:
  - tiled
  - reuse

pipeline_pattern:
  - multi-stage

key_techniques:
  - tile abstraction
  - L1 reuse
  - cube vector cooperation

status: reviewed

confidence: source-code

license:

last_reviewed:
```

---

# 14. Case Card 标准

`case.md` 必须回答下面的问题。

## 14.1 What

这个算子做什么？

只需非常简洁。

---

## 14.2 Why This Case

为什么它值得 AscendWeaver 保存？

这是 Case Card 最重要的部分之一。

例如：

```text
该 Case 的价值不是 GEMV 算法本身，而是展示：

1. Vector 与 Cube 的协同方式
2. L1/L0 Tensor Layout
3. Tail Tile 处理
4. Pipeline 组织
```

---

## 14.3 Data Flow

必须描述：

```text
GM
 ↓
L1 / UB
 ↓
L0 / Vector
 ↓
Compute
 ↓
GM
```

尽可能画 ASCII 图。

---

## 14.4 Parallel Strategy

描述：

```text
Block 如何切
Core 如何分
Tile 如何划
Tail 如何处理
```

---

## 14.5 Memory Strategy

重点：

```text
GM
UB
L1
L0A
L0B
L0C
Workspace
```

回答：

```text
数据为什么放这里？
数据什么时候复用？
什么时候释放？
```

---

## 14.6 Pipeline

描述：

```text
Stage 0
Stage 1
Stage 2
```

重点回答：

> 为什么这个 Pipeline 是这样组织的？

而不是简单描述代码顺序。

---

## 14.7 Key APIs

不要复制整个 API 文档。

只列：

```text
这个 Case 为什么选择这个 API。
```

---

## 14.8 Key Optimization Ideas

例如：

```text
连续 GM 访问
Double Buffer
UB Reuse
减少同步
提高 Cube 利用率
通信计算重叠
```

---

## 14.9 What Agent Should Learn

非常重要。

明确告诉 Agent：

```text
从这个案例应该抽象出什么设计模式。
```

---

## 14.10 What NOT To Copy

同样重要。

例如：

```text
这个 Tile Size 是针对某型号硬件，不应直接复用。

这个 Layout 是特定 Shape 下的选择。

这个同步方式不一定适用于 SIMT。
```

---

## 14.11 Applicable Scenarios

写清楚：

```text
什么时候值得参考这个 Case。
```

---

## 14.12 Source

必须记录：

```text
Repository
Commit
Source File
Hardware
```

---

# 15. 不要简单复制完整源码

Case 的核心不是保存 GitHub/GitCode 镜像。

默认原则：

> `case.md` 优先，源码其次。

`reference/` 只保留真正需要研究的部分。

如果需要保存完整代码：

* [ ] 确认 License
* [ ] 保留原 Copyright
* [ ] 记录 Source Commit
* [ ] 不修改后伪装成本项目原创
* [ ] 定期确认 upstream 是否变化

如果只是学习 Pattern，可以只保存源码路径和少量关键片段。

---

# 16. 一份优秀 Case 要继续“榨干”

Case 分析完成后，不代表工作结束。

继续问：

> 这里有没有通用经验，可以脱离这个算子存在？

如果有，就沉淀到：

```text
knowledge/engineering-experience/
```

例如 CATLASS GEMV 可以产生：

```text
knowledge/engineering-experience/
├── tile-design.md
├── cube-vector-pipeline.md
├── tail-tile-processing.md
└── l1-data-reuse.md
```

也就是说：

```text
一个优秀源码
      ↓
    Case
      +
Engineering Knowledge
```

---

# 17. Hardware Knowledge 建设

目标不是复制芯片手册。

每个硬件目录：

```text
knowledge/hardware/ascend950/
```

建议逐步建设：

```text
README.md
core-architecture.md
memory-hierarchy.md
vector.md
cube.md
simt.md
data-movement.md
synchronization.md
pipeline.md
data-types.md
alignment.md
performance-model.md
common-pitfalls.md
```

---

# 18. Hardware Knowledge Card 要回答的问题

例如 `memory-hierarchy.md`：

Agent 真正需要知道：

* [ ] 有哪些 Memory Level
* [ ] 每层容量
* [ ] 谁可以访问
* [ ] 数据如何搬
* [ ] 对齐约束
* [ ] Bandwidth 特征
* [ ] Latency 特征
* [ ] 什么情况适合数据复用
* [ ] 什么情况容易成为瓶颈
* [ ] 对算子设计意味着什么

最后一定加：

```text
Implications for Operator Design
```

例如：

> 因为 XXX，所以设计 Tile 时应该 XXX。

硬件事实必须最终转化成工程行为。

---

# 19. Hardware 信息必须标记可信度

推荐每条关键知识标注：

```text
Source Type:
- official-document
- official-source-code
- benchmark
- developer-experience
- inferred
```

特别注意：

> 推断不能写成硬件事实。

---

# 20. AscendC API Knowledge 建设

目标：

```text
knowledge/ascendc-api/
```

不要复制官方 API Manual。

Agent 需要的是“写代码时怎么用”。

每张 API Card 推荐包含：

```text
Purpose

Signature

Pipeline

Supported Types

Alignment

Mask

Repeat

Stride

Memory Requirements

Common Usage

Common Mistakes

Performance Notes

Minimal Example

Related Cases

Source
```

---

# 21. AscendC API 优先级

第一批只整理高频 API。

优先约 20~30 个。

重点类型：

```text
DataCopy
DataCopyPad

Cast
Duplicate
Adds
Muls
Add
Mul
Max
Compare
Select

Reduce
WholeReduce

Gather
Scatter

SetFlag
WaitFlag
PipeBarrier

Queue / Tensor API

Matmul / Cube 相关 API
```

后续根据实际 Case 再补。

原则：

> Case 使用到什么，再优先建设什么。

而不是一次整理 300 个 API。

---

# 22. Engineering Experience 建设

这个目录长期来看可能是 AscendWeaver 最值钱的部分：

```text
knowledge/engineering-experience/
```

建议逐步建设：

```text
gm-access-patterns.md
ub-reuse.md
double-buffer.md
pipeline-design.md
sync-patterns.md
multi-core-partition.md
tail-processing.md
alignment-pitfalls.md
layout-design.md
vector-optimization.md
cube-optimization.md
communication-overlap.md
performance-debugging.md
```

---

# 23. Engineering Experience 来源

可以来自：

```text
官方源码
CATLASS
真实优化项目
Code Review
问题单
性能分析
开发者分享
失败案例
```

其中失败案例非常重要。

例如：

```text
某种写法正确但性能差
某种同步导致流水断裂
某种地址计算导致非连续访问
某种 UB 规划导致额外搬运
```

这些内容比“最佳实践”有时更有价值。

---

# 24. Experience Card 标准

每篇经验建议采用：

```text
# 名称

## Problem

什么问题？

## Symptom

代码或性能表现是什么？

## Root Cause

为什么？

## Better Pattern

推荐怎么做？

## Bad Pattern

不要怎么做？

## Hardware Scope

适用于什么硬件？

## Related Cases

哪些真实算子体现了这个规律？

## Evidence

来源是什么？
```

---

# 25. 从 Cannbot 等 Skill 中提炼内容

Cannbot 不应该整个复制。

Agent 需要做：

```text
读取 Skill
 ↓
识别它解决什么任务
 ↓
提取 Workflow
 ↓
提取 Decision Rule
 ↓
提取 Validation Rule
 ↓
提取 Output Contract
 ↓
删除冗余背景
 ↓
与 AscendWeaver Skill 合并
```

---

# 26. 分析外部 Skill 时必须回答

* [ ] 它什么时候触发？
* [ ] 输入要求是什么？
* [ ] 如何判断用户信息不足？
* [ ] 第一步做什么？
* [ ] 如何查资料？
* [ ] 如何做技术决策？
* [ ] 如何验证代码？
* [ ] 失败后怎么迭代？
* [ ] 输出必须有哪些？
* [ ] 有哪些禁止行为？
* [ ] 哪部分非常适合 AscendWeaver？
* [ ] 哪部分是冗余内容？
* [ ] 哪部分已经过时？
* [ ] 哪部分和我们的流程冲突？

---

# 27. Skill 的核心是 Decision Making

优秀 Skill 不应该写成：

```text
AscendC 是一种用于……
```

而应该更多写：

```text
IF ...
THEN ...

WHEN ...
CHECK ...

IF FAILURE ...
DO ...

BEFORE GENERATING CODE ...
VERIFY ...
```

Agent Skill 应该指导行为。

---

# 28. Workflow 建设

Skill 和 Workflow 分开。

例如：

```text
workflows/new-operator/
```

回答：

```text
一个新算子从 Requirement 到 NPU Validation 完整经历什么。
```

而：

```text
skills/operator-development/
```

回答：

```text
Agent 在这个流程里应该如何思考和行动。
```

---

# 29. Template 建设

逐步建立：

```text
templates/
├── operator/
├── test/
├── build/
├── case-card/
├── modification-report/
├── profiling-report/
└── optimization-report/
```

目标是减少 Agent 每次重新设计格式。

---

# 30. NPU 内网输出模板必须提前设计

建议最终统一让内网 Agent 输出：

```text
npu-report/
├── summary.md
├── patch.diff
├── compile.log
├── runtime.log
├── correctness.md
├── profiling.md
└── optimization-suggestions.md
```

蓝区 Agent 可以直接读取整个目录。

这样闭环才能真正自动化。

---

# 31. Case 与 Knowledge 的关系

不要重复。

例如 GEMV Case 里：

```text
使用 Double Buffer
```

只解释：

> 为什么这个 GEMV 需要 Double Buffer。

通用原理放：

```text
knowledge/engineering-experience/double-buffer.md
```

Case 再引用 Knowledge。

最终：

```text
Case
 ↓
引用 Knowledge

Knowledge
 ↓
关联多个 Cases
```

---

# 32. Knowledge 去重原则

每次新增文档前 Agent 必须搜索：

```text
knowledge/
```

判断是否已有相同主题。

如果已有：

```text
优先更新旧文档
```

而不是创建：

```text
double-buffer-2.md
double-buffer-new.md
double-buffer-best.md
```

---

# 33. 内容长度控制

建议：

API Card：

```text
50 ~ 200 行
```

Engineering Experience：

```text
50 ~ 300 行
```

Case Card：

```text
100 ~ 500 行
```

Hardware Topic：

```text
100 ~ 400 行
```

Skill：

可以更长，但需要明确章节和决策路径。

原则：

> Agent 能快速找到答案比文档完整更重要。

---

# 34. Source Provenance

所有外部知识必须可追踪。

建议文档末尾：

```text
## Sources

Repository:
Commit:
File:
Hardware:
Document:
Last Verified:
```

不要只写：

```text
来源：CATLASS
```

必须尽可能精确到 Commit 和 File。

---

# 35. 硬件版本隔离

尤其重要。

不要把：

```text
A2
A3
A5
Ascend 950
```

经验混在一起。

每个结论尽量标：

```text
hardware:
```

如果跨硬件通用：

```text
hardware:
  - generic
```

如果不确定：

```text
hardware:
  - unknown
```

不要猜。

---

# 36. “事实”和“经验”必须区分

例如：

```text
UB Size = XXX
```

属于硬件事实。

而：

```text
通常 Tile 使用 70% UB 更容易安排流水
```

属于经验。

不能混为一谈。

推荐标记：

```text
Fact
Guideline
Empirical
Inference
```

---

# 37. 自动化 Agent 分工

不要让一个 Agent 一口气“分析整个仓库并全部整理”。

推荐拆成四种角色。

## Scout Agent

负责：

```text
扫描 Repository
发现候选 Kernel
生成 catalog
初步分类
```

禁止：

```text
直接修改正式 Knowledge
```

---

## Curator Agent

负责：

```text
分析候选
评分
选择优秀 Case
生成 Case Card
```

---

## Knowledge Miner Agent

负责：

```text
从 Case 提炼通用知识
更新 knowledge/
```

---

## Reviewer Agent

负责：

```text
检查事实
检查重复
检查硬件范围
检查 Source
检查 License
检查格式
```

最后再进入主分支。

---

# 38. 推荐自动化 Pipeline

```text
Upstream Repository
        ↓
      Scout
        ↓
Candidate Catalog
        ↓
     Scoring
        ↓
     Curator
        ↓
      Case
        ↓
 Knowledge Miner
        ↓
  Engineering Knowledge
        ↓
     Reviewer
        ↓
     Accepted
```

---

# 39. 不要一次处理太多

每个 Agent Batch：

```text
3 ~ 5 个 Case
```

比较合理。

避免：

```text
分析这个仓库所有 200 个算子
```

这种任务通常最终会产生大量低质量总结。

---

# 40. 推荐 Git 工作方式

例如处理 CATLASS：

```text
git checkout -b curate/catlass-gemv
```

完成：

```text
catalog update
case card
knowledge extraction
source metadata
```

然后 Commit：

```text
feat(cases): add CATLASS GEMV reference case
```

不同来源尽量独立 Commit。

---

# 41. 自动 Review Checklist

每个新增 Case 必须检查：

* [ ] 是否解释了 Why This Case
* [ ] 是否描述 Data Flow
* [ ] 是否描述 Parallel Strategy
* [ ] 是否描述 Memory Strategy
* [ ] 是否描述 Pipeline
* [ ] 是否提炼 Key Optimization
* [ ] 是否说明 What Agent Should Learn
* [ ] 是否说明 What NOT To Copy
* [ ] 是否有 Hardware Scope
* [ ] 是否有 Source Commit
* [ ] 是否存在重复 Case
* [ ] 是否存在未经验证的性能结论
* [ ] 是否有 License 风险

---

# 42. 自动 Knowledge Review Checklist

* [ ] 是通用知识而不是某个 Case 的代码解释
* [ ] 给出了适用范围
* [ ] 给出了工程意义
* [ ] 有 Bad Pattern
* [ ] 有推荐 Pattern
* [ ] 有真实 Case 支撑
* [ ] 有 Source
* [ ] 没有与现有文档重复
* [ ] 没有把推断写成事实

---

# 43. 自动 Skill Review Checklist

* [ ] 描述的是 Agent 行为，而不是百科知识
* [ ] 有明确输入
* [ ] 有明确输出
* [ ] 有决策条件
* [ ] 有失败处理
* [ ] 有验证步骤
* [ ] 有禁止行为
* [ ] 能引用 Knowledge
* [ ] 能引用 Case
* [ ] 没有复制大量外部文档

---

# 44. Build 自动化工具

后续可以逐渐加入：

```text
tools/scan_repo.py
```

负责：

```text
发现 AscendC 文件
统计 API
定位 Kernel
生成 Candidate List
```

---

```text
tools/build_catalog.py
```

负责：

```text
根据 metadata 自动生成 cases/catalog.md
```

---

```text
tools/lint_metadata.py
```

负责检查：

```text
source 是否存在
hardware 是否填写
category 是否合法
status 是否合法
```

---

```text
tools/check_links.py
```

负责检查 Source URL。

---

# 45. Case Status

统一使用：

```text
discovered
candidate
selected
extracted
reviewed
accepted
deprecated
```

不要使用自由文本。

---

# 46. Knowledge Status

推荐：

```text
draft
reviewed
verified
deprecated
```

---

# 47. Eval 从第一天开始

不要等知识库建设完成再做。

Eval 方法学：第一道 L1 Behavior 例如 `debug_507035`，判卷与任务分离。L2/L3 未开。（eval 不随本部署提供）

产出层再选固定算子任务，例如：

```text
tasks/
├── vector-basic/
├── reduce/
├── quant/
├── gemv/
└── moe-simple/
```

记录：

```text
Compile
Correctness
Iteration Count
Human Modification
Performance
```

---

# 48. 每加入一批重要 Knowledge 后跑 Eval

例如：

```text
Baseline

↓ 加 CATLASS Cases

Eval

↓ 加 Hardware Knowledge

Eval

↓ 加 API Knowledge

Eval

↓ 优化 Operator Development Skill

Eval
```

这样才能知道：

> 什么知识真的帮助 Agent。

---

# 49. 不要只统计成功率

还可以记录：

```text
Agent 阅读了哪些文件
引用了哪些 Case
用了哪些 Knowledge
哪些资料从未被使用
```

如果一篇 Knowledge 半年没人使用：

```text
考虑删除、合并或重新组织。
```

---

# 50. AscendWeaver v0.1 推荐建设顺序

不要并行建设所有目录。

## Phase 1

先完成：

* [ ] Source Registry
* [ ] Case Catalog Format
* [ ] Case Card Template
* [ ] Review Checklist

---

## Phase 2

研究三个最关键来源：

```text
asc-devkit
CATLASS
cannbot
```

原因：

```text
asc-devkit
→ 基础 AscendC Pattern

CATLASS
→ 高性能算子工程模式

cannbot
→ Agent Skill 设计模式
```

---

## Phase 3

选第一批 5 个 Case：

建议覆盖：

```text
Basic Vector
Reduce / Norm
GEMV
GEMM
Irregular Memory
```

不要第一批就做 MoE。

---

## Phase 4

从 5 个 Case 反向生成：

```text
knowledge/
```

不要提前凭空写很多“优化经验”。

---

## Phase 5

整理 Ascend 950：

```text
Hardware
Memory
Vector
Cube
Pipeline
Alignment
Data Type
```

---

## Phase 6

整理第一批高频 AscendC API。

---

## Phase 7

开始：

```text
ops-transformer
sgl-kernel-npu
```

增加：

```text
Attention
MoE
Communication
Compute + Communication
```

---

## Phase 8

建立第一套 Eval。

---

# 51. 最终目标

一个优秀的 AscendWeaver，不应该表现为：

```text
500 篇 Markdown
+
300 个源码目录
```

而应该表现为：

```text
30 个真正优秀的 Case

+

数十篇高密度 Engineering Knowledge

+

准确的 Hardware Knowledge

+

高频 AscendC API Cards

+

少量但强大的 Skills

+

标准 Workflow / Template

+

可量化的 Eval
```

最终希望 Agent 面对一个新算子时能够完成：

```text
Requirement
      ↓
Operator Classification
      ↓
Search Relevant Cases
      ↓
Load Hardware Knowledge
      ↓
Load Required API Knowledge
      ↓
Design Data Flow
      ↓
Design Tile / Core / Pipeline
      ↓
Generate Code
      ↓
Build
      ↓
Test
      ↓
Instrumentation
      ↓
NPU Validation
      ↓
Performance Analysis
      ↓
Optimization
```

AscendWeaver 的核心价值不是：

> 帮 AI 找到更多资料。

而是：

> **把优秀 AscendC 开发者做技术决策的方法，逐渐沉淀成 Agent 可以复用的工程能力。**

---

# 52. 可直接交给 Agent 的任务指令

下面这套任务可以作为后续自动化内容建设的基础 Prompt。

```text
You are the AscendWeaver Curator Agent.

Your task is to analyze an external Ascend/AscendC repository and extract
high-value reusable engineering knowledge for AscendWeaver.

Do NOT blindly copy source code or documentation.

Your goal is to identify reusable operator design patterns.

For the given repository:

1. Understand the repository purpose and structure.

2. Locate AscendC kernels.

3. Build a candidate kernel catalog.

4. For every candidate, identify:
   - operator category
   - hardware
   - compute engine
   - memory pattern
   - pipeline pattern
   - communication pattern
   - key optimization techniques

5. Score candidates using AscendWeaver Case Selection Rules.

6. Do not select multiple kernels representing essentially the same pattern
   unless they demonstrate substantially different optimization techniques.

7. Select no more than 3-5 cases in one batch.

8. For every selected case, generate:
   - metadata.yaml
   - case.md

9. case.md must explain:
   - why this case is valuable
   - data flow
   - parallel strategy
   - memory strategy
   - pipeline
   - key APIs
   - key optimization ideas
   - what an Agent should learn
   - what should NOT be copied directly
   - hardware scope
   - source location

10. After analyzing the case, identify reusable engineering knowledge.

11. If reusable knowledge already exists in knowledge/, update it instead
    of creating duplicate documents.

12. Clearly distinguish:
    - documented hardware facts
    - source-code observations
    - empirical engineering experience
    - inference

13. Never claim performance improvement without evidence.

14. Record exact upstream repository, commit and file path.

15. Check licensing before copying source code.

16. Finish by producing a review report containing:
    - discovered candidates
    - selected cases
    - rejected cases and reasons
    - new knowledge created
    - existing knowledge updated
    - unresolved questions
    - recommended next batch

Prioritize quality over quantity.

If none of the candidates provide unique engineering value, select none.
```

---

# 53. 人应该介入的位置

Agent 可以完成大量扫描、总结、分类工作。

但是以下决策建议由有经验的开发者最终确认：

* [ ] 这个 Case 是否真的优秀
* [ ] 这个性能设计是否真的有代表性
* [ ] Agent 对代码设计目的是否理解正确
* [ ] 某个经验是否应该推广成通用规律
* [ ] 某个硬件结论是否可靠
* [ ] 两个 Case 是否实际上是重复模式

这恰恰是人的高价值工作。

目标不是：

```text
人工整理文档
```

而是：

```text
Agent 做 80% 信息加工
        ↓
Expert 做 20% 技术判断
        ↓
形成高质量 Knowledge
```

这也是比较适合 AscendWeaver 的建设方式。
