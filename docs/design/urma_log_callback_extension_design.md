# URMA日志回调接口扩展设计文档

## 1. 需求背景

应用希望urma提供一套注册日志函数的新接口，在原有`urma_log_cb_t`基础上新增文件名称和代码行数参数，以便应用能够更精确地定位日志来源。

## 2. 现有接口分析

### 2.1 当前类型定义

**文件**: `src/urma/lib/urma/core/include/urma_types.h:1416`

```c
typedef void (*urma_log_cb_t)(int level, char *message);
```

### 2.2 当前注册接口

**文件**: `src/urma/lib/urma/core/include/urma_api.h:988`

```c
urma_status_t urma_register_log_func(urma_log_cb_t func);
urma_status_t urma_unregister_log_func(void);
```

### 2.3 现有调用者

| 文件 | 回调函数 | 说明 |
|------|----------|------|
| `tools/urma_perftest/perftest_resources.c:46` | `print_log` | 打印到stdout |
| `tools/urma_ping/ping_run.c:245` | `urma_log_func` | 使用LOG_VVERBOSE宏 |

### 2.4 内部日志格式

urma内部日志已包含文件名和行号信息（通过`urma_vlog`函数拼接在message中）：
```
[URMA][liburma][thread_id=tid][thread_tag][function[Line=line]][format]
```

## 3. 设计方案

### 3.1 核心原则

**影响最小化原则**：
1. 保持原有`urma_log_cb_t`类型和`urma_register_log_func`接口不变，确保向后兼容
2. 新增扩展类型和注册接口，应用可按需选择使用
3. 内部实现共享，避免代码重复

### 3.2 新增类型定义

**文件**: `src/urma/lib/urma/core/include/urma_types.h`

```c
// 原有类型保持不变（向后兼容）
typedef void (*urma_log_cb_t)(int level, char *message);

// 新增扩展类型，包含文件名、函数名和行号
typedef void (*urma_log_cb_ex_t)(int level, const char *file, const char *function, int line, char *message);
```

### 3.3 新增API接口

**文件**: `src/urma/lib/urma/core/include/urma_api.h`

```c
/**
 * User register extended log function with file and line info.
 * @param[in] func: extended log callback function;
 * Return: 0 on success, other value on error
 * Note: If both urma_register_log_func and urma_register_log_func_ex are called,
 *       the last registered function will be used.
 */
urma_status_t urma_register_log_func_ex(urma_log_cb_ex_t func);
```

### 3.4 内部实现方案

**文件**: `src/urma/lib/urma/core/urma_log.c`

```c
// 新增全局变量
static urma_log_cb_ex_t g_urma_log_func_ex = NULL;
static bool g_use_extended_log = false;

// 修改urma_vlog函数，根据注册类型选择调用方式
static int urma_vlog(const char *function, int line, urma_vlog_level_t level,
                     const char *format, va_list va)
{
    // ... 格式化message ...

    if (g_use_extended_log && g_urma_log_func_ex != NULL) {
        // 调用扩展回调，传入文件名、函数名和行号
        (*g_urma_log_func_ex)((int)level, file, function, line, logmsg);
    } else {
        (*g_urma_log_func)((int)level, logmsg);
    }
    return ret;
}

// 新增注册函数实现
urma_status_t urma_register_log_func_ex(urma_log_cb_ex_t func)
{
    if (func == NULL) {
        URMA_LOG_ERR("Invalid parameter.\n");
        return URMA_EINVAL;
    }
    g_urma_log_func_ex = func;
    g_use_extended_log = true;
    URMA_LOG_INFO("registered extended log successfully.\n");
    return URMA_SUCCESS;
}

// 修改urma_unregister_log_func，同时清理扩展注册
urma_status_t urma_unregister_log_func(void)
{
    // ... 原有逻辑 ...
    g_urma_log_func = urma_default_log_func;
    g_urma_log_func_ex = NULL;
    g_use_extended_log = false;
    return URMA_SUCCESS;
}
```

### 3.5 内部实现优化（统一日志宏）

**设计目标**：内部记录日志统一使用一个宏，根据应用注册行为自动选择不同的回调函数。

**当前日志宏**（`urma_log.h`）：
```c
#define URMA_LOG(l, ...)                                                                                               \
    if (!urma_log_drop(URMA_VLOG_LEVEL_##l)) {                                                                         \
        urma_log(__func__, __LINE__, URMA_VLOG_LEVEL_##l, __VA_ARGS__);                                                \
    }
```

**优化方案**：修改`URMA_LOG`宏，使其调用`urma_log_ex`函数，自动携带`__FILE__`和`__func__`：

