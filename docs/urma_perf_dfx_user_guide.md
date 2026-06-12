# URMA 性能监控 DFX 用户操作说明书

## 一、概述

URMA 提供了一套性能监控 DFX（Design for X）功能，用于采集和分析 URMA 关键操作的性能数据。该功能通过 API 接口提供，不支持命令行方式调用。

**主要功能**：
- 监控 URMA 核心操作（Post、Poll、Wait 等）的延迟统计
- 支持多线程环境下的性能数据采集
- 提供完整的延迟分布统计（平均值、最小值、最大值、百分位值）

**监控的操作类型**：
| 类型 | 说明 |
|------|------|
| `UB_JETTY_POST_SEND` | UB Jetty 发送操作延迟 |
| `BOND_JETTY_POST_SEND` | Bond Jetty 发送操作延迟 |
| `UB_JFS_POST_SEND` | UB JFS 发送操作延迟 |
| `BOND_JFS_POST_SEND` | Bond JFS 发送操作延迟 |
| `UB_JETTY_POST_RECV` | UB Jetty 接收操作延迟 |
| `BOND_JETTY_POST_RECV` | Bond Jetty 接收操作延迟 |
| `UB_POST_JFR_RECV` | UB JFR 接收操作延迟 |
| `BOND_POST_JFR_RECV` | Bond JFR 接收操作延迟 |
| `UB_POLL_JFC` | UB Poll JFC 操作延迟 |
| `BOND_POLL_JFC` | Bond Poll JFC 操作延迟 |
| `UB_WAIT_JFC` | UB Wait JFC 操作延迟 |
| `BOND_WAIT_JFC` | Bond Wait JFC 操作延迟 |
| `UB_ACK_JFC` | UB Ack JFC 操作延迟 |
| `UB_REARM_JFC` | UB Rearm JFC 操作延迟 |
| `BOND_REARM_JFC` | Bond Rearm JFC 操作延迟 |

---

## 二、API 详细说明

### 2.1 urma_start_perf

#### 功能描述

启动 URMA 性能监控功能。调用此 API 后，URMA 内部会开始记录各操作的延迟数据。

#### 函数原型

```c
urma_status_t urma_start_perf(void);
```

#### 参数

无参数。

#### 返回值

| 返回值 | 说明 |
|-------|------|
| `URMA_SUCCESS (0)` | 成功 |

#### 实现行为

1. **全局上下文初始化**（仅首次调用时执行）：
   - 初始化自旋锁 `g_urma_perf_record_lock`
   - 创建线程私有数据键 `perf_record_clean_key`，用于线程退出时清理资源

2. **重置统计数据**：
   - 清零所有线程的性能记录数据
   - 重置所有 bucket（延迟分布桶）计数

3. **启用性能监控**：
   - 设置 `g_urma_perf_record_enable = true`

#### 使用约束

| 约束项 | 说明 |
|-------|------|
| **调用时机** | 必须在执行需要监控的业务操作之前调用 |
| **重复调用** | 允许重复调用，每次调用会重置所有统计数据 |
| **线程安全** | 是，全局操作会自动同步 |
| **最大线程数** | 最多支持 `URMA_PERF_THREAD_MAX_NUM (128)` 个线程同时记录性能数据 |
| **性能影响** | 存在轻微性能开销，建议仅在诊断场景启用 |

#### 使用示例

```c
#include <urma_perf.h>

int main() {
    // 初始化 URMA
    urma_init(NULL);

    // 启动性能监控
    urma_status_t ret = urma_start_perf();
    if (ret != URMA_SUCCESS) {
        fprintf(stderr, "Failed to start perf: %d\n", ret);
        return -1;
    }

    // 执行业务操作...
    run_workload();

    // 停止监控并获取数据
    urma_stop_perf();

    char perf_buf[8192];
    uint32_t len = sizeof(perf_buf);
    urma_get_perf_info(perf_buf, &len);
    printf("%s\n", perf_buf);

    return 0;
}
```

