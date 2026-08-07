<!-- SPDX-License-Identifier: MIT -->
# URMA 数据面无锁 DFX 设计

> 本文基于 umdk 用户态实现（核心为 urma_perf.c）走读整理，聚焦商用 URMA 数据面通信性能场景下，为解决数据面统计的锁竞争开销而引入的**用户态无锁 DFX（延迟性能统计）设计**。
>
> 本设计位于 umdk 用户态库，区别于内核态 DFX（内核态经 netlink 上报，见 admin_cmd_perf.c，非本文重点）。

## 目录

- [1. 问题背景](#1-问题背景)
  - 1.1 商用数据面通信的性能痛点
  - 1.2 传统加锁统计在数据面的不足
- [2. 解决方案与技术点](#2-解决方案与技术点)
  - 2.1 总体设计思路
  - 2.2 关键技术点概览
- [3. 技术介绍](#3-技术介绍)
  - 3.1 per-thread 无锁统计与槽位管理
  - 3.2 对数分桶延迟直方图
  - 3.3 低开销时间戳：CNTVCT 直读
  - 3.4 热路径零开销开关与惰性初始化
  - 3.5 UB / BOND 双路径埋点
  - 3.6 离线聚合与百分位计算
  - 3.7 控制接口与上报通道
  - 3.8 辅助原子计数器
- [4. 解决效果](#4-解决效果)
- [5. 总结](#5-总结)
- [6. 关键文件索引](#6-关键文件索引)

---

## 1. 问题背景

### 1.1 商用数据面通信的性能痛点

URMA 用户态库提供数据面接口（post_send / post_recv / poll / rearm / wait 等），商用场景对纳秒级延迟与多线程高并发吞吐极为敏感。为支撑数据面性能观测，库内引入一套 DFX 延迟统计机制，覆盖直连路径（UB）与聚合路径（BOND）两套调用栈，输出 p90 / p99 / p9999 长尾分位。

### 1.2 传统加锁统计在数据面的不足

统计点位于每次 post / poll / rearm / wait 操作上，调用频率与业务流量同阶。若统计采用单一全局计数器 + 互斥锁/自旋锁的传统方案，存在以下问题：

1. **锁竞争**：多线程同时更新需抢同一把锁，热路径排队退化。
2. **缓存行抖动**：共享计数器在多核间反复失效，开销随核数上升而恶化。
3. **长尾刻画不足**：仅计数/平均值无法反映 p90/p99/p9999 长尾分布，而商用场景恰恰关注长尾。

因此数据面 DFX 必须采用**无锁、per-thread 隔离、低开销时间源**的设计。

---

## 2. 解决方案与技术点

### 2.1 总体设计思路

核心思想：**每线程一份独立统计槽位，写路径只访问本线程槽位，无需任何锁；读路径离线聚合全部线程槽位。**

用户态下，"per-CPU" 对应为 "per-thread"（线程局部存储 TLS），这是本设计与内核态 per-CPU 方案的根本差异。

### 2.2 关键技术点概览

- per-thread 无锁统计与槽位生命周期管理
- 对数分桶延迟直方图（64 桶覆盖 1ns ~ 4s）
- 低开销时间戳：AArch64 CNTVCT 直读
- 热路径零开销开关与惰性初始化
- UB / BOND 双路径埋点
- 离线聚合与百分位计算
- 控制接口与 umq 上报通道
- 辅助原子计数器

---

## 3. 技术介绍

### 3.1 per-thread 无锁统计与槽位管理

- 全局上下文持有 `record_table[128]`（每线程一槽），写路径通过 `__thread` 变量 `g_perf_record_index` 直接定位，零锁、零跨核访存。
- 槽位在首次采集时惰性分配（分配时短暂持自旋锁，热路径不持锁）；线程退出回调仅置 `thread_initialized = false`，不擦除数据，便于事后聚合。
- 无空闲槽时复用已退出线程的槽位；注意**复用不清零**，新线程会继承旧线程本窗口内的样本。
- **128 槽上限**：超过后分配失败，后续统计静默跳过，商用多线程场景需关注该边界。

### 3.2 对数分桶延迟直方图

- 64 桶（主桶 32 × 子桶 2），用 `__builtin_clzll` 求 `ilog2(latency)` 做对数主分桶，桶内线性细分。
- 分位定位由 `urma_perf_cal_quantile()` + `bucket_low_bound()` 完成，在目标桶内均匀插值估算具体延迟。
- 固定桶数同时覆盖 1ns ~ 4s 大动态范围，比内核态 2048 桶更精简，降低内存占用。

### 3.3 低开销时间戳：CNTVCT 直读

- AArch64 下直读 `CNTVCT_EL0`（不插 ISB），牺牲约 10~20ns 抖动换取省掉 ISB 的 15~20ns 串行化开销；mult/shift 借鉴内核 `clocks_calc_mult_shift()` 算法离线计算，并以 atomic 存储保证跨核可见。
- 时间戳以原始 tick 记录，**tick→ns 转换延迟到离线聚合时统一在 delta 上做一次**，减少乘除次数。
- 未启用 CNTVCT 时回退 `clock_gettime(CLOCK_MONOTONIC)`；编译开关默认在 AArch64 打开。

### 3.4 热路径零开销开关与惰性初始化

- 全局开关为 `atomic_bool`，埋点入口用 `memory_order_relaxed` 原子读判定，关闭时立即跳过；默认零侵入。
- 全局上下文（自旋锁、pthread_key）在首次 `urma_start_perf()` 时经双重检查懒加载；线程槽位在首次取时间戳时分配。

### 3.5 UB / BOND 双路径埋点

- **UB 路径**（`UDMA_PERF_PROFILING_*` 宏）：位于 urma_dp_api.c，通过 `urma_is_bonding_dev()` 排除聚合设备，覆盖 poll / rearm / wait / ack / post send / post recv 等 8 类。
- **BOND 路径**（`PERF_PROFILING_*` 宏）：位于 bondp_datapath.c 与 bondp_dp_interrupt.c，覆盖 poll / wait / rearm / post send / post recv 等 7 类。
- 注意：wait 类埋点包裹了实际阻塞等待（含 epoll_wait 超时），p90/p99 反映端到端等待时长，与 poll/post 的纯处理延迟语义不同。

### 3.6 离线聚合与百分位计算

- `urma_perf_dump_info()` 遍历 128 个在用槽位，把各线程 cnt / accumulation / bucket 累加，min / max 取极值。
- `urma_perf_fill_type_stats()` 依据桶累计计数定位 p90 / p99 / p9999 桶边界并插值，同时输出 average / min / max。
- 聚合与统计只发生在 dump（冷路径），不进入热路径。

### 3.7 控制接口与上报通道

- 三个控制接口：`urma_start_perf()`（关采集 → 懒加载 → 计算 mult/shift → 清零全部槽位 → 开采集）、`urma_stop_perf()`、`urma_get_perf_info()`。
- urma 库以动态库形式被 umq（urpc 进程）加载，三个接口经 umq_symbol_private.c 以函数指针方式导出，由 umq 的 dfx ops 表对外提供启停与查询。
- 清零在每次 start 时遍历全部槽位，为冷路径一次性成本；热路径写线程不持锁，窗口边界存在极小概率的撕裂/丢样本，作为统计工具可容忍。

### 3.8 辅助原子计数器

- `retry_count` 来自 ubagg_stats.c 的全局 `atomic_uint`，在 bond failover 重发路径自增，作为聚合层切换行为的伴随指标。
- 注意：该计数器为**进程生命周期累计值**，不会随 `urma_start_perf()` 清零，与直方图的统计窗口不同。

---

## 4. 解决效果

- **写路径零锁、零跨核访存**：消除锁竞争与缓存行抖动，统计开销与核数无关。
- **默认零侵入**：关闭状态下热路径仅一次 relaxed 原子读 + 条件判断。
- **长尾可观测**：64 桶对数直方图 + 离线聚合，输出 samples / avg / min / max / p90 / p99 / p9999。
- **时间开销可控**：CNTVCT 直读 + 转换延迟到冷路径，热路径只做整数运算与本地写。
- **双路径可定位**：UB / BOND 分开统计，配合 `retry_count` 可快速判断耗时归属。

---

## 5. 总结

本设计以 per-thread 无锁统计为核心，配合对数分桶直方图与低开销时间源，解决了商用数据面统计的锁竞争、缓存行抖动与长尾不可见问题；通过零开销开关保证默认不拖累业务，并通过 umq 提供统一的启停与查询通道。整体方案在用户态数据面热路径上以最小代价换取可量化的延迟观测能力。

---

## 6. 关键文件索引

| 内容 | 文件 |
|------|------|
| 数据面无锁 DFX 实现（核心） | urma_perf.c |
| DFX 接口与类型定义 | urma_perf.h |
| 探针埋点宏 + bonding 判定 | urma_private.h |
| UB 路径埋点（直连数据面） | urma_dp_api.c |
| BOND 路径埋点（聚合数据面） | bondp_datapath.c |
| BOND 中断/等待路径埋点 | bondp_dp_interrupt.c |
| bond 切换原子计数器 | ubagg_stats.c |
| umq 动态加载 DFX 符号 | umq_symbol_private.c |
| 内核态 DFX 对照（netlink 上报，非本文重点） | admin_cmd_perf.c |