```c
// urma_log.h 修改宏定义
#define URMA_LOG(l, ...)                                                                                               \
    if (!urma_log_drop(URMA_VLOG_LEVEL_##l)) {                                                                         \
        urma_log_ex(__FILE__, __func__, __LINE__, URMA_VLOG_LEVEL_##l, __VA_ARGS__);                                   \
    }

// urma_log.c 新增函数实现
void urma_log_ex(const char *file, const char *function, int line, urma_vlog_level_t level, const char *format, ...)
{
    va_list va;
    va_start(va, format);
    (void)urma_vlog_ex(file, function, line, level, format, va);
    va_end(va);
}

// urma_vlog_ex 内部根据注册类型分发
static int urma_vlog_ex(const char *file, const char *function, int line, urma_vlog_level_t level,
                        const char *format, va_list va)
{
    // ... 格式化message ...

    if (g_use_extended_log && g_urma_log_func_ex != NULL) {
        // 扩展回调：传入文件名、函数名和行号
        (*g_urma_log_func_ex)((int)level, file, function, line, logmsg);
    } else {
        // 原有回调
        (*g_urma_log_func)((int)level, logmsg);
    }
    return ret;
}
```

**说明**：
- `URMA_LOG`宏修改为调用`urma_log_ex`，自动携带`__FILE__`和`__func__`
- `URMA_LOG_INFO`、`URMA_LOG_ERR`等宏无需修改，继续使用`URMA_LOG`
- 原有`urma_log`函数保留，供外部组件直接调用（如有需要）
- 分发逻辑封装在`urma_vlog_ex`内部，对上层透明
- 应用注册扩展接口后，自动切换到扩展回调路径

## 4. 影响范围分析

### 4.1 无需修改的部分

| 组件 | 原因 |
|------|------|
| 现有应用代码 | 原接口保持不变，向后兼容 |
| `urma_log_cb_t`类型定义 | 保留，供旧应用使用 |
| `urma_register_log_func`接口 | 保留，功能不变 |
| 现有工具（perftest, ping） | 无需修改，继续使用原接口 |

### 4.2 需要修改的部分

| 文件 | 修改内容 | 影响程度 |
|------|----------|----------|
| `urma_types.h` | 新增`urma_log_cb_ex_t`类型定义（含file和function参数） | 低（仅新增） |
| `urma_api.h` | 新增`urma_register_log_func_ex`接口声明 | 低（仅新增） |
| `urma_log.h` | 新增`urma_log_ex`函数声明，修改`URMA_LOG`宏使用`__FILE__` | 低（仅新增和修改宏） |
| `urma_log.c` | 新增扩展注册函数和日志函数实现，修改日志分发逻辑 | 中（核心逻辑） |

### 4.3 ABI兼容性

- **原有接口ABI不变**：`urma_log_cb_t`和`urma_register_log_func`签名不变
- **新增接口不影响现有符号**：新符号独立导出
- **建议版本号更新**：minor版本+1（新增功能，向后兼容）

## 5. 使用示例

### 5.1 应用使用新接口示例

```c
#include <urma_api.h>

// 扩展日志回调函数
static void my_log_callback_ex(int level, const char *file, const char *function, int line, char *message)
{
    printf("[%s][%s:%s:%d] %s\n",
           level_to_string(level), file, function, line, message);
}

int main(void)
{
    // 使用扩展接口注册
    urma_status_t status = urma_register_log_func_ex(my_log_callback_ex);
    if (status != URMA_SUCCESS) {
        fprintf(stderr, "Failed to register log func\n");
        return -1;
    }

    // 使用日志宏（自动携带__FILE__和__func__）
    URMA_LOG_INFO("This log includes file and function info.\n");

    // ... 应用逻辑 ...

    urma_unregister_log_func();
    return 0;
}
```

### 5.2 应用继续使用原接口示例

```c
// 原有代码无需任何修改，继续正常工作
static void my_log_callback(int level, char *message)
{
    printf("[%s] %s\n", level_to_string(level), message);
}

urma_register_log_func(my_log_callback);  // 仍然有效
```

## 6. 实现步骤

1. **Phase 1**: 类型定义扩展
   - 在`urma_types.h`中新增`urma_log_cb_ex_t`类型

2. **Phase 2**: API接口扩展
   - 在`urma_api.h`中新增`urma_register_log_func_ex`接口声明

3. **Phase 3**: 内部实现
   - 在`urma_log.c`中新增全局变量和注册函数实现
   - 修改日志分发逻辑，支持两种回调类型

4. **Phase 4**: 测试验证
   - 单元测试：验证新旧接口功能
   - 兼容性测试：确保现有应用不受影响

## 7. 风险评估

| 风险项 | 等级 | 缓解措施 |
|--------|------|----------|
| ABI兼容性破坏 | 低 | 原接口完全保留，仅新增 |
| 性能影响 | 低 | 仅增加一个bool判断 |
| 线程安全 | 低 | 使用现有锁机制保护 |

## 8. 总结

本方案通过新增扩展类型和接口的方式满足应用需求，同时完全保持向后兼容：

- **影响最小**：仅新增代码，不修改现有接口和类型
- **向后兼容**：现有应用无需任何修改
- **实现简洁**：复用现有日志格式化逻辑，仅增加分发判断
- **扩展性好**：为未来可能的进一步扩展预留空间
