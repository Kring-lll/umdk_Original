# URMA日志流控功能设计方案

## 1. 需求背景

在URMA日志模块中，某些错误场景（如网络异常、设备故障等）可能产生大量重复日志，导致：
- 日志系统负载过高
- 磁盘空间快速消耗
- 关键日志信息被淹没

需要增加主动流控功能，对高频日志进行限制，同时保证关键信息不丢失。

## 2. 功能概述

### 2.1 核心需求
- 提供带流控的日志宏：`URMA_LOG_ERR_RL`、`URMA_LOG_WARN_RL`、`URMA_LOG_INFO_RL`、`URMA_LOG_DEBUG_RL`
- 流控策略：每5秒内每个日志点（函数名+行号）最多记录100条日志
- 超过限制后，记录流控摘要信息，包含被流控的日志数量、函数名、行号等

### 2.2 流控行为
```
时间窗口: 5秒
每窗口上限: 100条/日志点
流控触发时: 记录摘要日志，说明被流控的日志数量
窗口重置: 5秒后自动重置计数器
```

## 3. 详细设计

### 3.1 数据结构设计

#### 3.1.1 流控状态结构（借鉴Linux内核设计）

采用**自旋锁 + 原子变量**的混合设计，每个日志点静态定义独立的流控状态：

```c
/* urma_log.h */

#include <pthread.h>
#include <stdatomic.h>

#define URMA_LOG_RL_WINDOW_SEC      5       /* 流控时间窗口(秒) */
#define URMA_LOG_RL_LIMIT           100     /* 每窗口日志上限 */

/* 流控状态标志 */
#define URMA_LOG_RL_INITIALIZED     0x01

/* 流控状态结构 - 每个日志点独立 */
typedef struct urma_log_rl_state {
    pthread_spinlock_t lock;         /* 自旋锁，保护窗口重置 */
    atomic_uint        n_left;       /* 窗口内剩余配额(原子变量) */
    atomic_uint        missed;       /* 被流控数量(原子变量) */
    time_t             begin;        /* 窗口起始时间 */
    uint32_t           flags;        /* 状态标志 */
} urma_log_rl_state_t;

/* 静态初始化宏 */
#define URMA_LOG_RL_STATE_INIT(name) { \
    .lock = PTHREAD_SPINLOCK_INITIALIZER, \
    .n_left = ATOMIC_VAR_INIT(URMA_LOG_RL_LIMIT), \
    .missed = ATOMIC_VAR_INIT(0), \
    .flags = 0 \
}

/* 定义流控状态变量 */
#define DEFINE_URMA_LOG_RL_STATE(name) \
    static urma_log_rl_state_t name = URMA_LOG_RL_STATE_INIT(name)
```

**设计要点:**
- **自旋锁(pthread_spinlock_t)**: 适合持锁时间极短的场景，不会让出CPU
- **原子变量(atomic_uint)**: 无锁计数操作，避免锁竞争
- **per-state设计**: 每个日志点独立状态，无需全局查找
- **静态定义**: 编译时初始化，无运行时分配开销

### 3.2 核心函数设计

#### 3.2.1 流控检查函数
```c
/**
 * @brief 检查日志是否应该被流控
 * @param rs 流控状态指针
 * @return true: 日志可以记录
 *         false: 日志应被流控(丢弃)
 */
bool urma_log_rl_check(urma_log_rl_state_t *rs);
```

**处理流程:**
```
1. 获取当前时间 now
2. 尝试获取自旋锁 (trylock)
   a. 如果获取失败(锁竞争):
      - 检查是否已初始化且剩余配额 > 0
      - 原子递减 n_left，如果结果 >= 0 则允许记录
      - 跳转到步骤5
3. 获取锁后的完整处理:
   a. 检查是否已初始化，未初始化则初始化窗口
   b. 检查窗口是否过期 (now - begin >= WINDOW_SEC)
      - 过期: 重置窗口，输出摘要(如有)
   c. 原子检查配额，递减 n_left
4. 解锁
5. 如果不允许记录: 原子递增 missed
6. 返回结果
```

**关键设计点:**
- **trylock快速路径**: 锁竞争时不阻塞，用原子操作快速判断
- **原子变量无锁计数**: n_left 和 missed 使用原子操作
- **避免死锁**: 解锁后再输出摘要日志
- **无额外线程**: 窗口重置和摘要输出在流控检查时惰性触发

