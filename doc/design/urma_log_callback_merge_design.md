# URMA 日志回调合并设计文档

## 1. 概述

### 1.1 背景

当前 URMA 日志子系统定义了两套日志回调类型及其对应的注册 API：

| 回调类型 | 签名 | 注册 API |
|---------|------|---------|
| `urma_log_cb_t` | `void (int level, char *message)` | `urma_register_log_func()` |
| `urma_loc_log_cb` | `void (int level, const char *file, const char *function, int line, char *message)` | `urma_register_loc_log_func()` |

两套回调语义重叠、互斥注册（后注册的覆盖前者），且 `urma_loc_log_cb` 虽已定义并导出，但截至目前没有任何实际使用者调用 `urma_register_loc_log_func()`。这种重复设计增加了库的维护成本和用户的理解负担。

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
/* Log callback flags — 指示变参中携带的可选信息 */
#define URMA_LOG_FLAG_LOC    (1u << 0)   /* variadic args: const char *file, const char *function, int line */

/**
 * Unified log callback type.
 *
 * @param level     Log level (urma_vlog_level_t)
 * @param flags     Bitmask of URMA_LOG_FLAG_* indicating which variadic args are present
 * @param message   Pre-formatted log message string
 * @param ...       Optional variadic arguments as indicated by @c flags
 *
 * Variadic argument layout when @c flags has URMA_LOG_FLAG_LOC set:
 *   va_arg(ap, const char *) — file name
 *   va_arg(ap, const char *) — function name
 *   va_arg(ap, int)          — line number
 *
 * The variadic arguments appear in a defined order and are always present as a
 * complete set when the corresponding bit is set. Partial presence is undefined.
 *
 * Example — minimal handler (ignore location):
 *   void my_log(int level, unsigned int flags, const char *message, ...) {
 *       (void)flags;
 *       fprintf(stderr, "%s\\n", message);
 *   }
 *
 * Example — with location parsing:
 *   void my_log(int level, unsigned int flags, const char *message, ...) {
 *       if (flags & URMA_LOG_FLAG_LOC) {
 *           va_list ap;
 *           va_start(ap, message);
 *           const char *f = va_arg(ap, const char *);
 *           const char *fn = va_arg(ap, const char *);
 *           int ln = va_arg(ap, int);
 *           va_end(ap);
 *           fprintf(stderr, "[%s:%s:%d] %s", f, fn, ln, message);
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
 *   When invoked, the @c flags parameter indicates whether location
 *   information (file/function/line) is available via variadic args.
 *   See URMA_LOG_FLAG_LOC.
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
| flags 位定义 | 无 | `URMA_LOG_FLAG_LOC` |
| 位置参数传递方式 | 固定参数 | 变参（flags 指示） |

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

// 修改后 — 统一调用变参回调，flags 携带 URMA_LOG_FLAG_LOC
static int urma_vlog_loc(..., va_list va)
{
    // ... 格式化 logmsg ...
    (*g_urma_log_func)((int)level, URMA_LOG_FLAG_LOC, logmsg, file, function, line);
    return ret;
}
```

> **关键变化**：删除了运行时的二分支路由。无论用户注册何种回调，`urma_vlog_loc` 始终统一调用 `g_urma_log_func`并携带位置变参。由回调自身根据 `flags` 决定是否解析。

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
(*g_urma_log_func)(level, URMA_LOG_FLAG_LOC, logmsg, file, function, line)
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

> **改动量**: 每处回调定义仅需：
> 1. 增加 `unsigned int flags` 参数
> 2. 变参占位（`...`）
> 3. `message` 类型由 `char *` 改为 `const char *`
> 4. 添加 `(void)flags;`

### 4.2 场景二：选择性解析位置信息

如果应用在某些场景下需要日志位置，但在其他场景下忽略：

```c
static void my_log_cb(int level, unsigned int flags, const char *message, ...)
{
    if (flags & URMA_LOG_FLAG_LOC) {
        /* 变参中存在 file/function/line。
         * 注意：URMA_LOG_FLAG_LOC 保证三个参数全部存在，
         * 不会出现部分缺失的情况。 */
        va_list ap;
        va_start(ap, message);
        const char *file     = va_arg(ap, const char *);
        const char *function = va_arg(ap, const char *);
        int        line      = va_arg(ap, int);
        va_end(ap);

        fprintf(stderr, "[%s:%s:%d] %s", file, function, line, message);
    } else {
        /* 变参不存在（无 URMA_LOG_FLAG_LOC），
         * 不得尝试 va_arg 读取！ */
        fprintf(stderr, "%s", message);
    }
}
```

### 4.3 变参解析规则（重要约束）

| 规则 | 说明 |
|------|------|
| **完整性保证** | 当 `URMA_LOG_FLAG_LOC` 置位时，三个变参（file/function/line）全部存在且按固定顺序出现。无部分置位情况。 |
| **类型匹配** | file/function 为 `const char *`，line 为 `int`。必须严格按顺序用 `va_arg` 取出。 |
| **非置位时禁止解析** | 当 `flags & URMA_LOG_FLAG_LOC == 0` 时，变参区域**不存在**任何额外参数，不得调用 `va_start`/`va_arg`。 |
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

/* ✓ 正确：先检查 flags 再解析 */
void good_cb(int level, unsigned int flags, const char *msg, ...)
{
    if (flags & URMA_LOG_FLAG_LOC) {
        va_list ap;
        va_start(ap, msg);
        const char *f = va_arg(ap, const char *);
        const char *fn = va_arg(ap, const char *);
        int ln = va_arg(ap, int);
        va_end(ap);
        // 使用 file/function/line
    } else {
        // 仅使用 message
    }
}
```

