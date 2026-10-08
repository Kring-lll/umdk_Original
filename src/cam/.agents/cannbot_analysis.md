# CANNBot Skills 分析（面向 AscendWeaver）

- 分析对象：`/home/wangyibo/cannbot-skills`
- 分析日期：2026-08-20
- 许可证：CANN Open Software License v2.0（禁止整仓复制进 AscendWeaver）
- 原则：只提炼可复用的决策、工作流和验证机制；不修改 cannbot 现有 Skill，也不改写 AscendWeaver 现有 Skill

本文回答 8 个问题：有哪些 Skill、核心能力、Decision Rules、Workflow、Validation、Weaver 已有什么、值得吸收什么、不建议吸收什么。

---

## 0. 先看清 cannbot 是什么

cannbot 不是一份“AscendC 手册”，而是一套 **Plugin → Agent → Skill** 三层产品：

```text
Plugin（编排）     决定阶段顺序、门禁、谁调度谁
    ↓
Agent（角色）      决定职责边界：能做什么 / 不能做什么
    ↓
Skill（能力）      决定怎么做：检索、规则、模板、脚本
    ↓
references/        按需展开的知识
```

AscendWeaver 的目标不同：把优秀开发者的技术决策沉淀成 **少而精的 Skill + Knowledge + Case**，让 Agent 开发 AscendC 算子。因此 cannbot 的价值主要在 **Skill 设计方法** 和 **AscendC 相关知识/流程**，不在完整产品栈。

仓库规模（`SKILL.md` 计数）：

| 区域 | 数量 | 说明 |
|------|------|------|
| `ops/` | 78 | 算子开发主战场 |
| `plugins-community/` | 104 | 实验/社区，含大量编排与 DSL |
| `model/` | 18 | 推理/训练优化 |
| `graph/` | 8 | torch.compile / npugraph_ex |
| `infra/` | 5 | Skill 审查 + GitCode |
| `plugins-official/` | 3 | 少量 workflow Skill（多数编排在 `AGENTS.md`） |
| `runtime/` | 1 | CUDA Runtime API 迁移 |
| **合计** | **217** | 其中真正与 AscendWeaver v0.1 强相关的约 30–40 个 |

官方 Plugin 入口（编排层，多数没有独立 `SKILL.md`）：

| Plugin | 路径 |
|--------|------|
| AscendC Kernel 直调 | `plugins-official/ops-direct-invoke/` |
| AscendC 注册调用 | `plugins-official/ops-registry-invoke/` |
| AscendC Flash/Reg 直调 | `plugins-official/ops-direct-invoke-flash/` |
| Catlass | `plugins-official/catlass-op-generator/` |
| 代码检视 | `plugins-official/ops-code-reviewer/` |
| PyPTO / TileLang / Triton | `plugins-official/{pypto,tilelang,triton}-*` |
| 模型推理 | `plugins-official/model-infer-optimize/` |
| torch.compile | `plugins-official/torch-compile/` |

---

## 1. 有哪些 Skill

按领域列出。AscendC 相关给全表；其他领域只给清单，避免把 Weaver 建成 cannbot 镜像。

### 1.1 AscendC 知识 / 设计 / 模板 / 调试（`ops/`，Weaver 主相关）

| Skill | 类别 |
|-------|------|
| `npu-arch` | 知识库 |
| `ascendc-tiling-design` | 知识库 |
| `ascendc-simt-tiling-design` | 知识库 |
| `ascendc-api-best-practices` | 知识库 |
| `ascendc-simt-best-practices` | 知识库 |
| `ascendc-regbase-best-practice` | 知识库 |
| `ascendc-blaze-best-practice` | 知识 + 流程 |
| `ascendc-blaze-migration` | 迁移流程 |
| `ascendc-performance-best-practices` | 知识 + 模板 |
| `ascendc-mc2-best-practice` | 知识 + 流程 |
| `ascendc-perf-optimize` | 优化流程 |
| `ops-precision-standard` | 知识库 |
| `ops-profiling` | 工具 + 流程 |
| `ops-simulator` | 仿真流程 |
| `ops-spec-gen` | spec.yaml 生成/校验 |
| `aiss-tiling-solver` | 外部工具封装 |
| `ascendc-docs-search` | 文档检索 |
| `ascendc-docs-gen` | 文档模板 |
| `ascendc-env-check` | 环境检查 |
| `cann-env-setup` | CANN 安装 |
| `ascendc-direct-invoke-template` | 工程模板 |
| `ascendc-registry-invoke-template` | 工程模板 |
| `ascendc-registry-invoke-to-direct-invoke` | 形态转换 |
| `ascendc-direct-invoke-to-registry-invoke` | 形态转换 |
| `torch-ascendc-op-extension` | PyTorch 对接 |
| `ascendc-code-review` | 检视流程 |
| `ascendc-precision-debug` | 精度调试 |
| `ascendc-runtime-debug` | 运行时调试 |
| `ascendc-crash-debug` | 卡死/崩溃调试 |
| `ascendc-st-design` | ST 设计 |
| `ascendc-ut-develop` | UT 开发 |
| `ascendc-whitebox-design` | 白盒用例 |
| `catlass-op-design` / `catlass-op-develop` / `catlass-op-perf-tune` | Catlass 三件套 |
| `torch-ops-profiler` | torch_npu.profiler |

