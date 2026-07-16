# URMA 日志回调合并设计文档

## 1. 概述

### 1.1 背景

当前 URMA 日志子系统定义了两套日志回调类型及其对应的注册 API：

| 回调类型 | 签名 | 注册 API |
|---------|------|---------|
| `urma_log_cb_t` | `void (int level, char *message)` | `urma_register_log_func()` |
| `urma_loc_log_cb` | `void (int level, const char *file, const char *function, int line, char *message)` | `urma_register_loc_log_func()` |

两套回调语义重叠、互斥注册（后注册的覆盖前者）。`urma_loc_log_cb` 已定义并导出，其注册接口 `urma_register_loc_log_func()` 并非"无使用者"：`umq` 库通过 `dlopen` 动态加载该符号（见 `src/urpc/umq/umq_ub/core/umq_ub_impl.c` 的 `umq_ub_log_config_set_impl`，经 `src/urpc/umq/umq_ub/core/private/umq_symbol_private.c` 解析），将上层 `umq_log_ext_func_t` 回调转发给 liburma。这种"liburma 维护两套回调 + umq 再做一层桥接"的重复设计增加了库的维护成本和用户的理解负担。

### 1.2 目标

将两个回调类型合并为**一个**统一的回调类型，通过 C 语言变参机制（`...`）将 file/function/line 等位置信息作为可选参数传递。用户注册一个回调，根据 `flags` 标识决定是否解析变参中的位置信息。

### 1.3 设计原则

1. **向后兼容** — 现有注册 `urma_register_log_func` 的用户，只需调整回调签名，逻辑代码基本不变。
2. **零开销抽象** — 不使用变参时，不产生额外参数压栈和解析负担。
3. **显式而非隐式** — 通过 `flags` 位明确告知回调当前提供哪些额外参数，无需猜测。
4. **单一职责** — 不再维护两套回调指针和分支逻辑，内部实现简化。

---

## 2. 接口变更详述

### 2.1 回调类型定义

**文件:** `src/urma/lib/urma/core/include/urma_types.h`

#### 删除

```c
// 删除 — 简单回调（无位置信息）
typedef void (*urma_log_cb_t)(int level, char *message);

// 删除 — 位置回调（固定 file/function/line 参数）
typedef void (*urma_loc_log_cb)(int level, const char *file, const char *function, int line, char *message);
```

#### 新增

```c
/* Log callback flags — 一位 flag 对应一个变参 */
#define URMA_LOG_FLAG_FILE (1u << 0)   /* 变参: const char *file     */
#define URMA_LOG_FLAG_FUNC (1u << 1)   /* 变参: const char *function */
#define URMA_LOG_FLAG_LINE (1u << 2)   /* 变参: int line             */

/**
 * Unified log callback type.
 *
 * @param level     Log level (urma_vlog_level_t)
 * @param flags     Bitmask of URMA_LOG_FLAG_*; each set bit corresponds to exactly
 *                  one variadic argument, present in the order FILE → FUNC → LINE.
 * @param message   Pre-formatted log message string
 * @param ...       Optional variadic arguments, gated bit-by-bit by @c flags
 *
 * Variadic argument layout (fixed order, one arg per flag bit):
 *   flags & URMA_LOG_FLAG_FILE → va_arg(ap, const char *) — file name
 *   flags & URMA_LOG_FLAG_FUNC → va_arg(ap, const char *) — function name
 *   flags & URMA_LOG_FLAG_LINE → va_arg(ap, int)          — line number
 *
 * Two hard rules for variadic args:
 * 1. Order is fixed. Callbacks MUST call va_arg() in the order FILE → FUNC →
 *    LINE. Skipping a set bit or reordering is undefined behavior.
 * 2. Presence is prefix-contiguous. If a bit is set, every lower-numbered bit
 *    must also be set (no "LINE without FUNC"). In practice the library only
 *    emits "all three set" (location path) or "none set" (plain path), so the
 *    prefix constraint is naturally satisfied.
 *
 * Example — minimal handler (ignore location):
 *   void my_log(int level, unsigned int flags, const char *message, ...) {
 *       (void)flags;
 *       fprintf(stderr, "%s\\n", message);
 *   }
 *
 * Example — with location parsing (one flag bit per argument):
 *   void my_log(int level, unsigned int flags, const char *message, ...) {
 *       const char *file = NULL, *function = NULL;
 *       int line = 0;
 *       if (flags != 0) {
 *           va_list ap;
 *           va_start(ap, message);
 *           if (flags & URMA_LOG_FLAG_FILE) { file     = va_arg(ap, const char *); }
 *           if (flags & URMA_LOG_FLAG_FUNC) { function = va_arg(ap, const char *); }
 *           if (flags & URMA_LOG_FLAG_LINE) { line    = va_arg(ap, int); }
 *           va_end(ap);
 *       }
 *       if (file != NULL) {
 *           fprintf(stderr, "[%s:%s:%d] %s", file, function, line, message);
 *       } else {
 *           fprintf(stderr, "%s", message);
 *       }
 *   }
 */
typedef void (*urma_log_cb_t)(int level, unsigned int flags, const char *message, ...);
```