---

### 2.2 urma_stop_perf

#### 功能描述

停止 URMA 性能监控功能。调用后，URMA 内部停止记录新的性能数据，但已采集的数据仍然保留。

#### 函数原型

```c
urma_status_t urma_stop_perf(void);
```

#### 参数

无参数。

#### 返回值

| 返回值 | 说明 |
|-------|------|
| `URMA_SUCCESS (0)` | 成功 |

#### 实现行为

1. **禁用性能监控**：
   - 设置 `g_urma_perf_record_enable = false`

2. **保留已采集数据**：
   - 不清空已有的统计数据
   - 后续可通过 `urma_get_perf_info` 获取数据

#### 使用约束

| 约束项 | 说明 |
|-------|------|
| **调用时机** | 应在业务操作完成后、获取性能数据前调用 |
| **重复调用** | 允许重复调用，幂等操作 |
| **线程安全** | 是 |
| **数据保留** | 停止后数据仍保留，直到下次 `urma_start_perf` 调用 |

#### 使用示例

```c
// 停止性能监控
urma_status_t ret = urma_stop_perf();
if (ret != URMA_SUCCESS) {
    fprintf(stderr, "Failed to stop perf: %d\n", ret);
    return -1;
}

// 获取性能数据
char perf_buf[8192];
uint32_t len = sizeof(perf_buf);
ret = urma_get_perf_info(perf_buf, &len);
```

---

### 2.3 urma_get_perf_info

#### 功能描述

获取性能监控统计数据。返回格式化的文本报告，包含各操作的延迟统计信息。

#### 函数原型

```c
urma_status_t urma_get_perf_info(char *perf_buf, uint32_t *length);
```

#### 参数

| 参数 | 方向 | 说明 |
|------|------|------|
| `perf_buf` | [OUT] | 用于存储性能数据的缓冲区 |
| `length` | [IN/OUT] | 输入为缓冲区大小，输出为实际写入的数据长度（含结尾符） |

#### 返回值

| 返回值 | 说明 |
|-------|------|
| `URMA_SUCCESS (0)` | 成功 |
| `URMA_EINVAL` | 参数无效（`perf_buf` 或 `length` 为 NULL，或缓冲区不足） |

#### 实现行为

1. **聚合多线程数据**：
   - 遍历所有线程的性能记录槽位
   - 汇总每个操作类型的统计数据：
     - `accumulation`：累计延迟总和
     - `cnt`：采样次数
     - `min`：最小延迟
     - `max`：最大延迟
     - `bucket[]`：延迟分布桶

2. **计算统计指标**：
   - 平均值：`accumulation / cnt`
   - 百分位值（p90、p99、p9999）：基于 bucket 分布计算

3. **格式化输出**：
   - 生成可读的文本报告

#### 输出格式

```
+----------------------+----------+----------+----------+----------+----------+----------+----------+
  retry_count: 0
+----------------------+----------+----------+----------+----------+----------+----------+----------+
  Type                 | samples  | avg[ns]  | min[ns]  | max[ns]  | p90[ns]  | p99[ns]  | p9999[ns]
+----------------------+----------+----------+----------+----------+----------+----------+----------+
  UB_JETTY_POST_SEND   | 10000    | 1234     | 500      | 5000     | 1500     | 2000     | 3000
  UB_POLL_JFC          | 10000    | 567      | 100      | 2000     | 800      | 1200     | 1800
+----------------------+----------+----------+----------+----------+----------+----------+----------+
```

#### 使用约束

| 约束项 | 说明 |
|-------|------|
| **缓冲区大小** | 建议至少 8192 字节；若不足，API 返回 `URMA_EINVAL` 并打印需要的字节数 |
| **调用时机** | 应在 `urma_stop_perf` 之后调用 |
| **数据状态** | 返回的是调用时刻的统计数据快照 |
| **线程安全** | 是 |