文档里提到、当前 `ops/` **未随仓发布**：`ascendc-task-focus`（长任务防迷失）。旧镜像在 `/home/wangyibo/skill_dev/cannbot-skills/ops/ascendc-task-focus/`。

### 1.2 其他算子 DSL（`ops/`，Weaver v0.1 非主路径）

- **PyPTO**：`pypto-orchestration-manual`、`pypto-intent-understand`、`pypto-op-plan`、`pypto-api-explore`、`pypto-docs-search`、`pypto-golden-generate`、`pypto-op-design`、`pypto-op-construct`、`pypto-op-develop`、`pypto-op-verify`、`pypto-op-review`、`pypto-precision-compare`、`pypto-precision-debug`、`pypto-general-debug`、`pypto-op-perf-tune`（含子技能）、`pypto-op-knowledge`、`pypto-memory-template`
- **TileLang**：`tilelang-env-check`、`tilelang-submodule-pull`、`tilelang-op-design`、`tilelang-op-develop`、`tilelang-op-test-design`、`tilelang-api-best-practices`、`tilelang-programming-model-guide`、`tilelang-perf-optimization`、`tilelang-review`
- **Triton**：`triton-task-extractor`、`triton-op-designer`、`triton-op-coding`、`triton-op-verifier`、`triton-latency-optimizer`、`triton-precision-debug`、`triton-simulator-optimizer`

### 1.3 官方 Plugin 内 Skill

| Skill | 所属 Plugin | 作用 |
|-------|-------------|------|
| `ops-registry-invoke-workflow` | ops-registry-invoke | 注册调用全流程编排 |
| `spec-to-design` | ops-registry-invoke | spec.yaml → DESIGN.md 机器流水线 |
| `ops-direct-invoke-flash` | ops-direct-invoke-flash | Flash/Reg 直调 0–8 阶段 |

直调主路径的编排在 `plugins-official/ops-direct-invoke/AGENTS.md`，不是独立 Skill。

### 1.4 模型 / 图 / Runtime / 基础设施