### 2.2 注册 API 声明

**文件:** `src/urma/lib/urma/core/include/urma_api.h`

#### 删除

```c
urma_status_t urma_register_loc_log_func(urma_loc_log_cb func);   // 删除
```

#### 修改

`urma_register_log_func` 的函数签名不变（参数类型仍为 `urma_log_cb_t`），但语义扩充：注册的这个回调将**同时**处理有位置信息和无位置信息的日志消息。

```c
/**
 * User register own log function, default rsyslog.
 * @param[in] func: log callback function (urma_log_cb_t);
 *   When invoked, the @c flags parameter indicates, bit by bit, which
 *   location variadic args (file/function/line) are present.
 *   See URMA_LOG_FLAG_FILE / URMA_LOG_FLAG_FUNC / URMA_LOG_FLAG_LINE.
 * Return: 0 on success, other value on error
 */
urma_status_t urma_register_log_func(urma_log_cb_t func);
```

`urma_unregister_log_func` 保持不变。

### 2.3 变更摘要 — 符号对比

| 项目 | 修改前 | 修改后 |
|------|--------|--------|
| 回调签名 | `(int level, char *msg)` <br>`(int level, const char *file, const char *func, int line, char *msg)` | `(int level, unsigned int flags, const char *msg, ...)` |
| 回调类型数量 | 2 个 | 1 个 |
| 注册函数数量 | 2 个 | 1 个 |
| flags 位定义 | 无 | `URMA_LOG_FLAG_FILE` / `URMA_LOG_FLAG_FUNC` / `URMA_LOG_FLAG_LINE`（一位一参） |
| 位置参数传递方式 | 固定参数 | 变参（flags 逐位指示） |