#### 使用示例

```c
char perf_buf[8192];
uint32_t len = sizeof(perf_buf);

urma_status_t ret = urma_get_perf_info(perf_buf, &len);
if (ret != URMA_SUCCESS) {
    fprintf(stderr, "Failed to get perf info: %d, need %u bytes\n", ret, len);
    return -1;
}

printf("Performance Report:\n%s\n", perf_buf);
```

---

## 三、完整使用流程

### 3.1 基本使用流程

```
┌─────────────────────────────────────────────────────────────────┐
│                        性能监控使用流程                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│   1. urma_start_perf()     ← 启动监控，重置统计数据              │
│         │                                                       │
│         ▼                                                       │
│   2. 执行业务操作           ← URMA 内部自动采集延迟数据          │
│         │                                                       │
│         ▼                                                       │
│   3. urma_stop_perf()      ← 停止监控，保留已采集数据            │
│         │                                                       │
│         ▼                                                       │
│   4. urma_get_perf_info()  ← 获取格式化的统计报告               │
│         │                                                       │
│         ▼                                                       │
│   5. 分析性能数据          ← 根据统计结果进行性能调优            │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 3.2 完整示例代码

```c
#include <stdio.h>
#include <urma_api.h>
#include <urma_perf.h>

#define PERF_BUF_SIZE 8192

int main(int argc, char *argv[])
{
    urma_status_t ret;

    // 1. 初始化 URMA 环境
    ret = urma_init(NULL);
    if (ret != URMA_SUCCESS) {
        fprintf(stderr, "urma_init failed: %d\n", ret);
        return -1;
    }

    // 2. 创建必要的资源（context、jfs、jfr、jfc 等）
    // ... 省略资源创建代码 ...

    // 3. 启动性能监控
    ret = urma_start_perf();
    if (ret != URMA_SUCCESS) {
        fprintf(stderr, "urma_start_perf failed: %d\n", ret);
        goto cleanup;
    }
    printf("Performance monitoring started.\n");

    // 4. 执行需要监控的业务操作
    for (int i = 0; i < 10000; i++) {
        // 执行 Post、Poll 等操作
        // urma_post_jfs_wr(...);
        // urma_poll_jfc(...);
    }

    // 5. 停止性能监控
    ret = urma_stop_perf();
    if (ret != URMA_SUCCESS) {
        fprintf(stderr, "urma_stop_perf failed: %d\n", ret);
        goto cleanup;
    }
    printf("Performance monitoring stopped.\n");

    // 6. 获取性能统计数据
    char perf_buf[PERF_BUF_SIZE];
    uint32_t len = PERF_BUF_SIZE;

    ret = urma_get_perf_info(perf_buf, &len);
    if (ret != URMA_SUCCESS) {
        fprintf(stderr, "urma_get_perf_info failed: %d, need %u bytes\n", ret, len);
        goto cleanup;
    }

    // 7. 打印性能报告
    printf("\n=== URMA Performance Report ===\n%s\n", perf_buf);

cleanup:
    // 8. 清理资源
    urma_uninit();
    return (ret == URMA_SUCCESS) ? 0 : -1;
}
```

---

## 四、多线程使用说明

### 4.1 线程模型

URMA 性能监控支持多线程环境：

- 每个线程首次调用 `urma_get_perf_timestamp()` 时，自动分配一个性能记录槽位
- 最多支持 **128** 个线程同时记录性能数据
- 线程退出时自动释放槽位（通过 pthread  cleanup 机制）

### 4.2 多线程示例

```c
#include <pthread.h>

#define THREAD_NUM 4
#define ITERATIONS 10000

void *worker_thread(void *arg)
{
    // 线程首次获取时间戳时自动初始化性能记录槽位
    // 无需显式调用初始化 API

    for (int i = 0; i < ITERATIONS; i++) {
        // 执行业务操作，延迟数据自动记录到本线程的槽位
        urma_post_jfs_wr(jfs, &wr, &bad_wr);
        urma_poll_jfc(jfc, 16, cr);
    }

    return NULL;
}

