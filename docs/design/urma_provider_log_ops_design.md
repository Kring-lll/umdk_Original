# URMA Provider 日志注册函数设计文档

## 1. 背景与目标

### 1.1 现状分析

当前 URMA 的日志回调注册通过两个全局 API 实现：

- `urma_register_log_func(urma_log_cb_t func)` — 注册简单日志回调
- `urma_register_loc_log_func(urma_loc_log_cb func)` — 注册带位置的日志回调

这些函数操作的是 **进程级全局静态变量**（`g_urma_log_func`、`g_urma_loc_log_func`、`g_use_loc_log`），位于 [urma_log.c](src/urma/lib/urma/core/urma_log.c#L49-L51)。

**问题**：当应用调用上述 API 注册日志函数时，URMA 仅将回调保存在核心库的全局变量中，**没有通知底层驱动（provider）**。如果驱动也需要使用这些日志回调（例如驱动内部需要向应用上报日志），则缺少将日志回调传递给驱动的通道。

### 1.2 目标

在 `urma_provider_ops_t` 中增加两个 provider 函数指针，使 URMA 在应用调用 `urma_register_log_func` 和 `urma_register_loc_log_func` 时，能够**调用驱动的 provider 函数，将应用的日志回调注册到驱动**。同时确保：

1. **跨版本二进制兼容**：新版本 liburma 可以加载旧版本 provider `.so`，旧版本 liburma 也可以加载新版本 provider `.so`
2. **向前兼容**：已有的 provider（udma、bond）无需修改即可继续工作
3. **流程清晰**：应用程序 → URMA → 驱动，日志回调单向传递

## 2. 现有结构分析

### 2.1 `urma_provider_ops_t` 当前定义

定义在 [urma_provider.h](src/urma/lib/urma/core/include/urma_provider.h#L201-L212)：

```c
typedef struct urma_provider_ops {
    const char *name;
    urma_provider_attr_t attr;           // 包含 version 字段
    urma_match_entry_t *match_table;
    urma_status_t (*init)(urma_init_attr_t *conf);
    urma_status_t (*uninit)(void);
    /* Device OPs */
    urma_status_t (*query_device)(urma_device_t *dev, urma_device_attr_t *dev_attr);
    urma_context_t *(*create_context)(urma_device_t *dev, uint32_t eid_index, int dev_fd);
    urma_status_t (*delete_context)(urma_context_t *ctx);
    urma_status_t (*get_uasid)(uint32_t *uasid); /* obsolete */
} urma_provider_ops_t;
```

结构体大小为固定值（11 个字段），所有 provider 的 `.so` 编译时确定此大小。

### 2.2 现有日志回调类型

定义在 [urma_types.h](src/urma/lib/urma/core/include/urma_types.h#L1445-L1448)：

```c
typedef void (*urma_log_cb_t)(int level, char *message);
typedef void (*urma_loc_log_cb)(int level, const char *file, const char *function,
                                 int line, char *message);
```

### 2.3 现有 Provider 注册模式

Provider 通过 `__attribute__((constructor))` 在 `.so` 加载时自动调用 `urma_register_provider_ops()`，ops 结构体为 **静态 const 全局变量**，编译时完全确定所有字段。

### 2.4 现有的兼容性处理模式

代码库中已有两种兼容性处理模式：

**模式 A（空指针检查 fallback）**：[urma_cp_api.c](src/urma/lib/urma/core/urma_cp_api.c#L1230-L1233)

```c
static inline bool urma_check_ctrlplane_compat(void *op_ptr)
{
    return (op_ptr == NULL);
}
```

当新 `_ex` 版本的函数指针为 NULL 时，回退到旧版本实现。例如 `import_jfr` vs `import_jfr_ex`。

**模式 B（version 字段）**：[urma_provider.h](src/urma/lib/urma/core/include/urma_provider.h#L196-L198)

```c
typedef struct urma_provider_attr {
    uint32_t version; /* compatible with abi version of kernel driver */
    urma_transport_type_t transport_type;
} urma_provider_attr_t;
```

### 2.5 `urma_ops_t` 的历史演进

`urma_ops_t` 已有多次字段扩展的历史，例如：
- `import_jfr` → 后续增加 `import_jfr_ex`（新字段追加在结构体末尾）
- `import_jetty` → 后续增加 `import_jetty_ex`
- `bind_jetty` → 后续增加 `bind_jetty_ex`

这些 `_ex` 后缀的函数指针都追加在结构体**末尾**，通过 NULL 检查实现新旧兼容。

## 3. 设计方案

### 3.1 总体方案：结构体末尾追加 + 版本号判断

在 `urma_provider_ops_t` 末尾追加两个新的 provider 函数指针。当应用调用 `urma_register_log_func` / `urma_register_loc_log_func` 时，URMA 核心库除了保存到全局变量外，还调用这些 provider 函数，将日志回调**传递给驱动**。

#### 3.1.1 修改 `urma_provider_ops_t` 定义

```c
typedef struct urma_provider_ops {
    const char *name;
    urma_provider_attr_t attr;
    urma_match_entry_t *match_table;
    urma_status_t (*init)(urma_init_attr_t *conf);
    urma_status_t (*uninit)(void);
    /* Device OPs */
    urma_status_t (*query_device)(urma_device_t *dev, urma_device_attr_t *dev_attr);
    urma_context_t *(*create_context)(urma_device_t *dev, uint32_t eid_index, int dev_fd);
    urma_status_t (*delete_context)(urma_context_t *ctx);
    urma_status_t (*get_uasid)(uint32_t *uasid); /* obsolete */

    /* === 新增字段（v2） === */
    /**
     * Register a simple log callback to the provider (driver).
     * Called by liburma when the application invokes urma_register_log_func().
     * The provider should save the callback and use it for internal log output.
     * @param[in] func: log callback function (urma_log_cb_t)
     * @return URMA_SUCCESS on success, error code otherwise
     */
    urma_status_t (*register_log_func)(urma_log_cb_t func);

    /**
     * Register a location-aware log callback to the provider (driver).
     * Called by liburma when the application invokes urma_register_loc_log_func().
     * The provider should save the callback and use it for internal log output
     * with file/function/line information.
     * @param[in] func: location log callback function (urma_loc_log_cb)
     * @return URMA_SUCCESS on success, error code otherwise
     */
    urma_status_t (*register_loc_log_func)(urma_loc_log_cb func);
} urma_provider_ops_t;
```

#### 3.1.2 版本号约定

| `attr.version` | 含义 |
|---|---|
| 0 或 1 | 旧版 provider，`register_log_func` 和 `register_loc_log_func` 字段不存在（结构体为旧大小） |
| 2 | 新版 provider，包含 `register_log_func` 和 `register_loc_log_func` 字段 |

现有 provider 无需修改：
- UDMA: `version = 1`，无需修改
- Bond: `version = 0`，无需修改

#### 3.1.3 类型定义复用

新增字段的函数类型直接复用 [urma_types.h](src/urma/lib/urma/core/include/urma_types.h) 中已有的 `urma_log_cb_t` 和 `urma_loc_log_cb` 类型定义。

### 3.2 核心兼容性设计

#### 3.2.1 跨版本二进制兼容原理

**关键认知**：`urma_provider_ops_t` 是 provider `.so` 在编译时静态初始化的全局 const 结构体。其内存布局在编译时固定。

**场景 A：新版 liburma + 旧版 provider .so**

- 旧 provider 编译时结构体大小为 11 字段（不含 `register_log_func`、`register_loc_log_func`）
- 新版 liburma 期望结构体大小为 13 字段
- 解决方案：liburma 通过 `driver->ops->attr.version < 2` 判断为旧版 provider，**不访问** `register_log_func` 和 `register_loc_log_func` 字段

```c
// urma_log.c 中的访问逻辑
static inline bool urma_provider_has_log_ops(urma_provider_ops_t *ops)
{
    return (ops != NULL && ops->attr.version >= 2);
}

static inline urma_status_t urma_provider_register_log_func(urma_provider_ops_t *ops,
                                                             urma_log_cb_t func)
{
    if (!urma_provider_has_log_ops(ops) || ops->register_log_func == NULL)
        return URMA_SUCCESS;  // 旧 provider 或未实现，静默成功
    return ops->register_log_func(func);
}

static inline urma_status_t urma_provider_register_loc_log_func(urma_provider_ops_t *ops,
                                                                 urma_loc_log_cb func)
{
    if (!urma_provider_has_log_ops(ops) || ops->register_loc_log_func == NULL)
        return URMA_SUCCESS;  // 旧 provider 或未实现，静默成功
    return ops->register_loc_log_func(func);
}
```

**场景 B：旧版 liburma + 新版 provider .so**

- 新 provider 编译时结构体大小为 13 字段（包含 `register_log_func`、`register_loc_log_func`）
- 旧版 liburma 只知道 11 字段的结构体
- 旧 liburma 通过 `urma_register_provider_ops()` 接收 `urma_provider_ops_t*` 指针，只访问前 11 个字段
- `register_log_func`/`register_loc_log_func` 字段在 provider .so 的内存中，旧 liburma 不访问，**天然兼容**

**场景 C：两个版本一致**

- 旧 liburma + 旧 provider：version < 2，不访问新字段 ✓
- 新 liburma + 新 provider：version >= 2，正常访问新字段 ✓

#### 3.2.2 为什么不使用 sizeof 判断

`sizeof(urma_provider_ops_t)` 在编译时确定，liburma 编译时看到的 `sizeof` 是当前头文件定义的大小。在场景 A 中，新版 liburma 编译出的 `sizeof` 为 13 字段大小，但旧 provider 的 `.so` 中实际结构体只有 11 字段大小。直接用 `sizeof` 无法区分。

因此必须使用 **version 字段** 进行显式版本判断。

### 3.3 日志回调注册流程

#### 3.3.1 注册流程

应用程序调用 `urma_register_log_func()`（或 `urma_register_loc_log_func()`）时的完整流程：

```
应用程序
  |
  | urma_register_log_func(my_log_callback)
  v
URMA 核心库 (urma_log.c)
  |
  |-- 1. 保存回调到全局变量 (g_urma_log_func)
  |-- 2. 遍历所有已注册的 driver
  |       |
  |       |-- driver->ops->attr.version >= 2 ?
  |       |       YES --> driver->ops->register_log_func(my_log_callback)
  |       |       NO  --> 跳过（旧版 provider，不支持）
  |       v
  v      驱动保存回调，用于内部日志输出
```

#### 3.3.2 修改后的 urma_register_log_func()

```c
urma_status_t urma_register_log_func(urma_log_cb_t func)
{
    if (func == NULL) {
        URMA_LOG_ERR("Invalid parameter.\n");
        return URMA_EINVAL;
    }

    // 1. 保存到全局变量（现有逻辑）
    g_urma_log_func = func;
    g_urma_loc_log_func = NULL;
    g_use_loc_log = false;

    // 2. 遍历所有 driver，将回调注册到驱动（新增逻辑）
    struct urma_driver *driver;
    UB_LIST_FOR_EACH(driver, node, &g_driver_list) {
        if (urma_provider_has_log_ops(driver->ops) &&
            driver->ops->register_log_func != NULL) {
            driver->ops->register_log_func(func);
        }
    }

    URMA_LOG_INFO("registered log successfully.\n");
    return URMA_SUCCESS;
}
```

#### 3.3.3 修改后的 urma_register_loc_log_func()

```c
urma_status_t urma_register_loc_log_func(urma_loc_log_cb func)
{
    if (func == NULL) {
        URMA_LOG_ERR("Invalid parameter.\n");
        return URMA_EINVAL;
    }

    // 1. 保存到全局变量（现有逻辑）
    g_urma_loc_log_func = func;
    g_use_loc_log = true;

    // 2. 遍历所有 driver，将回调注册到驱动（新增逻辑）
    struct urma_driver *driver;
    UB_LIST_FOR_EACH(driver, node, &g_driver_list) {
        if (urma_provider_has_log_ops(driver->ops) &&
            driver->ops->register_loc_log_func != NULL) {
            driver->ops->register_loc_log_func(func);
        }
    }

    URMA_LOG_INFO("registered extended log successfully.\n");
    return URMA_SUCCESS;
}
```

### 3.4 Provider 日志回调注册时机（补充场景）

除了应用主动调用 `urma_register_log_func` 时 URMA 向驱动注册回调外，还需考虑以下场景：

1. **应用先调用 register，后加载 provider**：provider 在 `init()` 成功后，URMA 应检查全局变量是否已有应用注册的回调，若有则立即调用 provider 的 `register_log_func`/`register_loc_log_func` 传递给驱动

```c
// urma_main.c urma_init() 中，provider init 成功后
UB_LIST_FOR_EACH_SAFE (driver, next, node, &g_driver_list) {
    if (driver->ops->init == NULL || driver->ops->init(conf) != URMA_SUCCESS) {
        URMA_LOG_WARN("Provider init failed %s\n", driver->ops->name);
        ub_list_remove(&driver->node);
        free(driver);
    } else {
        // provider init 成功后，将已有的日志回调传递给驱动（新增）
        urma_log_sync_to_provider(driver->ops);
    }
}
```

其中 `urma_log_sync_to_provider()` 的实现：

```c
void urma_log_sync_to_provider(urma_provider_ops_t *ops)
{
    if (!urma_provider_has_log_ops(ops))
        return;

    if (g_use_loc_log && g_urma_loc_log_func != NULL &&
        ops->register_loc_log_func != NULL) {
        ops->register_loc_log_func(g_urma_loc_log_func);
    } else if (!g_use_loc_log && g_urma_log_func != urma_default_log_func &&
               ops->register_log_func != NULL) {
        ops->register_log_func(g_urma_log_func);
    }
}
```

2. **多 provider 场景**：日志回调应传递给**所有**已注册的 provider，而非仅最后一个

## 4. 修改清单

### 4.1 文件修改列表

| 文件 | 修改内容 |
|---|---|
| [urma_provider.h](src/urma/lib/urma/core/include/urma_provider.h) | 在 `urma_provider_ops_t` 末尾追加 `register_log_func` 和 `register_loc_log_func` 两个字段；增加版本号宏 `URMA_PROVIDER_OPS_VERSION_2` |
| [urma_log.c](src/urma/lib/urma/core/urma_log.c) | 修改 `urma_register_log_func()` 和 `urma_register_loc_log_func()`，增加向驱动传递回调的逻辑；新增 `urma_log_sync_to_provider()` 函数 |
| [urma_log.h](src/urma/lib/urma/core/urma_log.h) | 声明 `urma_log_sync_to_provider()` |
| [urma_main.c](src/urma/lib/urma/core/urma_main.c) | 在 `urma_init()` 中 driver init 成功后调用 `urma_log_sync_to_provider()` |
| [urma_types.h](src/urma/lib/urma/core/include/urma_types.h) | 无需修改（`urma_log_cb_t` 和 `urma_loc_log_cb` 已存在） |
| [urma_api.h](src/urma/lib/urma/core/include/urma_api.h) | 无需修改（全局 API 不变） |

### 4.2 各 Provider 修改

| Provider | 修改内容 |
|---|---|
| UDMA ([udma_u_ops.c](src/urma/hw/udma/udma_u_ops.c)) | 可选：如需接收应用的日志回调，设置 `version = 2` 并实现 `register_log_func`、`register_loc_log_func` |
| Bond ([bondp_main.c](src/urma/lib/urma/bond/bondp_main.c)) | 可选：如需接收应用的日志回调，设置 `version = 2` 并实现 `register_log_func`、`register_loc_log_func` |

**无需修改即可兼容**：两个 provider 保持 `version = 1`（UDMA）和 `version = 0`（Bond），新版 liburma 检测到 `version < 2` 后不访问新字段。

### 4.3 不修改的文件

- 所有公共 API 头文件（`urma_api.h`）— 全局日志 API 不变
- `urma_types.h` — 类型定义已存在
- 所有 provider 内部实现 — 除非需要主动使用新功能

## 5. 兼容性验证矩阵

| liburma 版本 | Provider 版本 | version 值 | register_log_func 访问 | 结果 |
|---|---|---|---|---|
| 旧 (v1) | 旧 (v1) | 0/1 | 不访问 | ✅ 原有行为 |
| 旧 (v1) | 新 (v2) | 2 | 不访问（旧 liburma 不知此字段） | ✅ provider 新字段被忽略 |
| 新 (v2) | 旧 (v1) | 0/1 | version < 2，不访问 | ✅ 应用回调仅保存在全局变量 |
| 新 (v2) | 新 (v2) | 2 | 正常访问 | ✅ 完整功能：应用回调同时传给驱动 |

## 6. 与现有模式的一致性

本设计沿用了代码库中已验证的模式：

1. **结构体末尾追加**：与 `urma_ops_t` 中 `import_jfr_ex`、`import_jetty_ex`、`bind_jetty_ex` 等字段的追加方式一致
2. **NULL 检查 fallback**：与 `urma_check_ctrlplane_compat()` 的 `op_ptr == NULL` 判断逻辑一致
3. **version 字段**：直接使用现有的 `urma_provider_attr_t.version` 字段，不引入新的版本机制
4. **静态初始化**：与 `g_udma_provider_ops`、`g_bondp_provider_ops` 的初始化方式一致

## 7. 测试建议

1. **旧 provider 兼容性测试**：使用现有 UDMA/Bond provider 的 `.so`，验证新版 liburma 可正常加载，应用注册日志回调不报错
2. **新 provider 功能测试**：构造 `version=2` 的 provider，实现 `register_log_func`/`register_loc_log_func`，验证应用注册日志回调时驱动正确收到回调
3. **先注册后加载测试**：应用先调用 `urma_register_log_func`，后加载 provider，验证 provider init 后收到回调
4. **先加载后注册测试**：provider 先加载 init 成功，应用后调用 `urma_register_log_func`，验证驱动实时收到回调
5. **多 provider 测试**：验证多个 provider 共存时，所有 provider 都能收到应用的日志回调
6. **A/B 升级测试**：模拟 liburma 先升级、provider 后升级的滚动升级场景

## 8. 风险评估

| 风险 | 等级 | 缓解措施 |
|---|---|---|
| 旧 liburma + 新 provider 中 version=2 导致误判 | 低 | 旧 liburma 不检查 version 值，仅用作 ABI 版本号；新 provider 设置 version=2 仅影响新版 liburma 行为 |
| `urma_provider_attr_t` 中 version 语义变化 | 低 | version 原本仅用于内核驱动 ABI 兼容性标记（注释：`compatible with abi version of kernel driver`），现扩展为用户态 provider ops 版本标记，语义兼容 |
| 多 provider 场景下某个 provider 的 register_log_func 返回错误 | 低 | URMA 核心库忽略单个 provider 的返回值，继续向其他 provider 注册；日志注册是 best-effort 行为 |
| 驱动未实现 register_log_func 但 version >= 2 | 低 | 通过 NULL 检查跳过，静默成功 |