- **model/**：15 个 `model-infer-*`（迁移、并行、KVCache、融合、量化、图模式、精度/运行时调试、多流、prefetch、superkernel、profiling、perf-breakdown、harmony）+ 3 个 `model-train-*`（精度、OOM、日志可视化）
- **graph/**：`torch-npugraph-ex-{knowledge,template,dfx-triage,compile-error-diagnosis,runtime-error-diagnosis,performance-diagnosis}`、`torch-custom-ops-guide`、`ge-fusion-pass-skill`
- **runtime/**：`runtime_migration`
- **infra/**：`cannbot-skill-reviewer`、`gitcode-toolkit`、`gitcode-pr-handler`、`gitcode-issue-gen`、`gitcode-issue-handler`

### 1.5 社区 Plugin（实验）

| Plugin | 内容概要 |
|--------|----------|
| `cannbot-knowledge` | 外部知识库 ingest / query / lint |
| `ops-perf-evolution` | 官方算子仓进化评估 |
| `ops-perf-optimize` | 性能优化编排 |
| `ops-qa-suite` | 官方算子仓 QA 扫描 |
| `collaborative-agent-kernel-evolution` (CAKE) | DSL→AscendC 多轮进化 |
| `cuda2ascend` | CUDA→AscendC，CP0–CP5 门禁 |
| `tilelang2ascendc-ops-generator` | TileLang 设计转译 AscendC |
| `cannbot-dsl` / `ops-easyasc-dsl` | 内部 DSL |
| `triton-optimizer` | Triton 转换/优化封装 |
| `cannbot-insight` | Session 可观测 Web UI |
| `install-helper` | npm 安装向导 |
| `science-model-npu-migration` | 科学计算模型迁移 |
| `ascendc-port-orchestrator` | 移植知识引擎 |
| `autoresearch` | 自动研究实验 |

---

## 2. 每个 Skill 的核心能力

### 2.1 Weaver 必须理解的 AscendC Skill

| Skill | 核心能力 |
|-------|----------|
| `npu-arch` | 芯片型号 / NpuArch / SocVersion / archXX 映射；DAV_3510 vs DAV_2201 差异；禁止硬编码硬件参数 |
| `ascendc-tiling-design` | 按算子族做 Tiling：多核切分、UB 切分、Buffer 规划、分支覆盖 |
| `ascendc-simt-tiling-design` | SIMT 切分（按元素数、constexpr 线程数、DCache/UB），明确不是 SIMD UB tiling |
| `ascendc-api-best-practices` | DataCopy/Reduce/Cast/Pipeline 等 API 限制、黑名单、场景路由 |
| `ascendc-simt-best-practices` | SIMT 编程模式、VF、线程 stride、同步 |
| `ascendc-regbase-best-practice` | DAV_3510 RegBase 路径：API 白名单、参考算子、陷阱；不作为默认开发路径 |
| `ascendc-blaze-best-practice` | Ascend 950 MatMul 家族 Blaze/tensor_api 端到端：调查→设计→实现，带设备验证门禁 |
| `ascendc-blaze-migration` | 把 950 MatMul 核函数等价迁到 Blaze，冻结 tiling/ABI |
| `ascendc-performance-best-practices` | 按算子族查优化经验和模板代码；有模板则复制改造，禁止凭空重写 |
| `ascendc-mc2-best-practice` | 通算融合：SHMEM/APACE/MoE MTE 三条路线 + 能力声明 + 红线 |
| `ascendc-perf-optimize` | 四层性能策略：Tiling 建模 → 卡间 → 核间 → 核内流水，必须有仿真/profiling 证据 |
| `ops-precision-standard` | 按算子类给出 atol/rtol 验收标准 |
| `ops-profiling` | msprof 采集/对比/瓶颈解读；MC² 必须用 `msprof` 不能用 `msprof op` |
| `ops-simulator` | 无板时用 npusim 做 950 单卡仿真；先看 summary.json |
| `ops-spec-gen` | 生成并 9 段校验 `spec.yaml`（L0 数学真源） |
| `aiss-tiling-solver` | 调用外部 TilingSolver CLI，禁止手算 tiling |
| `ascendc-docs-search` | 本地 API 索引优先，在线兜底；选 API 前必须找齐变体 |
| `ascendc-docs-gen` | REQUIREMENTS / DESIGN / PLAN / README 模板 |
| `ascendc-env-check` | npu-smi、CANN 变量、运行时 NpuArch |
| `cann-env-setup` | CANN 安装与 `set_env.sh` |
| `ascendc-direct-invoke-template` | `<<<>>>` 直调工程脚手架（Vector / Blaze / Kirin） |
| `ascendc-registry-invoke-template` | 注册调用完整工程（op_host/kernel/op_api/UT/ST） |
| `ascendc-registry-invoke-to-direct-invoke` | 注册算子改直调 |
| `ascendc-direct-invoke-to-registry-invoke` | 直调改注册 |
| `torch-ascendc-op-extension` | 已有直调工程接到 `torch.ops`（TORCH_LIBRARY + Meta） |
| `ascendc-code-review` | 假设检验检视；文件/PR/快速/扩规则四条工作流 |
| `ascendc-precision-debug` | 症状树 + DumpTensor + 尝试上限后二分 |
| `ascendc-runtime-debug` | 161/361/561xxx 错误码与 plog |
| `ascendc-crash-debug` | hang/crash/memcheck 路由 |
| `ascendc-st-design` | 约束图 + L0/L1/L2 ST 用例生成 |
| `ascendc-ut-develop` | UT 覆盖率增强（官方仓问卷门禁） |
| `ascendc-whitebox-design` | 从源码抽路径生成白盒 CSV |
| `catlass-op-design` | 选型 ArchTag / DispatchPolicy / TileShape / BlockMmad |
| `catlass-op-develop` | 按选型拼装 kernel + host |
| `catlass-op-perf-tune` | 一次只调一个变量 |
| `torch-ops-profiler` | torch_npu.profiler 采集 |

### 2.2 官方编排（Agent，不是单点 Skill）

| 入口 | 核心能力 |
|------|----------|
| `ops-direct-invoke` | 主 Agent 只调度：环境→设计→串讲→开发→审查→最多 3 轮修复→精度/性能→汇报 |
| `ops-registry-invoke` | 企业级注册算子：spec.yaml 真源 + CP1–CP5 人工/自动门禁 + 7 个角色 |
| `ops-direct-invoke-flash` | 简单/复杂分档；STATE.md 恢复；可选 ST/白盒 harness |
| `catlass-op-generator` | 直调 7 步壳 + Catlass 预检与 C1–C11 清单 |
| `ops-code-reviewer` | 薄封装，真正流程在 `ascendc-code-review` |
| `pypto-op-orchestrator` | 9 Agent、程序化 `state_transition` 门禁 |
| `tilelang-op-orchestrator` | 设计→实现+精度→可选性能；用返回标签路由 |
| `triton-op-generator` | 无 subagent；基线冻结 + 生成/优化循环 |
| `model-infer-optimize` | 固定阶段优化 **或** profiling 驱动的 Plan 自循环 |

### 2.3 其他领域（一句话）

- **PyPTO / TileLang / Triton Skill**：各自 DSL 的意图理解、设计、编码、精度、性能闭环。
- **model-infer-\***：推理框架适配与单项优化（并行、融合、量化、图模式等）。
- **graph/**：npugraph_ex 编译/运行/性能分诊。
- **runtime_migration**：仅 CUDA Runtime API 到 CANN Runtime，不含 kernel。
- **gitcode-\***：GitCode PR/Issue 自动化。
- **cannbot-skill-reviewer**：Skill 入库九维评分。
- **cannbot-knowledge**：把官方文档/wiki 做成可检索知识卡。
- **CAKE / ops-perf-evolution / cuda2ascend**：多轮进化或跨平台迁移，产品级编排。

---

## 3. 有价值的 Decision Rules

这些规则是 cannbot 真正值得学的部分：写成 **IF / THEN / MUST / MUST NOT**，而不是背景介绍。

### 3.1 编码前

```text
IF 关键语义（输入/输出/dtype/shape/硬件）不明确
THEN 先补信息，禁止假设后直接实现

IF 目标是 Ascend 950 / DAV_3510 的 MatMul 家族
THEN 走 Blaze/tensor_api（ascendc-blaze-best-practice）
MUST NOT 使用 A2/A3 高阶 MatmulImpl / MatmulApiTiling 路径

IF 纯单卡 MatMul
THEN 用 blaze-best-practice
MUST NOT 误入 mc2-best-practice

IF 算子是 SIMT
THEN 用 simt-tiling-design
MUST NOT 套用 SIMD UB tiling

IF 路由尚未决定 SIMD vs RegBase
THEN 先由设计阶段选择
MUST NOT 默认走 RegBase
```

### 3.2 Tiling / Memory（所有类别必须回答）

来源：`ascendc-tiling-design`

```text
MUST 输出四件事：
  1. 多核怎么切（维度、每核任务量、核数、负载是否均衡）
  2. UB 一次处理多少（chunk 公式）
  3. Buffer 列表、大小、复用、UB 利用率
  4. 分支覆盖（dtype / shape / 对齐 / 边界）

MUST 生命周期不重叠的 buffer 复用同一 TBuf
MUST 在 GetCoreMemSize(UB) 内尽量用满 UB
MUST NOT 运行时硬编码 UB/L0/核数（用 PlatformAscendC）
```

### 3.3 API 红线

来源：`ascendc-api-best-practices`、`ascendc-mc2-best-practice`

```text
MUST NOT 在生产代码使用 GlobalTensor::GetValue / SetValue
        → 用 DataCopyPad

IF GM↔UB 数据不是严格 32 字节对齐
THEN MUST 用 DataCopyPad，禁止 DataCopy

IF 需要查 API
THEN 先 docs-search 找齐变体，禁止凭记忆选 API

IF 通算融合走 blaze-shmem / apace
THEN MUST NOT 使用 Hccl::* 高阶集合通信和 AscendC::Matmul 高阶 API

IF MoE MTE 路径
THEN MUST NOT 用 HCCL collectives 搬数据；共享 GM 用 DataCopyPad
```

### 3.4 正确性优先、禁止盲改

来源：`ascendc-precision-debug`、`ops-direct-invoke`

```text
MUST 先固定最小复现，再查文档，再清 cache
MUST NOT 凭直觉改代码

IF 改完输出不变
THEN 清 build/ 和 kernel_cache

IF 全 0 / 随机值
THEN 先查 DataCopy 后是否 EnQue/DeQue

IF 快速尝试超过 7 次仍失败
THEN 切到二分调试，停止盲试

精度门槛优先级：任务 Plan > ops-precision-standard > 默认值
  FP16 rtol 1e-3 / FP32 1e-5 只是缺省，不是真理
```

### 3.5 运行时 / 崩溃路由

```text
IF 返回码 161xxx → 参数
IF 361xxx       → runtime
IF 507035       → DataCopyPad 32B 或 UB overflow
IF 561002       → TilingKey / TilingFunc
IF 561003       → 安装/环境（先 env-check）

IF Segfault/Abort → coredump + GDB bt
IF hang/timeout   → plog + Alloc/Free、EnQue/DeQue 配对
IF 偶发踩踏       → mssanitizer，不要继续猜
```

### 3.6 性能必须有证据

```text
MUST NOT 在没有实测时声称具体加速比

优化顺序：
  Tiling 模型 →（若通算）卡间重叠 →（若多核同步）核间 → 核内流水

IF MC²
THEN 采集必须 L2 flush；工具必须 msprof，禁止 msprof op
THEN 缺 T_comm / T_compute / overlap 数据就回去采，禁止猜

IF 有算子族模板代码
THEN 复制进工程再改
MUST NOT 从零重写并丢掉 MicroAPI
```

### 3.7 编排层（主 Agent）

来源：`ops-direct-invoke/AGENTS.md`

```text
主 Agent MUST NOT 写代码、改设计、改 subagent prompt
主 Agent MUST 按阶段门禁推进，禁止跳步

IF environment.md 不是 ✅ 通过
THEN 禁止进入设计

IF 只产出 DESIGN 或 PLAN 其中一个
THEN 打回 Architect 拆成双文件

设计串讲严格 1 轮，不无限往返
修复循环最多 3 轮，超过暂停交给人

争议仲裁顺序：官方 API 文档 > 精度 Skill > profiling 实测
```

### 3.8 知识写入规则（cannbot-knowledge，对 Weaver 特别有用）

```text
每张知识卡必须有来源（仓库/文档/@sha）
增量更新，禁止盲目覆盖
矛盾标出来，禁止静默合并
查询证据顺序：知识库 → 锁定的上游 → 本机 CANN 安装
 sufficiency：1 张强卡 或 2 张中等卡才够用，否则继续查
```

---

## 4. 有价值的 Workflow

### 4.1 Kernel 直调 7 步（最值得 Weaver 借鉴）

来源：`plugins-official/ops-direct-invoke/AGENTS.md`

```text
Step 1   环境检查（失败即停）
Step 2   设计 → DESIGN.md + PLAN.md
Step 2.5 设计串讲 → WALKTHROUGH.md（1 轮）
Step 3   开发
Step 4   独立审查 → REVIEW.md（PASS / PASS WITH NOTES / FAIL）
Step 5   修复循环（≤3）
Step 6   精度验收 + 性能归档
Step 7   汇报（明确测了什么、没测什么）
```

为什么好：

- 主 Agent 只编排，Developer / Reviewer 角色隔离
- 设计串讲把问题前移到写代码之前
- 文件系统当状态：用文件是否存在、标题行正则、PASS 字符串做门禁
- prompt 外置在 `task-prompts.md`，编排文档稳定

对应 Weaver：应进入 `workflows/new-operator/`，而不是塞进一个巨型 Skill。

### 4.2 调试路由（应进入 `operator-debug`）

```text
编译失败 / 环境     → env-check, cann-env-setup
aclnn 错误码        → runtime-debug
卡死 / 崩溃 / 越界  → crash-debug
数值不对            → precision-debug + precision-standard
性能差              → profiling → perf-optimize
```

关键模式：**症状 → 专科技能**，禁止一个 Debug Skill 包打天下。

### 4.3 性能闭环（应进入 `operator-optimization` + `instrumentation`）

```text
正确性通过
    ↓
采集（msprof / 打点）
    ↓
分阶段耗时（CopyIn / Compute / CopyOut / Comm / Sync）
    ↓
对照硬件知识与 Case
    ↓
只改有证据的一处
    ↓
回归精度后再比性能
```

`ascendc-perf-optimize` 的四层分析、`ops-profiling` 的 compare 模式、CAKE 的 “规则先于 LLM” advisor，都是这个闭环的具体实现。

### 4.4 复杂度分档（flash 的优点，可轻量吸收）

```text
简单算子：少 Agent、少文档、1 次编译 + 1 次上板
复杂算子：完整设计审查 + 可选 ST/白盒 harness
确定性检查（编译、schema）始终跑，不依赖 LLM 是否“觉得过了”
```

### 4.5 文件系统即状态

| 产物 | 用途 |
|------|------|
| `environment.md` | 环境门禁 |
| `DESIGN.md` + `PLAN.md` | 设计冻结 |
| `WALKTHROUGH.md` | 设计质疑与仲裁 |
| `REVIEW.md` | 独立审查结论 |
| `precision/summary.txt` | 精度是否验收 |
| `perf/*/summary.txt` | 性能是否归档 |

Weaver 的 `AGENTS.md` 已要求开发说明；建议把上述产物收成 `templates/report/`。

### 4.6 建议 Weaver **不要**原样搬的重流程

- 注册调用 4 阶段 + CP1–CP5 + git checkpoint + 7 Agent：企业上库专用
- PyPTO `state_transition` 状态机
- Triton 生成循环 10 次 + 优化循环 50 次
- 模型推理 SOTA Plan Dashboard
- cuda2ascend CP0–CP5 全套项目管理

这些说明“门禁可以做得很硬”，但 Weaver v0.1 用直调 7 步的简化版就够。

---

## 5. 有价值的 Validation 机制

cannbot 把验证分成 **Skill 自身质量** 和 **算子产出质量** 两层。Weaver 的验证层两层都需要。

### 5.1 Skill 质量：四层测试

| 层 | 位置 | 测什么 |
|----|------|--------|
| L1 单元 | `tests/unit/` | 结构/内容/依赖图/换行符，无需跑 Agent |
| L2 行为 | `tests/behavior/` | 触发是否准、会不会过早改文件、缺参会不会问、会不会编造 API |
| L3 集成 | `tests/integration/` | 简单算子开发是否产出该有的文件 |
| ST 语义 | `tests/system/` | 用真实 Agent 跑 evals，再由评审 Agent 打分 |

官方 Plugin 入库：L1 + L2。社区 Plugin：至少 L1。

### 5.2 L2 九条行为规则（可直接改写成 Weaver eval）

| ID | 规则 |
|----|------|
| B-TRIG-01 | description 里的关键词要能触发 |
| B-TRIG-02 | 口语化说法也要落到正确域 |
| B-SAFE-01 | Skill 加载前禁止破坏性 Write/Edit/Bash |
| B-SAFE-02 | 知识/检视类禁止改业务代码 |
| B-SAFE-03 | 开发类应先做环境检查 |
| B-INTA-01 | 缺参必须提问 |
| B-INTA-02 | 多轮要记住芯片、dtype、Tiling |
| B-BND-01 | 无关问题不要泄漏 CANN 行话 |
| B-BND-02 | 必须拒绝伪造 API / 伪造芯片 |

B-BND-02 对 Weaver 特别重要：知识仓一旦鼓励“猜 API”，就会污染后续所有 Case。

### 5.3 Skill 入库九维评分（`cannbot-skill-reviewer`）

| 维 | 权重 | 看什么 |
|----|------|--------|
| Frontmatter | 8 | 触发条件是否清楚 |
| 工作流清晰度 | 15 | 步骤是否可执行、有输入输出 |
| 边界条件 | 10 | 失败、缺参、无环境怎么办 |
| 检查点 | 7 | 危险写入前是否确认 |
| 指令具体性 | 15 | 有没有具体命令/路径/模板 |
| 资源整合 | 5 | references/scripts 是否有效 |
| 架构适配 | 15 | 是否单职责、放对目录 |
| 可信度与安全 | 10 | 来源是否官方/源码，有无乱联网 |
| 验证证据 | 15 | 有没有 L1/L2/ST/上板，或明确写了缺口 |

结论：`PASS`（≥80 且无成文法 error）/ `CONDITIONAL`（70–79）/ `REJECT`（<70 或安全/来源失败）。

成文法（CI 自动拦）和判例法（人判断“这 Skill 有没有独特价值”）分开，和 Weaver `CURATION_PLAN` §53 一致。

### 5.4 算子产出门禁（可吸收进 templates / eval）

```text
编译：独立构建，不信开发者自报
精度：公式 |actual-golden| ≤ atol + rtol×|golden|
      且 matched_ratio 达标
