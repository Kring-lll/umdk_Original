---
name: umdk-callgraph
description: 为 UMDK 用户态开发包中的函数生成调用关系图——显示调用该函数的函数（callers）与它调用的函数（callees），callees 可逐层向下延展，所有函数附带功能解释与可点击跳转链接。当用户询问某函数的"调用关系""调用图""谁调用了它""它调用了什么""call graph""调用链"时使用此 skill。本 skill 是一个可复用的"函数调用关系图生成器"，通用方法在 references/methodology.md，输出格式在 references/output-format.md，框架特异内容（覆盖范围、功能解释来源、调用边界规则）在 references/profile.md，移植到其他代码库时只需重写 profile.md。
---
# 函数调用关系图 Skill（umdk-callgraph）

本 skill 为指定函数生成**调用关系图**：上游（谁调用它）、下游（它调用谁，可逐层延展），
每个节点带功能解释和跳转链接。它是 umdk-explainer 的可视化补充——explainer 用文字解释单个符号，
本 skill 用结构化图展示函数间的拓扑。

**覆盖范围**：`umdk/` 目录下的用户态开发包（URMA / URPC / DLock / UMS / CAM 及工具示例）。

## 何时使用

触发条件（用户问以下任一）：

- "X 的调用关系 / 调用图 / call graph / 调用链"
- "谁调用了 X" / "X 被谁调用" / "X 的 callers"
- "X 调用了什么 / 哪些函数" / "X 的 callees"
- "X 的调用树 / 调用层次"
- "把 X 的调用关系画出来"

不适用于：只问单个函数作用不涉及关系（用 umdk-explainer）、问数据流/报文流（非函数调用）、
问整个模块架构（用 umdk-explainer 的 profile 模块地图）。

## 核心原则

1. **接地优先，不臆造边**。每条 caller/callee 边必须能在代码里定位到 `file:line`。
   grep 不到的边不画；不确定的不画。宁可图少，不可图假。
2. **区分直接调用与间接调用**。默认只画**直接调用**（函数体里显式出现的调用）。
   通过函数指针表（ops）的间接调用单独标注 `（经 ops 表分发）`，不假装是直接调用。
3. **callee 可延展，caller 通常不延展**。用户要"依次往下延展"的是 callee 链——
   被调函数再展开它的被调函数，形成深度树。callers 一般只列一层（谁直接调用 X），
   不向上递归（向上会发散且常无意义）。
4. **每个节点带功能解释 + 跳转链接**。解释来自 Doxygen 注释或函数体语义（优先复用
   umdk-explainer 的 api-index/data-structures 已有说明），链接指向定义处 `file:line`。
5. **深度可控**。默认 callee 延展 2 层（X→Y→Z），避免无限递归或图过大。用户可指定深度。
6. **去重与防环**。递归调用（X 调 X）标 `(递归)` 不再展开；同一函数被多处调用只在首次出现时展开。

## 生成工作流

### 第一步：定位目标函数定义

1. Grep `pattern: "\bFUNC\b"`，`glob: **/*.{c,h}`，`output_mode: content`，`-n: true`。
2. 区分：
   - **定义**：`.c` 文件里 `返回类型 FUNC(...)` 后跟 `{`。记 `file:line`。
   - **声明**：`.h` 文件里 `返回类型 FUNC(...);`（行尾分号）。用于取 Doxygen 注释。
   - **调用点**：其余出现。这些是 callers 候选。
3. 若函数有多个同名重载或 static 版本，按用户语境或调用频次判断主目标，其余标注"另有 static 版本"。

### 第二步：采集 callers（上游，一层）

1. 从第一步的调用点清单中，对每个调用点确定其**所属函数**：Read 调用点所在文件，
   往前找最近的函数签名（`返回类型 函数名(...)` 后的 `{`），即为 caller。
2. 每个 caller 记：函数名 + 定义 `file:line` + 调用点 `file:line`（调用发生在哪一行）。
3. 过滤：排除宏展开、注释里的提及、字符串里的字面量。

### 第三步：采集 callees（下游，逐层延展）

1. Read 目标函数**完整函数体**（从定义 `{` 到匹配的 `}`）。
2. 提取函数体内的调用：
   - 直接调用：`func(...)` 形态。Grep `pattern: "\b\w+\s*\("` 在函数体行范围内，逐个确认是函数调用而非控制语句。
   - 间接调用：`ops->func(...)` / `ops.func(...)` / `(*func_ptr)(...)`。标注 `（经 ops 表分发）`，
     并尽量定位 ops 表注册点（`.func = impl`）写出实际实现函数，标注 `（注册于 ops）`。
   - 宏调用：常见校验宏（如 `URMA_CHECK_OP_INVALID_RETURN_POINTER`）展开后含调用，标注 `（宏展开）`。
