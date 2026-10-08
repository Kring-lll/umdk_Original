# AscendWeaver Agent Guide

## 1. 项目定位

AscendWeaver 是一个面向 AscendC 算子开发的 AI Skill 与工程知识仓。

目标是帮助 AI Agent 完成以下任务：

* 理解算子开发需求
* 编写 AscendC 算子代码
* 生成构建与测试脚本
* 根据目标硬件进行初步性能优化
* 生成开发分支与打点分支
* 根据 NPU 实测反馈定位问题并继续优化
* 沉淀可复用的算子开发经验

AscendWeaver 不应被视为单纯的文档仓库。

仓库中的内容应优先服务于：

> **让 AI Agent 更可靠、更高效地完成 AscendC 算子开发。**

---

## 2. 仓库结构

```text
AscendWeaver/
├── skills/
├── knowledge/
├── cases/
├── workflows/
├── templates/
└── gap/
```

各目录职责如下。

### skills/

描述 Agent 应该“如何完成任务”。

Skill 负责：

* 任务拆解
* 决策流程
* 信息检索
* 工具调用
* 代码生成
* 验证流程
* 输出要求

Skill 中不要大量复制硬件文档、API 文档或完整算子源码。

需要相关知识时，应从 `knowledge/` 和 `cases/` 中按需读取。

---

### knowledge/

描述 Agent 在开发过程中“需要知道什么”。

主要包括：

* Ascend 硬件特征
* AscendC API 使用方法
* Memory / Pipeline / Synchronization 等基础知识
* 编码规范
* 工程经验
* 常见问题与性能陷阱

Knowledge 应尽量精炼，优先保留对 Agent 写代码和做工程决策真正有帮助的信息。

---

### cases/

存放经典算子案例。

案例用于帮助 Agent：

* 学习成熟实现
* 查找类似的数据流设计
* 参考切分策略
* 参考流水编排
* 参考通信设计
* 参考性能优化方法

案例不是简单保存源码。

每个案例应尽可能配套 Case Card，说明：

* 算子类型
* 数据流
* 核心设计
* 使用的关键 API
* 性能特点
* 值得借鉴的设计
* 不应直接照搬的部分

---

### workflows/

存放标准工程流程。

例如：

* 新算子开发
* 正确性调试
* 性能优化
* 打点分析

Workflow 描述完整任务生命周期。

Skill 可以引用 Workflow。

---

### templates/

存放标准输出模板。

例如：

* 算子工程模板
* 构建脚本
* 测试脚本
* 修改报告
* 性能报告
* 打点报告

Agent 生成工程产物时应优先参考模板。

---

### gap/

Agent 实际失败 / 犹豫 / 绕路的 **账本**。先分类，再决定补 Skill、Knowledge、Case，还是 **NO-ACTION**。

```text
GAP_LOG     = learning backlog + provenance（不是运行时 prompt）
Knowledge   = 已确认的事实 / 机制
Skill       = Agent 的决策流程
PBC / Gates = 性能设计的 executable checklist
Cases       = 具体实现如何体现规则
clean-room  = regression / transfer test
```

**日常算子开发不要打开 `gap/GAP_LOG.md`。** 记 OPEN ≠ 已回馈 Weaver。
闭环：OPEN → 通用规则 → canonical 落盘 → LANDED → **换算子** clean-room → VALIDATED →（确认无副作用）CLOSED。
发现任务本身不能 VALIDATED。LANDED 不能因“写进文件”升 CLOSED。

MoonEP Combine 训练链已暂停。下一次 clean-room 换算子，专门触发 007/009/014/015/016。
不要告诉 Agent 规则来源于 Combine。

用法：`gap/GAP_LOG.md` 文首 Promotion protocol、验收冻结、审计表。不要为一条原则新开一篇 Markdown。

---

# 3. Agent 工作原则

## 3.1 先理解，再编码

收到算子开发任务后，不应立即开始大量生成代码。

没有可靠设计文档时，先走 `skills/operator-spec-reconstruction/SKILL.md`：先冻结 **R0 Target Context**（repo / commit / mode / 硬件），再从调用链、测试/golden 和 kernel **交叉重建** Operator Contract，并落盘到目标仓 `docs/ascendweaver/`。不要把变量名写成语义事实。不要把不同 commit/mode 的证据交叉投票。

首先明确：

* 算子数学语义
* 输入
* 输出
* dtype
* shape
* layout
* 动态 shape 范围
* 目标硬件
* 正确性要求
* 性能目标
* 工程约束

信息不足时，应明确指出缺失信息，并把 `unknown` 分成 **blocking / non-blocking**。存在 blocking unknown 时不得开始写核。

---

## 3.2 优先复用已有知识

开发前应检查：

1. 是否存在类似 Case
2. 是否存在对应硬件知识
3. 是否存在相关 AscendC API 指南
4. 是否已有工程经验说明

优先复用经过验证的模式，而不是重新设计所有逻辑。

---