> **适用范围说明**：本章（第 2 节）描述的接口变更**仅覆盖 liburma 自身对外暴露的符号**（`urma_log_cb_t`、`urma_register_log_func`）。`umq`/`urpc` 下游库通过 `dlopen` 间接依赖被删除的 `urma_register_loc_log_func`，其类型与桥接代码的同步变更见 [§4.6](#46-下游库umqurpc适配方案) 与 [§6 涉及文件清单](#6-涉及文件清单)。

---

## 3. 内部实现变更

### 3.1 状态简化

**文件:** `src/urma/lib/urma/core/urma_log.c`

#### 静态全局变量

```c
// 修改前 — 3 个全局变量 + 两个分支路径
static urma_log_cb_t g_urma_log_func = urma_default_log_func;
static urma_loc_log_cb g_urma_loc_log_func = NULL;     // ← 删除
static bool g_use_loc_log = false;                     // ← 删除

// 修改后 — 仅 1 个全局变量
static urma_log_cb_t g_urma_log_func = urma_default_log_func;
```

#### 默认回调函数

```c
// 修改前
static void urma_default_log_func(int level, char *message)
{
    syslog(level, "%s", message);
}

// 修改后
static void urma_default_log_func(int level, unsigned int flags, const char *message, ...)
{
    (void)flags;   // 默认回调忽略位置信息
    syslog(level, "%s", message);
}
```

### 3.2 注册/注销函数

#### `urma_register_log_func`

```c
// 修改前 — 需清除 g_urma_loc_log_func 和 g_use_loc_log
urma_status_t urma_register_log_func(urma_log_cb_t func)
{
    if (func == NULL) {
        URMA_LOG_ERR("Invalid parameter.\n");
        return URMA_EINVAL;
    }
    URMA_LOG_INFO("registered log successfully.\n");
    g_urma_log_func = func;
    g_urma_loc_log_func = NULL;
    g_use_loc_log = false;
    return URMA_SUCCESS;
}

// 修改后 — 直截了当
urma_status_t urma_register_log_func(urma_log_cb_t func)
{
    if (func == NULL) {
        URMA_LOG_ERR("Invalid parameter.\n");
        return URMA_EINVAL;
    }
    g_urma_log_func = func;
    return URMA_SUCCESS;
}
```

#### `urma_register_loc_log_func` — 删除

此函数不再存在。用户只需调用一个注册函数。

#### `urma_unregister_log_func`

```c
// 修改前 — 清除两个指针 + 标志位
urma_status_t urma_unregister_log_func(void)
{
    // ... 打印注销消息 ...
    (*g_urma_log_func)((int)URMA_VLOG_LEVEL_INFO, logmsg);
    g_urma_log_func = urma_default_log_func;
    g_urma_loc_log_func = NULL;      // ← 删除
    g_use_loc_log = false;           // ← 删除
    return URMA_SUCCESS;
}

// 修改后
urma_status_t urma_unregister_log_func(void)
{
    // ... 打印注销消息 ...
    (*g_urma_log_func)((int)URMA_VLOG_LEVEL_INFO, 0, logmsg);
    g_urma_log_func = urma_default_log_func;
    return URMA_SUCCESS;
}
```

### 3.3 调用点修改（核心路由逻辑）

#### `urma_vlog` — 无位置信息的路径

当内部代码通过 `urma_log()`（非宏）直接调用时，不携带 file/function/line：

```c
// 修改前
(*g_urma_log_func)((int)level, logmsg);

// 修改后 — flags = 0，无变参
(*g_urma_log_func)((int)level, 0, logmsg);
```

#### `urma_vlog_loc` — 有位置信息的路径（核心路径）

所有 `URMA_LOG(...)` 宏均走此路径，携带 file/function/line：

```c
// 修改前 — 需要运行时检查 g_use_loc_log 和 g_urma_loc_log_func
static int urma_vlog_loc(..., va_list va)
{
    // ... 格式化 logmsg ...
    if (g_use_loc_log && g_urma_loc_log_func != NULL) {
        (*g_urma_loc_log_func)((int)level, file, function, line, logmsg);
    } else {
        (*g_urma_log_func)((int)level, logmsg);
    }
    return ret;
}

// 修改后 — 统一调用变参回调，三位 flags 全置，按序压三个变参
static int urma_vlog_loc(..., va_list va)
{
    // ... 格式化 logmsg ...
    (*g_urma_log_func)((int)level,
                       URMA_LOG_FLAG_FILE | URMA_LOG_FLAG_FUNC | URMA_LOG_FLAG_LINE,
                       logmsg, file, function, line);
    return ret;
}
```

> **关键变化**：删除了运行时的二分支路由。无论用户注册何种回调，`urma_vlog_loc` 始终统一调用 `g_urma_log_func` 并携带三位位置变参。由回调自身根据 `flags` 逐位决定是否解析。

### 3.4 内部调用图（修改后）

```
URMA_LOG(...) macro  (urma_log.h)
  │
  ▼
urma_log_loc(file, func, line, level, ...)   (urma_log.c)
  │
  ▼
urma_vlog_loc(file, func, line, level, ...)
  │
  │  // 格式化消息 → logmsg
  │
  ▼
(*g_urma_log_func)(level, URMA_LOG_FLAG_FILE|URMA_LOG_FLAG_FUNC|URMA_LOG_FLAG_LINE,
                   logmsg, file, function, line)
  │
  ├── 默认: urma_default_log_func() → syslog()
  └── 用户注册: 用户回调 → 自定处理

urma_log(func, line, level, ...)  ← 极少使用
  │
  ▼
urma_vlog(func, line, level, ...)
  │
  ▼
(*g_urma_log_func)(level, 0, logmsg)
```

### 3.5 umq 桥接层符号指针合并

"两种注册函数指针"不仅在 liburma 内部持有（§3.1 的 `g_urma_log_func` + `g_urma_loc_log_func`），还镜像存在于 `umq` 桥接层：`umq` 通过 `dlopen` 把两个注册符号都加载进一张函数指针表。合并须同步收敛这张表，否则 `umq` 仍持有指向已删除符号的指针，导致加载失败或运行时崩溃。

涉及文件：
- `src/urpc/umq/umq_ub/core/private/umq_symbol_private.h` — 符号表结构体与 typedef
- `src/urpc/umq/umq_ub/core/private/umq_symbol_private.c` — `dlopen` 加载逻辑
- `src/urpc/umq/umq_ub/core/umq_ub_impl.c` — 转发调用点

#### 3.5.1 符号表 typedef 合并

```c
/* umq_symbol_private.h — 修改前：两个 typedef */
typedef urma_status_t (*urma_register_log_func_t)(urma_log_cb_t func);
typedef urma_status_t (*urma_register_loc_log_func_t)(urma_loc_log_cb func);   // ← 依赖被删的 urma_loc_log_cb

/* umq_symbol_private.h — 修改后：删除 urma_register_loc_log_func_t */
typedef urma_status_t (*urma_register_log_func_t)(urma_log_cb_t func);
/* urma_register_loc_log_func_t 整体删除 */
```

#### 3.5.2 符号表结构体字段合并

```c
/* umq_symbol_private.h — 符号表结构体（修改前） */
struct umq_urma_symbol {
    ...
    // Log
    urma_log_set_level_t urma_log_set_level;
    urma_register_log_func_t urma_register_log_func;
    urma_register_loc_log_func_t urma_register_loc_log_func;   // ← 删除
    urma_unregister_log_func_t urma_unregister_log_func;
    ...
};

/* 修改后：结构体仅保留单一注册指针 */
struct umq_urma_symbol {
    ...
    // Log
    urma_log_set_level_t urma_log_set_level;
    urma_register_log_func_t urma_register_log_func;
    urma_unregister_log_func_t urma_unregister_log_func;
    ...
};
```

#### 3.5.3 dlopen 加载逻辑合并

```c
/* umq_symbol_private.c — 修改前：两行 LOAD_SYMBOL */
LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_register_log_func_t, urma_register_log_func);
LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_register_loc_log_func_t, urma_register_loc_log_func);   // ← 删除

/* 修改后：仅加载单一注册符号 */
LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_register_log_func_t, urma_register_log_func);
```

> 若 `urma_register_loc_log_func` 已从 liburma 导出表删除，保留对应 `LOAD_SYMBOL` 会在 `dlsym` 时返回 NULL，导致 `umq` 初始化失败。故必须同步删除。

#### 3.5.4 转发调用点合并

```c
/* umq_ub_impl.c — umq_ub_log_config_set_impl（修改前） */
if (config->log_flag & UMQ_LOG_FLAG_EXT_FUNC) {
    return umq_symbol_urma()->urma_register_loc_log_func(config->ext_func);   // ← 指针已删
}
if (config->log_flag & UMQ_LOG_FLAG_FUNC) {
    return umq_symbol_urma()->urma_register_log_func(config->func);
}

/* 修改后：EXT_FUNC 与 FUNC 均走单一注册指针 */
if (config->log_flag & UMQ_LOG_FLAG_EXT_FUNC) {
    return umq_symbol_urma()->urma_register_log_func(config->ext_func);
}
if (config->log_flag & UMQ_LOG_FLAG_FUNC) {
    return umq_symbol_urma()->urma_register_log_func(config->func);
}
```

> EXT_FUNC 分支改走 `urma_register_log_func` 成立的前提是 `umq_log_ext_func_t` 已收敛为新变参签名（见 [§4.6.2](#462-umq-路径带位置信息原走-urma_register_loc_log_func)），与 `urma_log_cb_t` 类型一致，方可经同一指针注册。

#### 3.5.5 合并前后对照

| 维度 | liburma 内部（§3.1） | umq 桥接层（§3.5） |
|------|---------------------|-------------------|
| 合并前持有指针 | `g_urma_log_func` + `g_urma_loc_log_func` | 符号表字段 `urma_register_log_func` + `urma_register_loc_log_func` |
| 合并后 | 仅 `g_urma_log_func` | 仅 `urma_register_log_func` |
| 路由分支 | `urma_vlog_loc` 删二分支，统一调 `g_urma_log_func` | `umq_ub_log_config_set_impl` 的 EXT_FUNC 分支改调 `urma_register_log_func` |
| 依赖删除符号 | 删 `urma_register_loc_log_func` 实现 | 删 `LOAD_SYMBOL` 行 + 结构体字段 + typedef |

> 两层合并是**原子约束**：liburma 删除 `urma_register_loc_log_func` 导出符号后，umq 桥接层若不同步删除对该符号的 `dlsym`，将导致 `umq` 日志配置初始化失败。故 §3.1 与 §3.5 必须同批提交。

---

## 4. 应用适配指南

### 4.1 场景一：忽略位置信息（最简迁移）

这是当前所有用户（如 perftest、ping）的适配方式。如果应用在回调中只需要消息内容，不需要关注日志来自哪个文件/函数/行：

```c
/* ---------- 修改前 ---------- */
static void my_log_cb(int level, char *message)
{
    // 用户逻辑，只使用 message
    fprintf(stderr, "%s", message);
}
urma_register_log_func(my_log_cb);    // OK

/* ---------- 修改后 ---------- */
static void my_log_cb(int level, unsigned int flags, const char *message, ...)
{
    (void)flags;  // 显式忽略位置信息
    // 用户逻辑不变
    fprintf(stderr, "%s", message);
}
urma_register_log_func(my_log_cb);    // 同一个 API，不需要修改注册语句
```

> **忽略 flags 与变参是安全的**：被调用方对变参 `...` 没有读取义务。即便 liburma 在位置路径传入**非 0 flags + 三个变参**（file/function/line），本例这种完全不调用 `va_start`/`va_arg` 的回调也是**符合标准的 C 代码**，无未定义行为、无栈失衡、无资源泄漏。变参的栈平衡由调用方（liburma）按 ABI 负责，与回调是否读取无关。应用放弃的只是"位置元信息"，不影响 `message` 本身。这也是 perftest/ping 当前的迁移方式。

> **改动量**: 每处回调定义仅需：
> 1. 增加 `unsigned int flags` 参数
> 2. 变参占位（`...`）
> 3. `message` 类型由 `char *` 改为 `const char *`
> 4. 添加 `(void)flags;`

### 4.2 场景二：选择性解析位置信息

如果应用在某些场景下需要日志位置，但在其他场景下忽略。一位 flag 对应一个变参，逐位判断、按序取出：

```c
static void my_log_cb(int level, unsigned int flags, const char *message, ...)
{
    const char *file = NULL, *function = NULL;
    int line = 0;

    if (flags != 0) {
        /* 至少有一位位置 flag 被置，变参区存在。
         * 每位独立判断是否解析；但 va_arg 的取出顺序必须与压栈顺序一致
         * （FILE → FUNC → LINE），不得跳过或乱序。 */
        va_list ap;
        va_start(ap, message);
        if (flags & URMA_LOG_FLAG_FILE) { file     = va_arg(ap, const char *); }
        if (flags & URMA_LOG_FLAG_FUNC) { function = va_arg(ap, const char *); }
        if (flags & URMA_LOG_FLAG_LINE) { line     = va_arg(ap, int); }
        va_end(ap);
    }

    if (file != NULL) {
        fprintf(stderr, "[%s:%s:%d] %s", file, function, line, message);
    } else {
        /* flags == 0（无任何位置位），变参区不存在，不得 va_arg。 */
        fprintf(stderr, "%s", message);
    }
}
```

### 4.3 变参解析规则（重要约束）

| 规则 | 说明 |
|------|------|
| **一位一参** | 每个位置 flag 位对应恰好一个变参：`URMA_LOG_FLAG_FILE`→file、`URMA_LOG_FLAG_FUNC`→function、`URMA_LOG_FLAG_LINE`→line。 |
| **顺序固定** | 变参按压栈顺序 `FILE → FUNC → LINE` 排列。回调的 `va_arg` 调用顺序必须与此一致，**不得跳过已置位、不得乱序**，否则行为未定义。 |
| **前缀连续** | 若某位被置，则其所有低位必须也被置（不能"有 LINE 但无 FUNC"）。库实践中只发"三位全置"或"全不置"，前缀约束自然满足。 |
| **类型匹配** | file/function 为 `const char *`，line 为 `int`。必须用对应类型的 `va_arg` 取出。 |
| **非置位时禁止解析** | 当 `flags == 0`（无任何位置位）时，变参区域**不存在**任何额外参数，不得调用 `va_start`/`va_arg`。 |
| **单次遍历** | 每个回调调用中变参最多解析一次，不允许重复 `va_start`。 |
| **va_end 配对** | 每次 `va_start` 必须配对 `va_end`。 |

### 4.4 错误处理模式

以下为常见错误用法及后果：

```c
/* ❌ 错误：未检查 flags 就解析变参 */
void bad_cb(int level, unsigned int flags, const char *msg, ...)
{
    va_list ap;
    va_start(ap, msg);
    const char *f = va_arg(ap, const char *);  // 如果 flags == 0，行为未定义！
    va_end(ap);
}

/* ❌ 错误：假设永远有位置信息 */
void bad_cb2(int level, unsigned int flags, const char *msg, ...)
{
    (void)flags;
    va_list ap;
    va_start(ap, msg);
    const char *f = va_arg(ap, const char *);  // urma_unregister_log_func 调用时不携带变参
    va_end(ap);
}

/* ✓ 正确：先检查 flags 再解析，一位一参、按序取出 */
void good_cb(int level, unsigned int flags, const char *msg, ...)
{
    const char *f = NULL, *fn = NULL;
    int ln = 0;
    if (flags != 0) {
        va_list ap;
        va_start(ap, msg);
        if (flags & URMA_LOG_FLAG_FILE) { f  = va_arg(ap, const char *); }
        if (flags & URMA_LOG_FLAG_FUNC) { fn = va_arg(ap, const char *); }
        if (flags & URMA_LOG_FLAG_LINE) { ln = va_arg(ap, int); }
        va_end(ap);
        // 使用 file/function/line
    } else {
        // flags == 0，仅使用 message
    }
}
```

### 4.5 代码迁移检查清单

对现有应用的每处回调定义：

- [ ] 参数列表从 `(int level, char *message)` → `(int level, unsigned int flags, const char *message, ...)`
- [ ] 添加 `(void)flags;` 或添加 `flags & URMA_LOG_FLAG_FILE/FUNC/LINE` 检查分支
- [ ] 如需解析位置信息，遵循"一位一参、按序 `va_arg`（FILE→FUNC→LINE）"——见 [§4.3](#43-变参解析规则重要约束)
- [ ] 如果原回调没有使用 `message` 之外的内容，不需要引入变参解析代码
- [ ] `urma_register_log_func(my_cb)` 调用语句**无需修改**
- [ ] 删除 `urma_register_loc_log_func(...)` 调用（如果有）

### 4.6 下游库（umq/urpc）适配方案

`umq` 与 `urpc` 通过 `urma_register_log_func` / `urma_register_loc_log_func` 转发上层回调，本次合并须同步修改以下两条路径，否则会导致下游库编译失败或与 liburma 的 ABI 不一致。

#### 4.6.1 urpc 路径（简单回调，无位置信息）

`urpc` 上层类型 `urpc_log_func_t` 原签名与旧 `urma_log_cb_t` 对齐：

```c
/* urpc_framework_types.h — 修改前 */
typedef void (*urpc_log_func_t)(int level, char *log_msg);
typedef struct urpc_log_config {
    ...
    urpc_log_func_t func;
    ...
} urpc_log_config_t;

/* urpc_lib_log.c — 调用点（修改前） */
urma_register_log_func(config->func);
```

适配方式与 [§4.1](#41-场景一忽略位置信息最简迁移) 相同：将 `urpc_log_func_t` 收敛为新变参签名，回调实现补 `flags`/`...` 占位即可，注册调用语句不变。

```c
/* urpc_framework_types.h — 修改后 */
typedef void (*urpc_log_func_t)(int level, unsigned int flags, const char *log_msg, ...);

/* urpc_lib_log.c — 调用点（修改后，语句不变） */
urma_register_log_func(config->func);
```

#### 4.6.2 umq 路径（带位置信息，原走 `urma_register_loc_log_func`）

`umq` 原通过 `dlopen` 加载 `urma_register_loc_log_func`，将上层 `umq_log_ext_func_t`（5 参数，与旧 `urma_loc_log_cb` 对齐）转发给 liburma。由于 `urma_register_loc_log_func` 已删除，此路径必须改造：**改走 `urma_register_log_func`，并将 `umq_log_ext_func_t` 收敛为统一变参回调**。

```c
/* umq_types.h — 修改前 */
typedef void (*umq_log_func_t)(int level, char *log_msg);
typedef void (*umq_log_ext_func_t)(int level, const char *file, const char *function, int line, char *log_msg);

/* umq_ub_impl.c — 转发逻辑（修改前） */
if (config->log_flag & UMQ_LOG_FLAG_EXT_FUNC) {
    return umq_symbol_urma()->urma_register_loc_log_func(config->ext_func);   // 符号已删除
}
if (config->log_flag & UMQ_LOG_FLAG_FUNC) {
    return umq_symbol_urma()->urma_register_log_func(config->func);
}
```

```c
/* umq_types.h — 修改后：umq_log_func_t 与 umq_log_ext_func_t 统一为同一变参签名 */
typedef void (*umq_log_func_t)(int level, unsigned int flags, const char *log_msg, ...);
typedef void (*umq_log_ext_func_t)(int level, unsigned int flags, const char *log_msg, ...);

/* umq_ub_impl.c — 转发逻辑（修改后）：EXT_FUNC 与 FUNC 均走统一注册接口 */
if (config->log_flag & UMQ_LOG_FLAG_EXT_FUNC) {
    return umq_symbol_urma()->urma_register_log_func(config->ext_func);
}
if (config->log_flag & UMQ_LOG_FLAG_FUNC) {
    return umq_symbol_urma()->urma_register_log_func(config->func);
}
```

> **`umq_log_ext_func_t` 与 `umq_log_func_t` 的关系**：合并后二者签名相同，仅靠 `UMQ_LOG_FLAG_EXT_FUNC` / `UMQ_LOG_FLAG_FUNC` 区分语义。保留两个 typedef 是为避免破坏 `umq_log_config_t` 的 ABI 字段命名，但二者类型等价、可互换。若 `umq` 侧也愿意进一步收敛为单一 typedef，可由 `umq` 维护者另作决策，不强制。
>
> **ext_func 回调如何取位置信息**：经 `urma_register_log_func` 注册的 ext_func 回调，liburma 在位置路径下仍按 [§2.1](#21-回调类型定义) 约定以三位 flags 全置、按序压 file/function/line。故上层 ext_func 实现须按"一位一参、按序 `va_arg`（FILE→FUNC→LINE）"解析，规则与 [§4.2](#42-场景二选择性解析位置信息) 一致。

`umq_symbol_private.h` 中通过 `dlopen` 加载的符号表须同步删除 `urma_register_loc_log_func` 条目：

```c
/* umq_symbol_private.h — 删除 */
typedef urma_status_t (*urma_register_loc_log_func_t)(urma_loc_log_cb func);   // 类型已不存在
...
urma_register_loc_log_func_t urma_register_loc_log_func;                       // 结构体字段删除

/* umq_symbol_private.c — 删除对应的 LOAD_SYMBOL 行 */
// LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_register_loc_log_func_t, urma_register_loc_log_func);
```

#### 4.6.3 下游库迁移检查清单

- [ ] `urpc_log_func_t` / `umq_log_func_t` / `umq_log_ext_func_t` 签名收敛为新变参形式
- [ ] `urma_register_log_func` 调用语句保持不变（参数类型虽变，但传递的函数指针类型已同步更新）
- [ ] 删除 `umq` 对 `urma_register_loc_log_func` 的调用与动态加载
- [ ] 下游库自身的回调实现按 [§4.3](#43-变参解析规则重要约束) 约束，逐位检查 `flags & URMA_LOG_FLAG_FILE/FUNC/LINE`、按序 `va_arg` 再解析变参
- [ ] 验证 `util_vlog.h` 中 `vlog_output_ext_func`（同款 5 参数签名）是否需一并收敛，由 `urpc` 维护者确认

---

## 5. 向后兼容性分析

### 5.1 ABI 兼容性

| 维度 | 分析 |
|------|------|
| 回调签名变化 | **不兼容**。函数指针的入参类型和数量均发生变化，旧二进制无法直接使用新库。需要重新编译回调代码。 |
| 符号可见性 | `urma_log_cb_t` 符号名保留但定义变化；`urma_loc_log_cb` 被删除。删除影响**并非为零**：`umq` 通过 `dlopen` 加载 `urma_register_loc_log_func` 符号（见 [§4.6.2](#462-umq-路径带位置信息原走-urma_register_loc_log_func)），删除后该符号在 liburma 中不再导出，`umq` 必须改走 `urma_register_log_func`，否则 `dlopen` 加载失败。 |
| 调用方/回调方 ABI 一致性 | liburma 通过变参 `...` 将 `file`/`function`/`line` 压栈后调用 `g_urma_log_func`，回调实现可能来自独立编译单元（perftest/ping）或经 `umq` 转发的上层回调。**调用方与回调方必须 ABI 兼容**：在 x86-64/ARM64 等主流平台，`const char *` 与 `int` 走标准变参布局，默认一致；但跨平台或异常调用约定下需显式约束，文档须明确"回调方与 liburma 必须使用同一平台 ABI 编译"。 |

### 5.2 API 源码兼容性

- `urma_register_log_func(func)` — 调用语句不变，仅需调整回调定义（参数类型虽变，但传递的函数指针类型已同步更新）。
- `urma_register_loc_log_func(func)` — 已被删除。存在实际使用者：`umq` 经 `dlopen` 调用该符号（[§4.6.2](#462-umq-路径带位置信息原走-urma_register_loc_log_func)），须将 `umq` 路径改走 `urma_register_log_func`，同步收敛 `umq_log_ext_func_t` 签名。

### 5.3 迁移建议

建议在主版本号变更时（或其他 API 破坏时机）一并发布此修改，或单独发布兼容性公告。

---

## 6. 涉及文件清单

| 文件 | 操作类型 | 变更概要 |
|------|---------|---------|
| `src/urma/lib/urma/core/include/urma_types.h` | 修改 | 删除 `urma_log_cb_t` 旧定义、删除 `urma_loc_log_cb`；定义 `URMA_LOG_FLAG_FILE`/`FUNC`/`LINE`（一位一参）；重定义 `urma_log_cb_t` 为变参回调 |
| `src/urma/lib/urma/core/include/urma_api.h` | 修改 | 删除 `urma_register_loc_log_func` 声明；更新 `urma_register_log_func` 注释 |
| `src/urma/lib/urma/core/urma_log.c` | 修改 | 删除 `g_urma_loc_log_func`、`g_use_loc_log`；更新默认回调、注册/注销函数、`urma_vlog`、`urma_vlog_loc` 调用点 |
| `src/urma/tools/urma_perftest/perftest_resources.c` | 修改 | `print_log` 回调签名变更为新变参形式；添加 `(void)flags` |
| `src/urma/tools/urma_ping/ping_run.c` | 修改 | `urma_log_func` 回调签名变更为新变参形式；添加 `(void)flags` |
| `src/urpc/include/framework/urpc_framework_types.h` | 修改 | `urpc_log_func_t` 收敛为新变参签名；`urpc_log_config_t.func` 类型随之更新 |
| `src/urpc/framework/urpc_lib_log.c` | 修改 | `urma_register_log_func(config->func)` 调用语句不变（类型已同步），确认无残留旧签名依赖 |
| `src/urpc/include/umq/umq_types.h` | 修改 | `umq_log_func_t` / `umq_log_ext_func_t` 收敛为新变参签名；`umq_log_config_t.func` / `ext_func` 类型随之更新 |
| `src/urpc/umq/umq_ub/core/umq_ub_impl.c` | 修改 | `umq_ub_log_config_set_impl` 中 EXT_FUNC 分支改走 `urma_register_log_func`（见 [§3.5.4](#354-转发调用点合并)、[§4.6.2](#462-umq-路径带位置信息原走-urma_register_loc_log_func)） |
| `src/urpc/umq/umq_ub/core/private/umq_symbol_private.h` | 修改 | 删除 `urma_register_loc_log_func_t` typedef 与结构体字段（见 [§3.5.1](#351-符号表-typedef-合并)、[§3.5.2](#352-符号表结构体字段合并)） |
| `src/urpc/umq/umq_ub/core/private/umq_symbol_private.c` | 修改 | 删除 `urma_register_loc_log_func` 的 `LOAD_SYMBOL` 行（见 [§3.5.3](#353-dlopen-加载逻辑合并)） |
| `src/urpc/util/util_vlog.h` | 待确认 | `vlog_output_ext_func`（同款 5 参数签名）是否需一并收敛，由 `urpc` 维护者确认 |
| `test/urma/ut/core/options_log_perf_test.cpp` | 修改 | 删除 `urma_register_loc_log_func` 调用；`MockLogCallback` / `MockLocLogCallback` 签名收敛为新变参形式 |
| `test/urma/fuzz/urma_api_fuzz.c` | 修改 | 删除 `Fuzz_urma_register_loc_log_func` 及其符号表条目；`Fuzz_urma_register_log_func` 内回调签名收敛 |
| `doc/design/urma_log_callback_merge_design.md` | 新增 | 本文档 |

---

## 7. 未来展望

### 7.1 可扩展的 flags 机制

使用 `unsigned int` 作为 flags 位图，预留了 31 个标志位。当前已占用 bit 0–2（`URMA_LOG_FLAG_FILE`/`FUNC`/`LINE`，一位一参），未来可低成本扩展更多可选信息：

```c
#define URMA_LOG_FLAG_FILE      (1u << 0)   /* 当前定义 */
#define URMA_LOG_FLAG_FUNC      (1u << 1)   /* 当前定义 */
#define URMA_LOG_FLAG_LINE      (1u << 2)   /* 当前定义 */
#define URMA_LOG_FLAG_THREAD    (1u << 3)   /* 预留：传入 thread_id */
#define URMA_LOG_FLAG_TIMESTAMP (1u << 4)   /* 预留：传入时间戳 */
```

新增标识时，只需在库内部调用点增加相应变参，回调通过检查对应 flag 位决定是否使用——完全不影响现有回调。扩展时须保持"一位一参"与"按位号升序压栈/`va_arg`"的约定。

### 7.2 与 printf-like format 注解的关系

新回调类型无法标注 `__attribute__((format(printf, ...)))`，因为消息字符串在传入回调前已被库内部格式化完成，回调收到的 `message` 已经是完整字符串。因此不需要 format 注解。