3. 每个 callee 记：函数名 + 定义 `file:line` + 被调点 `file:line`。
4. **延展**：对每个 callee 重复第三步，直到达到深度上限或无可延展（叶子函数）。

### 第四步：取功能解释

对图中每个函数节点：

1. 优先查 [profile.md](references/profile.md) 指向的复用来源（umdk-explainer 的
   [api-index.md](../umdk-explainer/references/api-index.md) / [data-structures.md](../umdk-explainer/references/data-structures.md)）
   是否已有该函数的一句话说明——有则直接引用。
2. 没有则读该函数头文件声明上方的 Doxygen `/** */` 注释，取首行描述。
3. 无 Doxygen 的内部/static 函数，读函数体入口校验和主逻辑，提炼一句话，标注"（按实现推断）"。
4. 解释统一一句话，不超过一行；详细语义让用户点链接跳转后自行读。

### 第五步：按输出格式组装

按 [output-format.md](references/output-format.md) 的模板渲染为 **Mermaid flowchart**。核心结构：

- 一句话引述：目标函数、深度、覆盖范围提示。
- 一个 Mermaid `flowchart` 代码块：上游 callers（箭头指向目标）、目标节点（粗框高亮）、
  下游 callees（目标指向，逐层延展）；用 `subgraph` 分上游/目标/下游三区。
- 直接调用实线、间接调用(ops/宏)虚线、递归自指、不可达下层(syscall/ioctl)标注止步、叶子浅灰。
- 节点显示文本：`FUNC_NAME — 一句话解释 → file:line`（纯文本，Mermaid 节点内不支持 markdown 链接）。
- 图**下方附节点跳转表**：把每个有明确落点的函数映射到可点击的 `[file:line](path#L行号)` 链接。

### 第六步：校验

- 抽检 2~3 条边：Read 调用点行确认确实是该调用。
- 确认所有 `file:line` 链接真实存在。
- 确认无臆造边（grep 不到的函数不进图）。
- 确认深度符合用户要求或默认 2 层。

## 回答规范

### 必做

- 开头一句话说明：目标函数是什么、图覆盖了什么深度。
- 上游/下游分开展示，结构清晰。
- **每个节点带跳转链接和一句话解释**——这是本 skill 与 explainer 纯文字答疑的区别。
- 链接用 markdown `[FUNC](相对路径#L行号)`，路径相对仓库根。
- 间接调用、宏展开、递归明确标注，不混同直接调用。

### 禁止

- 画 grep 不到的边。
- 把 ops 表分发假装成直接调用。
- 无限延展导致图爆炸——达到深度上限就停，标"↓ 可继续延展，当前止于 N 层"。
- 节点不带链接或带错误行号（用户点空比没链接更糟）。
- 解释写成大段（每节点一句话即可）。

## 与其他 skill 的协作

- **umdk-explainer**：本 skill 复用其 profile、api-index、data-structures 作为功能解释来源和
  命名约定。两者覆盖范围一致（仅 `umdk/`）。explainer 管"单个符号是什么"，本 skill 管"符号之间怎么连"。
- **umdk-skill-updater**：当本 skill 反复现编同一条调用子链，或 api-index 中某函数行号失效，
  提示用户调 updater 刷新（updater 的 call-graphs 索引可沉淀高频子链）。
- **c-code-standard**：本 skill 只读不改代码，与编码规范 skill 无交集。

## 框架知识加载

回答前读 [profile.md](references/profile.md)。它包含：

- 覆盖范围（`umdk/` 目录）。
- 功能解释复用来源（指向 umdk-explainer 的索引文件）。
- 调用边界规则（ops 表/syscall/ioctl/宏如何标注）。
- 本框架的高频 ops 表与分发宏清单（帮助识别间接调用）。

按需读 [methodology.md](references/methodology.md)（通用采集技巧）和 [output-format.md](references/output-format.md)（输出模板与示例）。

## 通用性

本 skill 的生成方法（定位/callers/callees/延展/取解释/校验）与框架无关，适用任意 C 代码库。
移植时重写 [profile.md](references/profile.md)（覆盖范围 + 复用来源 + 边界规则 + 框架特异 ops/宏清单），
methodology.md 与 output-format.md 原样复用。