### 4.5 代码迁移检查清单

对现有应用的每处回调定义：

- [ ] 参数列表从 `(int level, char *message)` → `(int level, unsigned int flags, const char *message, ...)`
- [ ] 添加 `(void)flags;` 或添加 `flags & URMA_LOG_FLAG_LOC` 检查分支
- [ ] 如果原回调没有使用 `message` 之外的内容，不需要引入变参解析代码
- [ ] `urma_register_log_func(my_cb)` 调用语句**无需修改**
- [ ] 删除 `urma_register_loc_log_func(...)` 调用（如果有）

---

## 5. 向后兼容性分析

### 5.1 ABI 兼容性

| 维度 | 分析 |
|------|------|
| 回调签名变化 | **不兼容**。函数指针的入参类型和数量均发生变化，旧二进制无法直接使用新库。需要重新编译回调代码。 |
| 符号可见性 | `urma_log_cb_t` 符号名保留但定义变化；`urma_loc_log_cb` 被删除（无实际使用方，影响为零）。 |

### 5.2 API 源码兼容性

- `urma_register_log_func(func)` — 调用语句不变，仅需调整回调定义。
- `urma_register_loc_log_func(func)` — 已被删除。当前实际使用者检索结果为 0，无影响。

### 5.3 迁移建议

建议在主版本号变更时（或其他 API 破坏时机）一并发布此修改，或单独发布兼容性公告。

---

## 6. 涉及文件清单

| 文件 | 操作类型 | 变更概要 |
|------|---------|---------|
| `src/urma/lib/urma/core/include/urma_types.h` | 修改 | 删除 `urma_log_cb_t` 旧定义、删除 `urma_loc_log_cb`；定义 `URMA_LOG_FLAG_LOC`；重定义 `urma_log_cb_t` 为变参回调 |
| `src/urma/lib/urma/core/include/urma_api.h` | 修改 | 删除 `urma_register_loc_log_func` 声明；更新 `urma_register_log_func` 注释 |
| `src/urma/lib/urma/core/urma_log.c` | 修改 | 删除 `g_urma_loc_log_func`、`g_use_loc_log`；更新默认回调、注册/注销函数、`urma_vlog`、`urma_vlog_loc` 调用点 |
| `src/urma/tools/urma_perftest/perftest_resources.c` | 修改 | `print_log` 回调签名变更为新变参形式；添加 `(void)flags` |
| `src/urma/tools/urma_ping/ping_run.c` | 修改 | `urma_log_func` 回调签名变更为新变参形式；添加 `(void)flags` |
| `doc/design/urma_log_callback_merge_design.md` | 新增 | 本文档 |

---

## 7. 未来展望

### 7.1 可扩展的 flags 机制

使用 `unsigned int` 作为 flags 位图，预留了 31 个标志位（`URMA_LOG_FLAG_LOC` 占 bit 0），未来可以低成本扩展更多可选信息：

```c
#define URMA_LOG_FLAG_LOC     (1u << 0)   /* 当前定义 */
#define URMA_LOG_FLAG_THREAD  (1u << 1)   /* 预留：传入 thread_id */
#define URMA_LOG_FLAG_TIMESTAMP (1u << 2) /* 预留：传入时间戳 */
```

新增标识时，只需在库内部调用点增加相应变参，回调通过检查 flags 决定是否使用——完全不影响现有回调。

### 7.2 与 printf-like format 注解的关系

新回调类型无法标注 `__attribute__((format(printf, ...)))`，因为消息字符串在传入回调前已被库内部格式化完成，回调收到的 `message` 已经是完整字符串。因此不需要 format 注解。