#### 3.2.2 流控检查函数实现
```c
bool urma_log_rl_check(urma_log_rl_state_t *rs) {
    time_t now = time(NULL);
    bool ret = false;

    /* 快速路径：trylock失败时用原子操作快速判断 */
    if (pthread_spin_trylock(&rs->lock) != 0) {
        /* 锁竞争时，原子检查剩余配额 */
        if ((rs->flags & URMA_LOG_RL_INITIALIZED) &&
            atomic_load(&rs->n_left) > 0 &&
            atomic_fetch_sub(&rs->n_left, 1) >= 0) {
            ret = true;
        }
        goto out;
    }

    /* 获取锁后的完整处理 */
    if (!(rs->flags & URMA_LOG_RL_INITIALIZED)) {
        rs->begin = now;
        rs->flags |= URMA_LOG_RL_INITIALIZED;
        atomic_store(&rs->n_left, URMA_LOG_RL_LIMIT);
    }

    /* 检查窗口是否过期 */
    if (now - rs->begin >= URMA_LOG_RL_WINDOW_SEC) {
        atomic_store(&rs->n_left, URMA_LOG_RL_LIMIT);
        rs->begin = now;

        /* 输出摘要 */
        uint32_t m = atomic_exchange(&rs->missed, 0);
        if (m > 0) {
            /* 解锁后输出，避免死锁 */
            pthread_spin_unlock(&rs->lock);
            urma_log(__func__, __LINE__, URMA_VLOG_LEVEL_INFO,
                    "rate limit: %u logs suppressed in last %ds",
                    m, URMA_LOG_RL_WINDOW_SEC);
            return false;
        }
    }

    /* 原子检查配额 */
    if (atomic_load(&rs->n_left) > 0 && atomic_fetch_sub(&rs->n_left, 1) >= 0) {
        ret = true;
    }

    pthread_spin_unlock(&rs->lock);

out:
    if (!ret) {
        atomic_fetch_add(&rs->missed, 1);
    }
    return ret;
}
```

### 3.3 宏定义设计

#### 3.3.1 流控日志宏
```c
/* urma_log.h */

/* 内部流控日志宏 */
#define URMA_LOG_RL(l, rs, ...) \
    do { \
        if (!urma_log_drop(URMA_VLOG_LEVEL_##l)) { \
            if (urma_log_rl_check(rs)) { \
                urma_log(__func__, __LINE__, URMA_VLOG_LEVEL_##l, __VA_ARGS__); \
            } \
        } \
    } while (0)

/* 对外暴露的流控日志宏 - 每次调用定义独立的流控状态 */
#define URMA_LOG_ERR_RL(...) \
    do { \
        DEFINE_URMA_LOG_RL_STATE(_rs); \
        URMA_LOG_RL(ERR, &_rs, __VA_ARGS__); \
    } while (0)

#define URMA_LOG_WARN_RL(...) \
    do { \
        DEFINE_URMA_LOG_RL_STATE(_rs); \
        URMA_LOG_RL(WARNING, &_rs, __VA_ARGS__); \
    } while (0)

#define URMA_LOG_INFO_RL(...) \
    do { \
        DEFINE_URMA_LOG_RL_STATE(_rs); \
        URMA_LOG_RL(INFO, &_rs, __VA_ARGS__); \
    } while (0)

#define URMA_LOG_DEBUG_RL(...) \
    do { \
        DEFINE_URMA_LOG_RL_STATE(_rs); \
        URMA_LOG_RL(DEBUG, &_rs, __VA_ARGS__); \
    } while (0)
```

**说明:** 每个日志点通过 `DEFINE_URMA_LOG_RL_STATE` 定义独立的静态流控状态变量，实现按日志点独立流控。

### 3.4 流控摘要输出策略

#### 3.4.1 触发时机
流控摘要在窗口过期时输出：
- 当检测到时间窗口已过期，且该窗口有被流控的日志(missed > 0)时，立即输出摘要
- 无需定时器或额外线程，在流控检查时惰性触发

#### 3.4.2 输出内容
```
[URMA][liburma][thread_id=xxx][-][func_name[Line=123]][rate limit: 150 logs suppressed in last 5s]
```

### 3.5 线程安全设计

1. **自旋锁保护窗口重置**: 使用 `pthread_spinlock_t` 保护窗口重置操作
2. **原子变量保护计数**: `n_left` 和 `missed` 使用原子变量，无锁操作
3. **trylock快速路径**: 锁竞争时不阻塞，用原子操作快速判断
4. **避免死锁**: 解锁后再输出摘要日志
5. **per-state设计**: 每个日志点独立状态，无全局锁竞争

### 3.6 性能优化