int main()
{
    pthread_t threads[THREAD_NUM];

    urma_init(NULL);

    // 启动性能监控
    urma_start_perf();

    // 创建多个工作线程
    for (int i = 0; i < THREAD_NUM; i++) {
        pthread_create(&threads[i], NULL, worker_thread, NULL);
    }

    // 等待所有线程完成
    for (int i = 0; i < THREAD_NUM; i++) {
        pthread_join(threads[i], NULL);
    }

    // 停止监控并获取汇总数据
    urma_stop_perf();

    char perf_buf[8192];
    uint32_t len = sizeof(perf_buf);
    urma_get_perf_info(perf_buf, &len);
    printf("%s\n", perf_buf);

    urma_uninit();
    return 0;
}
```

### 4.3 多线程约束

| 约束项 | 说明 |
|-------|------|
| **最大线程数** | 最多 128 个线程可同时记录性能数据 |
| **超出限制** | 超出 128 线程后，新线程无法记录数据，日志输出错误信息 |
| **数据聚合** | `urma_get_perf_info` 自动聚合所有线程的数据 |
| **线程退出** | 线程退出时自动清理槽位，可供新线程复用 |

---

## 五、性能数据解读

### 5.1 统计指标说明

| 指标 | 单位 | 说明 |
|------|------|------|
| `samples` | 次数 | 该操作被采样的总次数 |
| `avg` | 纳秒 | 平均延迟 |
| `min` | 纳秒 | 最小延迟 |
| `max` | 纳秒 | 最大延迟 |
| `p90` | 纳秒 | 90% 的请求延迟低于此值 |
| `p99` | 纳秒 | 99% 的请求延迟低于此值 |
| `p9999` | 纳秒 | 99.99% 的请求延迟低于此值 |
| `retry_count` | 次数 | Bond 设备切换重试次数 |

### 5.2 性能分析方法

1. **延迟异常排查**：
   - 关注 `max` 值是否异常大
   - 关注 `p99`/`p9999` 与 `avg` 的差距，差距大说明存在长尾延迟

2. **瓶颈定位**：
   - 对比各操作的平均延迟，找出瓶颈操作
   - 关注 `POLL_JFC` 延迟，反映完成队列处理效率

3. **趋势分析**：
   - 多次采样对比，观察性能趋势
   - 结合业务负载变化分析性能波动

---

## 六、注意事项

### 6.1 重要提示

| 提示项 | 说明 |
|-------|------|
| **仅 API 方式** | 性能监控功能只能通过 API 调用，不支持命令行工具 |
| **性能开销** | 启用监控会有轻微性能开销，生产环境建议按需开启 |
| **数据清空** | 每次 `urma_start_perf` 会清空历史数据 |
| **缓冲区大小** | 建议使用 8192 字节以上的缓冲区获取性能数据 |

### 6.2 常见问题

**Q1: 为什么获取不到性能数据？**
- 确认已调用 `urma_start_perf()`
- 确认业务操作已执行
- 确认在 `urma_stop_perf()` 之后调用 `urma_get_perf_info()`

**Q2: 缓冲区不足怎么办？**
- API 返回 `URMA_EINVAL` 时，`length` 参数会返回需要的字节数
- 增大缓冲区后重新调用

**Q3: 多线程环境下数据不准确？**
- 确认线程数未超过 128
- 检查日志中是否有 "no available thread slot" 错误

### 6.3 编译链接

```bash
# 编译
gcc -I/path/to/urma/include -c my_app.c

# 链接
gcc my_app.o -lurma -o my_app
```

---

## 七、头文件

```c
#include <urma_perf.h>
```

依赖头文件：
```c
#include <urma_types.h>
#include <urma_opcode.h>
```
