---
name: umdk-skill-updater
description: 扫描 UMDK 代码库，自动刷新 umdk-explainer（及其他学习型 skill）的框架特异索引文件——api-index / data-structures / call-graphs / profile 的模块地图与概念速查。当用户说"更新 skill / 刷新索引 / skill 过时了 / 同步代码变更到 skill"，或 explainer 提示某符号已失效、某调用链值得沉淀时使用。本 skill 是一个可复用的"代码→skill 索引"提取器，通用提取逻辑在 extraction-patterns.md，框架特异的目标文件与采集锚点在 profile.md，移植到其他框架时只需重写 profile.md。
---

# Skill 索引更新器（umdk-skill-updater）

本 skill 不回答代码问题，而是**维护回答代码问题的 skill**（umdk-explainer 及未来的 guide/quickstart）。
它扫描代码库，把符号、结构体、调用链重新提取到 explainer 的 references 索引文件，让 explainer 始终与代码同步。

## 何时使用

- 用户主动要求："更新 skill" / "刷新索引" / "skill 过时了" / "代码改了同步一下 skill"。
- umdk-explainer 在回答时发现索引失效（符号在代码里找不到、行号错位）并提示调用本 skill。
- umdk-explainer 反复现编同一条调用链，值得沉淀。
- 大规模代码变更后（如升级 UMDK 版本、合入大 PR）预防性刷新。

## 核心原则

1. **只改索引文件，不改方法论**。SKILL.md 与 methodology.md 是通用方法论，原则上不动；
   刷新对象是 profile.md 的模块地图/概念速查，以及 api-index / data-structures / call-graphs 三个索引文件。
2. **采集自真实代码，行号实测**。每个符号、字段、调用点都用 Grep/Read 验证存在并取真实行号。
   采集不到的标注"待补充"，绝不保留过期条目假装完整。
3. **增量优先**。默认只刷新用户指定或失效的部分，不全量重建（除非用户要求 `--full`）。
4. **保留人工沉淀内容**。call-graphs 中人工写的链路骨架、profile 中人工补充的概念说明，更新时保留，
   只刷新行号与符号存在性；不擅自删除人工注释。
5. **变更可追溯**。每次更新在 profile.md 的"变更记录"追加一行（日期 + 变更 + 采集范围）。

## 更新工作流

### 第一步：确定更新范围

读 [profile.md](references/profile.md) 的"目标 skill 与文件清单"，确认要刷新哪些文件。
默认增量模式：只更新用户点名或 explainer 标记失效的部分。

若用户要全量刷新，按 profile 清单逐个走。

### 第二步：按文件类型采集

每种索引文件有对应的提取模式，详见 [extraction-patterns.md](references/extraction-patterns.md)。

简表：

| 目标文件 | 采集对象 | 主要手段 |
|----------|----------|----------|
| `api-index.md` | 公开 API 函数 | Grep 头文件函数签名 + Doxygen 注释 |
| `data-structures.md` | 结构体/枚举/宏 + 字段 | Grep `typedef struct` + 读行内注释 |
| `call-graphs.md` | 跨层调用链 | 从入口 Grep 调用点，逐跳定位 |
| `profile.md` | 模块地图/概念/命名约定 | 读目录结构 + 头文件 + 文档目录 |

### 第三步：比对与合并

对每个目标文件：

1. 读现有文件，提取已有符号清单（行号 + 一句话）。
2. 用新采集结果比对：
   - **新增**（代码有、索引无）→ 追加。
   - **移除**（索引有、代码无）→ 删除或标"已废弃"。
   - **行号变化** → 用新行号覆盖。
   - **人工内容**（链路骨架、概念说明）→ 保留，只更新其引用的行号。
3. 生成新文件内容，用 Write 覆盖。

### 第四步：校验

刷新后抽检：

- 随机抽 3~5 个新行号，用 Read 验证指向的确实是该符号。
- 检查 markdown 链接路径相对仓库根正确。
- 确认未引入代码中不存在的符号。

### 第五步：记录

在 [profile.md](references/profile.md) 的"变更记录"表追加一行：

```
| YYYY-MM-DD | 刷新 api-index URMA 段 + call-graphs 链路1行号 | 头文件 grep + 实测 |
```

## 与 umdk-explainer 的协作

- explainer 回答时若发现索引失效，会在回答末尾提示"建议调用 umdk-skill-updater 刷新 X"。
- 本 skill 更新后，explainer 下次回答自动用新索引（无需任何激活动作）。
- 本 skill 不直接回答用户代码问题；若用户问的是代码本身，提示转用 umdk-explainer。

## 输出规范

更新完成后向用户报告：

- 刷新了哪些文件、各增/删/改多少条。
- 抽检结果（行号命中率）。
- 发现的异常（如某模块头文件丢失、符号大范围变更需人工确认）。
- 下次建议刷新的触发条件。

## 通用性

本 skill 的提取逻辑（extraction-patterns.md）与框架无关，适用于任意 C 代码库。
移植到其他框架时：重写 [profile.md](references/profile.md) 的"目标 skill 文件清单"与"采集锚点"
（哪些头文件是 API 源、哪些目录是模块入口），extraction-patterns.md 原样复用。