## 3.3 不盲目复制案例

Case 是参考，不是模板复制源。

引用案例时需要重新判断：

* shape 是否一致
* dtype 是否一致
* hardware 是否一致
* memory 约束是否一致
* pipeline 是否适用
* API 是否适用于当前平台

---

## 3.4 正确性优先于无法验证的微优化

开发前先定任务类型（[workflows/README.md](workflows/README.md)）：

```text
NEW OPERATOR
    → workflows/new-operator/workflow.md
    Requirement → Design → Gate A →（批准）Implementation

EXISTING OPERATOR FEATURE
    → workflows/existing-operator-feature/workflow.md
    Source → Baseline Freeze → Delta Design →（批准）只实现 delta
```

未批准不得跨 Gate。已有算子禁止带着 Feature 方案读源码。

```text
Operator Contract / Baseline
→ Design Proposal 或 Delta Design
→ Gate A 或 Gate A-Delta（打开 performance-by-construction）
→ Implementation
→ Gate B：Frozen-Clause Traceability → worker topology → PERF-SMELL-01…12 + whole-call-graph + Gate B.5 Implementation Adversarial Challenge
→ Operator Iteration（`skills/operator-iteration/SKILL.md`：Freeze → Build → Install → Load → L0…L8）
→ 性能分析 / operator-optimization
```

写完核禁止直接跑目标 e2e / msprof。上板状态机与 L0–L9 见 Iteration Skill；synchronize / 507xxx 不是 Python 行号上的根因。

正确性优先，不等于第一版可以完全串行、反复 GM、满屏 `PIPE_ALL`。那是 development 的结构门禁，细则 `skills/operator-development/references/performance-by-construction.md`。

```text
performance-aware baseline
  ≠
performance-optimal implementation
```

不要为了报加速比而引入无法验证的复杂实现。P-gates 过了也不要在无测量时声称具体收益。

---

## 3.5 性能优化应有依据

优化应尽量基于以下信息：

* 硬件特征
* 数据流分析
* Memory Traffic
* Pipeline
* Vector / Cube 利用率
* 通信等待
* 同步等待
* Cycle 打点
* NPU 实测结果

避免仅凭经验猜测性能收益。

---

# 4. 开发分支与打点分支

AscendWeaver 推荐维护两类代码分支。

## 开发分支

用途：

* 正确性开发
* 正式功能实现
* 性能优化
* 最终代码维护

开发分支应尽量保持代码整洁。

---

## 打点分支

用途：

* Cycle 打点
* Pipeline 分阶段测量
* 性能瓶颈定位
* 调试信息收集

打点代码不应污染正式开发分支。

推荐关系：

```text
development branch
        |
        +---- instrumentation branch
```

当开发分支发生重要变化时，打点分支应根据需要同步更新。

---

# 5. 蓝区与内网职责

AscendWeaver 的典型使用环境包含两个区域。

## 蓝区

蓝区 Agent 负责：

* 算子初步开发
* 查阅 AscendWeaver
* 编写源码
* 编写构建脚本
* 编写测试脚本
* 初步性能优化
* Git Commit
* Git Push
* 根据内网报告进行下一轮优化

---

## 内网 NPU 环境

内网 Claude Code / Agent 负责：

* Git Pull / Clone
* 编译
* 运行
* 收集日志
* 定位错误
* 本地修改
* 重新编译
* 重新运行
* 打点
* 性能分析
* 输出修改说明和性能报告

内网环境默认不能直接向蓝区 Git 仓库 Push。

---

## 人工职责

人工主要负责跨安全域资料搬运。

例如：

```text
patch / diff
修改说明
运行日志
打点数据
性能报告
```

人工原则上不承担 AI 已能够完成的重复编码与调试工作。

---

# 6. 默认开发输出

一个完整算子开发任务至少应尽量产出：

```text
AscendC 源码
编译产物
构建脚本
测试脚本
开发说明
```

根据任务需要，还可以产出：

```text
打点分支
patch / diff
修改报告
性能数据
性能分析报告
优化建议
```

---

# 7. 修改 AscendWeaver 的原则

向仓库新增内容时，优先问三个问题：

1. 这个信息 AI 写算子时真的会用到吗？
2. 它应该属于 Skill、Knowledge、Case、Workflow 还是 Template？
3. 是否可以比原始资料更短、更明确、更容易检索？

避免：

* 大量复制官方手册
* 一个文件包含所有内容
* 重复知识
* 没有来源和上下文的经验结论
* 没有适用条件的性能建议

---

# 8. 当前阶段

AscendWeaver 当前处于早期建设阶段。

v0.1 优先目标：

* 建立基本仓库结构
* 建立 Operator Development Skill（已收敛为主路由）
* 整理 Ascend 950 基础硬件知识
* 整理高频 AscendC API
* 建立少量经典案例
* 接入打点能力
* 建立基础 Eval

当前阶段允许内容不完整。

优先保证：

> **结构正确、职责清晰、能够持续演进。**