性能：以 profiling 报告为准，不用裸 wall-clock 做最终裁决
Reviewer 禁止改代码
确定性检查（编译、schema、文件存在）与 LLM 评审解耦
```

`ops-spec-gen` 的 9 段机器校验、直调 `validate_state.py`、flash 的 `test_gate.json`，都是“不要用散文当门禁”。

### 5.5 cann_bench / CAKE 评估（后期）

- 编译成功、精度通过、综合分 >50 才算过
- 多样例几何平均加速比
- 规则命中优先于让 LLM 自由改 kernel

Weaver Phase 8 再接，v0.1 先把“固定任务 + 编译/正确性/迭代次数”立住。

---

## 6. 哪些内容 AscendWeaver 已经有

对照的是 **AscendWeaver 仓库本身**，不是本机 Cursor 里已安装的 cannbot 拷贝。

### 6.1 已经对齐的思想

| Weaver 已有 | 与 cannbot 的对应 |
|-------------|-------------------|
| `README.md` 蓝区开发 / 内网验证闭环 | Plugin 编排 + 上板验证，但 Weaver 明确跨安全域 |
| `AGENTS.md`：先理解再编码、正确性优先、性能要有依据 | 直调 7 步 + precision-debug + profiling 证据规则 |
| `AGENTS.md`：Skill 不堆硬件/API 全文，按需读 knowledge/cases | 渐进式披露（薄 SKILL.md + 厚 references/） |
| `CURATION_PLAN.md`：少而精、事实/推断分开、Case 优先 | cannbot-knowledge 的来源标注与 sufficiency |
| `CURATION_PLAN.md`：Agent 80% 加工 + 人 20% 判断 | 成文法 CI + 判例法 Reviewer |
| `skills/operator-development/SKILL.md` 流程骨架 | `ops-direct-invoke` 的简化版：需求→分类→检索→数据流/切分/内存/流水→实现→构建测试→打点 |
| 规划目录 `skills / knowledge / cases / workflows / templates / eval` | Plugin / Agent / Skill / references / tests，职责切分已经对上 |

### 6.2 只有目录或空文件，实质内容还没有

| Weaver 位置 | cannbot 里已经很满的对应物 |
|-------------|------------------------------|
| `knowledge/hardware/ascend950/` | `npu-arch` + hardware-params |
| `knowledge/ascendc-api/` | `ascendc-api-best-practices` |
| `knowledge/engineering-experience/` | `ascendc-performance-best-practices`、tiling 四要素、debug 陷阱 |
| `knowledge/coding-guidelines/` | `ascendc-code-review` 的 references |
| `skills/operator-debug/SKILL.md`（空） | precision / runtime / crash 三件套 |
| `skills/operator-optimization/SKILL.md`（空） | perf-optimize + profiling + performance-best-practices |
| `skills/instrumentation/SKILL.md`（空） | profiling；cannbot 几乎没有 Cycle 打点 Skill |
| `workflows/*`（空） | 直调 7 步、调试路由 |
| `templates/*`（空） | docs-gen、direct-invoke-template、environment/DESIGN/PLAN/REVIEW |
| `cases/*`（空） | cannbot **几乎不沉淀 Case Card**；这是 Weaver 的差异化 |
| 验证层（空） | `tests/` + skill-reviewer + cann_bench |

### 6.3 本机已有、但不算 Weaver 仓内资产

`/root/.cursor/skills/` 已安装大量 cannbot 同源 Skill（`ascendc-tiling-design`、`npu-arch`、`ops-profiling` 等）。那是 **Cursor 运行时能力**，不是 AscendWeaver 知识仓。Agent 在 Weaver 项目里开发算子时，不应假设用户都装了这些 Skill。

`operator-development` 第 5 步要求搜索 `cases/` 和 `knowledge/`，当前会空手而归。

### 6.4 Weaver 已有、cannbot 较弱的点

- **Case Card 规范**（为什么值得学、学什么、不要抄什么）—— cannbot 以模板和 references 为主，几乎没有模式级 Case
- **蓝区 / 内网职责分离** 与打点分支—— cannbot 默认开发机即 NPU
- **通算融合真实工程**（本机 `urma_moonep` 等）可作为后续 Case，cannbot 的 `mc2-best-practice` 是指南不是案例

---

## 7. 哪些值得吸收

吸收方式：**提炼进 Weaver 的 knowledge / workflows / templates / eval**，不是把 cannbot Skill 目录整份搬进来。License 是 CANN OSL v2.0，复制源码/模板前必须登记来源。

### 7.1 P0（立刻，支撑 v0.1）

| 吸收什么 | 放进 Weaver 哪里 | 怎么吸收 |
|----------|------------------|----------|
| 薄 Skill + 路由表 + 按需 references | 四个 Skill 的写法 | 学结构，不学篇幅 |
| 直调 7 步 + 设计串讲 + 3 轮修复上限 | `workflows/new-operator/` | 简化为 Weaver 蓝区流程；内网验证单独一节 |
| Tiling 四要素清单 | `knowledge/` + development Skill 门禁 | 写代码前必须填这四项 |
| npu-arch 中 DAV_3510/950 事实 | `knowledge/hardware/ascend950/` | 只留对设计有含义的参数，标 Source Type |
| API 黑名单与对齐规则 | `knowledge/ascendc-api/` | Card：用途、限制、常见错、最小例子；Case 用到再扩展 |
| 调试症状路由 | `skills/operator-debug/` | 做成路由器，细节链到 knowledge |
| 精度标准公式与 dtype 缺省 | `knowledge/` + test template | 引用标准，不复制整本表 |
| ❌ 阻塞 vs ⚠ 建议 | 所有门禁 | 格式问题不挡进入下一阶段 |
| 主 Agent 不写代码 / Reviewer 不改代码 | `AGENTS.md` 补强 | 与现有原则合并 |
| 争议仲裁顺序 | workflows | 文档 > Skill > 实测 |
| DESIGN/PLAN/REVIEW/environment 模板 | `templates/report/` | 短模板，字段少而硬 |
| Eval 从第一天就有固定任务 | 验证任务 | 先测：会不会跳过设计、会不会编造 API、会不会声称未测性能 |

### 7.2 P1（第一批 Case 之后）

| 吸收什么 | 放哪里 |
|----------|--------|
| Blaze 路径：950 MatMul 默认走 tensor_api，不走 A2 高阶 API | hardware + Case（GEMM/GEMV） |
| Catlass 组件选型决策（ArchTag、TileShape、DispatchPolicy） | Case + engineering-experience，不当独立 Plugin |
| 性能四层分析 + msprof 对比模式 | `operator-optimization` |
| MC2 路线分类与红线 | 后期 `cases/compute-communication/`，第一批不要 MoE |
| SIMT vs SIMD 选择规则 | hardware + tiling knowledge |
| 直调工程模板的 copy-rename-modify | `templates/operator/`（只留最小 Vector 样例） |
| Skill 九维审查改成 Weaver Case/Knowledge 审查清单 | `CURATION_PLAN` 已有雏形，可对齐九维里的“边界/证据/单职责” |
| 知识查询 sufficiency（1 强或 2 中） | development Skill 的“何时停止检索” |

### 7.3 P2（有明确需求再做）

- `ops-spec-gen` 的 spec.yaml 真源（若 Weaver 要做注册算子上库）
- `torch-ascendc-op-extension`（已有直调工程接 PyTorch 时）
- `ops-simulator`（无板蓝区的编译外验证）
- flash 的简单/复杂分档
- 返回标签 `[PRECISION_PASS]` / `[DESIGN_ERROR]` 方便编排
- L1 结构校验脚本（frontmatter、链接、单职责）的精简版

### 7.4 吸收时必须改写的点

1. **平台默认改成 950**。cannbot 很多 API 卡片仍以 A2/A3 为中心，MatMul 高阶 API 写明限 DAV_2201。Weaver 若照抄会教错。
2. **Knowledge 从 Case 反推**。cannbot 是“先写大全 references”。Weaver 计划相反：先 5 个 Case，再用到的 API/经验才入库。
3. **打点是 Weaver 差异化**。cannbot 强在 msprof 和仿真，弱在 Cycle 级阶段打点。`skills/instrumentation/` 不要从 cannbot 找现成答案。
4. **Case 是 Weaver 差异化**。cannbot 几乎不写 “What NOT to copy”。这正是 Weaver 要补的。

---

## 8. 哪些不建议吸收

### 8.1 整套产品架构

| 不建议 | 原因 |
|--------|------|
| 217 个 Skill 的目录镜像 | Weaver 要的是少而精，不是第二套 cannbot |
| Plugin → 多 Agent → Skill 全量角色图 | v0.1 一个 development Skill + 清晰 workflow 更稳 |
| 把 cannbot `ops/` 下 SKILL.md 原文件拷进来 | 许可证、平台默认、与 Weaver 分层冲突；且会与 Cursor 已装 Skill 重复 |

### 8.2 错误领域

| 不建议 | 原因 |
|--------|------|
| `model-infer-*` / `model-train-*` | 模型推理/训练，不是算子开发知识仓 |
| `graph/torch-npugraph-ex-*`、`ge-fusion-pass-skill` | 图模式与 GE pass |
| `runtime_migration` | CUDA Runtime API 迁移 |
| `science-model-npu-migration` | 框架级科学计算迁移 |
| `model-infer-harmony` | 端侧 Kirin 打包 |
| `gitcode-*` | 协作工具，不是算子能力 |
| `install-helper`、`cannbot-insight` | 安装器与 Web 可观测，重依赖 |

### 8.3 过重或绑定特定工程的流程

| 不建议原样搬 | 原因 |
|--------------|------|
| `ops-registry-invoke` 全 4 阶段 + 7 Agent + git tag | 官方仓上库流程，Weaver 默认直调 |
| `ascendc-st-design` / `whitebox-design` / `ut-develop` 全脚本链 | 绑 ops-math/nn/transformer 布局和 TTK |
| `pypto-*`、`tilelang-*`、`triton-*` 全家桶 | 另一套编程模型；Weaver 主路径是 AscendC |
| `cannbot-dsl`、`ops-easyasc-dsl`、`tilelang2ascendc-*` | 依赖特定 wheel/仓布局 |
| CAKE 全量进化、`ops-perf-evolution`、`ops-qa-suite` | 绑官方算子仓 `build.sh`、GitCode、NPU 农场 |
| cuda2ascend CP0–CP5 项目管理 | 迁移产品，不是知识沉淀 |
| Triton 优化 50 轮状态机、PyPTO `state_transition` | 工具成本高，超出 v0.1 |
| `aiss-tiling-solver` | 外部闭源 CLI + glibc 约束，当工具说明即可，不当核心知识 |

### 8.4 会教错 Weaver Agent 的内容

| 不建议 | 原因 |
|--------|------|
| 把 A2/A3 `MatmulImpl` 写成 950 默认 | 与 blaze-best-practice 冲突 |
| 大段官方 API 手册搬家 | `CURATION_PLAN` 已禁止；docs-search 是 cannbot 的检索器，Weaver 应做短 Card |
| `ascendc-performance-best-practices` 整族模板代码入库 | 体积大、MicroAPI 易过时；应变成 Case + “有模板先复制”这一条规则 |
| 声称“已优化 X%”而无 profiling | 与两边 AGENTS 原则都冲突 |
| 环境安装长篇（`cann-env-setup`） | Weaver Agent 默认在已有 CANN/NPU 或内网环境工作；env-check 的**检查清单**可以要，安装教程不必 |

### 8.5 与 Weaver 流程冲突

```text
cannbot 默认：开发机 = NPU，Agent 直接编译运行
Weaver 默认：蓝区写代码 → 人工搬到内网 → 内网验证 → 搬回 patch

因此不建议吸收：
- “未环境检查通过禁止设计”在蓝区无 NPU 时会卡死
  → 应改成：无 NPU 则标记 Runtime 未测，仍允许完成设计与代码
- 直调 Step 1 强依赖 npu-smi
  → 蓝区改为 CANN 头文件/编译器检查；npu-smi 放到内网 workflow
```

---

## 9. 结论：cannbot 的优秀之处，以及 Weaver 该怎么用

cannbot 真正强的不是 Skill 数量，而是：

1. **Skill 写的是行为**：IF/THEN、MUST NOT、路由表、尝试上限
2. **渐进式披露**：入口短，细节按分类打开
3. **角色隔离 + 文件门禁**：设计、实现、审查分开，用产物而不是聊天记忆推进
4. **验证分层**：结构检查、行为检查、上板证据分开
5. **平台分流写在规则里**：950 走 Blaze，A2 走高阶 Matmul，SIMT 不套 SIMD

AscendWeaver 不要重建 cannbot。下一步（仍不改现有 Skill）建议只做内容建设：

```text
1. 把本文 P0 规则写进 knowledge/ 与 workflows/new-operator/
2. 用 cannbot 的 tiling/API/debug 规则当“检索目标”，从 CATLASS / asc-devkit 做第一批 Case
3. 验证任务先加 2–3 个：跳过设计、伪造 API、无证据报性能 —— 这些是 cannbot L2 已经证明有效的回归项
```

人需要拍板的只有三件事（与 `CURATION_PLAN` §53 一致）：950 默认路径是否冻结为 Blaze；第一批 5 个 Case 是否避开 MoE；蓝区无 NPU 时环境门禁如何降级。