#### 3.6.1 时间获取优化
```c
/* 使用 clock_gettime(CLOCK_MONOTONIC_COARSE) 替代 time() */
static inline time_t urma_log_rl_get_time(void)
{
#ifdef CLOCK_MONOTONIC_COARSE
    /* Linux: use CLOCK_MONOTONIC_COARSE for better performance */
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    return ts.tv_sec;
#else
    /* Fallback: use time() for portability */
    return time(NULL);
#endif
}
```
`CLOCK_MONOTONIC_COARSE` 是 Linux 特有的时钟类型，比 `time()` 性能更好，精度足够(毫秒级)。非 Linux 平台回退使用 `time()`。

#### 3.6.2 与原方案性能对比

| 对比项 | 原方案(pthread_mutex) | 新方案(spinlock+atomic) |
|--------|----------------------|------------------------|
| 锁类型 | 互斥锁(阻塞) | 自旋锁(不阻塞) |
| 锁竞争处理 | 线程阻塞等待 | trylock + 原子快速路径 |
| 计数操作 | 持锁操作 | 无锁原子操作 |
| 全局查找 | 需要遍历数组 | 无(静态定义) |
| 性能 | 中等 | 高 |

## 4. 接口设计

### 4.1 新增公共接口

| 接口 | 说明 |
|------|------|
| `URMA_LOG_ERR_RL(...)` | 带流控的错误日志宏 |
| `URMA_LOG_WARN_RL(...)` | 带流控的警告日志宏 |
| `URMA_LOG_INFO_RL(...)` | 带流控的信息日志宏 |
| `URMA_LOG_DEBUG_RL(...)` | 带流控的调试日志宏 |

### 4.2 新增内部接口

| 接口 | 说明 |
|------|------|
| `urma_log_rl_check()` | 流控检查函数，返回true允许记录，false流控 |

**注意**: 新方案采用per-state设计，每个日志点静态定义流控状态，无需全局初始化函数。

### 4.3 配置参数

可通过编译宏配置：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `URMA_LOG_RL_WINDOW_SEC` | 5 | 流控时间窗口(秒) |
| `URMA_LOG_RL_LIMIT` | 100 | 每窗口日志上限 |

**注意**: 新方案移除了 `URMA_LOG_RL_MAX_ENTRIES` 参数，因为采用per-state静态定义，无需全局条目数组。

## 5. 使用示例

### 5.1 基本使用
```c
/* 原有日志宏 - 无流控 */
URMA_LOG_ERR("Failed to connect to device %s\n", dev_name);

/* 新增流控日志宏 - 带流控 */
URMA_LOG_ERR_RL("Failed to connect to device %s\n", dev_name);
```

### 5.2 适用场景
- 网络重试循环中的错误日志
- 设备状态轮询中的异常日志
- 高频事件处理中的调试日志
- 可能被恶意触发的错误路径

### 5.3 不适用场景
- 关键错误必须记录的场景（使用普通日志宏）
- 低频操作中的日志（使用普通日志宏）
- 需要完整审计日志的场景

## 6. 实现计划

### 6.1 文件修改清单

| 文件 | 修改内容 |
|------|----------|
| `urma_log.h` | 新增流控数据结构、宏定义、函数声明 |
| `urma_log.c` | 实现流控检查、摘要输出等函数 |

### 6.2 实现步骤

1. **Step 1**: 在 `urma_log.h` 中定义数据结构和宏
2. **Step 2**: 在 `urma_log.c` 中实现流控检查函数 `urma_log_rl_check()`
3. **Step 3**: 添加单元测试（可选）
4. **Step 4**: 在关键位置应用流控日志宏

## 7. 测试方案

### 7.1 单元测试
- 测试流控计数正确性（100条限制）
- 测试窗口重置逻辑（5秒窗口）
- 测试多线程并发场景（锁竞争、trylock快速路径）
- 测试流控摘要输出正确性

### 7.2 集成测试
- 模拟高频日志场景，验证流控效果
- 验证流控摘要输出格式
- 验证与现有日志系统的兼容性
- 验证不同日志点独立流控

### 7.3 性能测试
- 测量流控日志的性能开销
- 对比普通日志和流控日志的性能差异
- 高并发场景下的锁竞争性能测试

## 8. 风险评估

### 8.1 潜在风险
| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| 流控导致关键日志丢失 | 中 | 仅在明确需要流控的场景使用_RL宏 |
| 多线程锁竞争 | 极低 | trylock快速路径，锁竞争时用原子操作 |
| 窗口过期后无新日志导致摘要丢失 | 低 | 属于可接受行为，该日志点已不再活跃 |
| 静态变量增加代码段大小 | 极低 | 每个日志点约32字节，影响极小 |

