# 调用图采集方法论（通用，与框架无关）

本文件沉淀"如何从一个 C 函数提取 callers / callees 并组织成调用树"的可复用技巧。
对任意 C/C++ 代码库有效。框架特异内容见 [profile.md](profile.md)。

## 1. 精确定位函数定义

生成调用图前必须先确认目标函数的**唯一定义**，否则 callers/callees 全错。

```
pattern: \bFUNC_NAME\b
glob: **/*.{c,h}
output_mode: content
-n: true
```

逐条判断命中行是哪类：

| 形态 | 判断 | 用途 |
|------|------|------|
| `.c` 中 `RETTYPE FUNC(...)` 后紧跟 `{` | 定义 | 主目标，callees 从这里读 |
| `.h` 中 `RETTYPE FUNC(...);` | 声明 | 取 Doxygen 注释 |
| 出现在别的函数体里 `FUNC(...)` | 调用点 | callers 候选 |
| `static RETTYPE FUNC(...)` | static 定义 | 可能是内部同名版本，单独标注 |
| 注释/字符串里 | 非代码 | 排除 |

注意：
- C 不支持重载，但同一名字可能有一个公开 + 多个 static 版本。按调用点上下文判断用户指的是哪个。
- 函数名作为子串出现在别的标识符里（`create` 命中 `precreate`）→ 用 `\b` 词边界。

## 2. 采集 callers（上游，一层）

callers = "谁直接调用了 FUNC"。

### 2.1 从调用点反查所属函数

对每个调用点 `file:line`：

1. Read 该文件，定位调用点所在行。
2. 往**前**扫，找最近的函数签名行（`RETTYPE FUNC_OWNER(...)` 后的 `{`），FUNC_OWNER 即 caller。
3. 若调用点在函数外的宏/初始化代码里（如 `ops->create = FUNC`），那是**注册点不是调用点**——
   归类为间接调用的注册，不是 caller。

### 2.2 过滤噪声

- 宏里提及 FUNC 名（字符串化 `#FUNC`）→ 排除。
- 注释、文档字符串 → 排除。
- 函数指针表注册 `ops.create_x = FUNC` → 不是 caller，是间接调用的来源（见 §4）。

### 2.3 callers 一般不向上递归

向上递归会快速发散（一个函数的 callers 的 callers 几何增长），且对理解"X 做什么"帮助有限。
默认只列一层 callers。用户明确要"向上追溯"时再递归，且需限深。

## 3. 采集 callees（下游，逐层延展）

callees = "FUNC 直接调用了什么"。这是用户要"依次往下延展"的部分。

### 3.1 提取函数体内的调用

Read FUNC 完整函数体（定义 `{` 到匹配的 `}`），在体内找调用：

```
# 在函数体行范围内 grep 调用形态
pattern: \b\w+\s*\(           # 标识符 + 左括号
```

逐个判断命中是否真为函数调用：

- 是函数调用 → callee 候选。
- 是控制语句（`if (`、`for (`、`while (`、`switch (`、`sizeof (`）→ 排除。
- 是宏调用（全大写或带下划线的宏名）→ 单独标 `（宏展开）`，必要时展开看内部调用。
- 是函数指针调用（`ptr(`、`ops->fn(`、`obj.fn(`）→ 间接调用，标 `（经 ops 表分发）`。

### 3.2 延展策略

对每个 callee，递归执行 §3，形成深度树（此处为采集阶段的逻辑结构示意，最终渲染为 Mermaid，
见 [output-format.md](output-format.md)）：

```
FUNC
├─ callee_A          （定义 file:line，被调于 FUNC:Lxxx）
│   ├─ callee_A1     （定义 file:line，被调于 callee_A:Lyyy）
│   └─ callee_A2
└─ callee_B
    └─ ...
```

**停止条件**（任一满足即停）：

- 达到深度上限（默认 2 层 callee）。
- callee 是叶子函数（函数体无调用，或只调标准库/外部库）。
- callee 已在当前路径出现过（防环，标 `(递归)` 或 `(已出现)` 不展开）。
- callee 经 syscall/ioctl 等 `umdk/` 内看不到实现的下层，标 `(下层不在 umdk 内，止步)`。

### 3.3 防环与去重

- **递归**：FUNC 调 FUNC → 标 `(递归)`，不再展开。
- **跨支重复**：callee_A 和 callee_B 都调 callee_C → callee_C 只在首次（A 支下）完整展开，
  B 支下标 `(见上方 callee_C)`，避免图重复膨胀。

## 4. 处理间接调用（函数指针 / ops 表）

C 代码库常通过函数指针表（ops）实现多态，直接 grep 看不到边。处理：

1. 识别调用形态：`ops->create_jetty(...)` / `obj.fn(...)` / `(*ptr)(...)`。
2. 找 ops 表类型：看 `ops` 的声明类型（如 `urma_ops_t *ops`）。
3. 找注册点：Grep `.create_jetty =` 或 `create_jetty:`（老式初始化），定位实际实现函数。
4. 图中标注：

   ```
   ops->create_jetty()  （经 urma_ops_t 表分发）
     ↓ 注册于: [bdp_ops](path#Lxx) / [udma_ops](path#Lyy) ...
     → 实际实现: impl_create_jetty  → file:line
   ```

5. 若注册点有多个（多驱动注册同一 ops 字段），列出所有实现，标注"按运行时设备选择其一"。

### 4.1 何时展开 ops 实现

默认展开一层 ops 实现（用户通常想知道"真正干活的是谁"）。若实现函数的注册点不在 `umdk/` 内
（ops 表由 `umdk/` 之外的代码注册），止步并标`(实现不在 umdk 内，止步)`。

## 5. 取每个节点的功能解释

调用图的每个函数节点要带一句话解释 + 跳转链接。解释来源优先级：

1. **复用已有索引**：先查当前 skill 的 [profile.md](profile.md) 指向的复用来源（如 umdk-explainer
   的 api-index / data-structures），有现成一句话直接用。
2. **Doxygen 注释**：读函数声明上方的 `/** 描述 */`，取首行。
3. **函数体推断**：读入口校验 + 主逻辑 + 返回值，提炼一句话，标 `（按实现推断）`。
4. **命名拆解**：前缀(模块) + 词根(动作) + 后缀(变体)，标 `（按命名推断）`。最弱，仅作兜底。

解释**一句话**，不超一行。详细语义让用户点链接跳转读代码。

## 6. 行号校验

调用图的边有两类行号，都要真实：

- **定义行号**：函数定义处（节点链接）。取自 grep 定位或 Read 行号前缀。
- **调用点行号**：调用发生处（边标注）。取自调用点 grep 的 `-n`。

抽检：随机选 2~3 条边，Read 调用点行确认确为该调用。失败则整图行号不可信，需重采。

## 7. 何时该沉淀（触发自我更新）

- 反复为不同用户/不同入口现编同一条 callee 子链 → 值得沉淀进 umdk-skill-updater 的
  call-graphs 索引，下次直接引用。
- 发现某 ops 表的字段→实现映射频繁被查 → 沉淀进 profile 的"高频 ops 表清单"。
- api-index 中某函数行号失效 → 提示调 updater 刷新。