### 8.2 回退方案
- 流控功能完全独立，不影响现有日志宏
- 可通过不使用_RL宏完全禁用流控功能

## 9. 附录

### 9.1 Linux内核日志流控实现原理调研

#### 9.1.1 内核数据结构
Linux内核使用 `ratelimit_state` 结构实现流控：
```c
/* include/linux/ratelimit_types.h */
struct ratelimit_state {
    raw_spinlock_t  lock;       /* 保护状态的自旋锁 */
    int             interval;   /* 时间窗口(单位: jiffies, 默认5*HZ=5秒) */
    int             burst;      /* 窗口内最大日志数(默认10) */
    atomic_t        rs_n_left;  /* 窗口内剩余配额 */
    atomic_t        missed;     /* 被流控的日志数 */
    unsigned int    flags;      /* 状态标志 */
    unsigned long   begin;      /* 窗口起始时间 */
};
```

#### 9.1.2 内核核心实现逻辑
```c
/* lib/ratelimit.c - ___ratelimit() 函数核心逻辑 */
int ___ratelimit(struct ratelimit_state *rs, const char *func)
{
    int interval = READ_ONCE(rs->interval);
    int burst = READ_ONCE(rs->burst);
    unsigned long flags;
    int ret = 0;

    /* 1. 快速路径：如果无法获取锁，尝试原子操作快速判断 */
    if (!raw_spin_trylock_irqsave(&rs->lock, flags)) {
        /* 锁竞争时，使用原子操作检查剩余配额 */
        if (READ_ONCE(rs->flags) & RATELIMIT_INITIALIZED &&
            atomic_read(&rs->rs_n_left) > 0 && 
            atomic_dec_return(&rs->rs_n_left) >= 0)
            ret = 1;  /* 允许记录 */
        goto nolock_ret;
    }

    /* 2. 获取锁后的完整处理 */
    if (!(rs->flags & RATELIMIT_INITIALIZED)) {
        rs->begin = jiffies;
        rs->flags |= RATELIMIT_INITIALIZED;
        atomic_set(&rs->rs_n_left, rs->burst);
    }

    /* 3. 检查窗口是否过期 */
    if (time_is_before_jiffies(rs->begin + interval)) {
        /* 重置配额 */
        atomic_set(&rs->rs_n_left, rs->burst);
        rs->begin = jiffies;
        
        /* 输出摘要 */
        int m = ratelimit_state_reset_miss(rs);
        if (m) {
            printk_deferred(KERN_WARNING "%s: %d callbacks suppressed\n", func, m);
        }
    }

    /* 4. 检查配额 */
    if (atomic_read(&rs->rs_n_left) > 0 && atomic_dec_return(&rs->rs_n_left) >= 0)
        ret = 1;

    raw_spin_unlock_irqrestore(&rs->lock, flags);

nolock_ret:
    if (!ret)
        ratelimit_state_inc_miss(rs);  /* 原子增加missed计数 */
    return ret;
}
```

#### 9.1.3 内核设计关键特点

| 特点 | 说明 |
|------|------|
| **自旋锁 + 原子变量混合** | 使用 `raw_spinlock_t` 保护窗口重置，使用 `atomic_t` 保护计数操作 |
| **trylock策略** | 锁竞争时不阻塞，直接用原子操作快速判断，减少锁等待 |
| **原子计数** | `rs_n_left` 和 `missed` 使用原子变量，无锁操作 |
| **per-state设计** | 每个流控点有独立的 `ratelimit_state`，无需全局查找 |
| **printk_deferred** | 输出摘要时不直接调用printk，避免递归和死锁 |
| **默认参数** | interval=5秒, burst=10条 |

#### 9.1.4 内核与用户态方案对比

| 对比项 | Linux内核 | 本方案(spinlock+atomic) |
|--------|-----------|-------------------------|
| 锁类型 | `raw_spinlock_t` (自旋锁) | `pthread_spinlock_t` (自旋锁) |
| 锁策略 | trylock + 原子操作快速路径 | trylock + 原子操作快速路径 |
| 计数方式 | 原子变量 `atomic_t` | 原子变量 `atomic_uint` |
| 条目管理 | 静态定义per-state | 静态定义per-state |
| 窗口检查 | 持锁检查 | 持锁检查 |
| 摘要输出 | deferred输出 | 解锁后输出 |
| 性能影响 | 极低(快速路径无锁) | 极低(快速路径无锁) |

### 9.2 术语说明
- **流控(Rate Limiting)**: 限制单位时间内日志输出数量
- **时间窗口**: 流控统计的时间范围
- **日志点**: 由函数名和行号唯一标识的日志调用位置
