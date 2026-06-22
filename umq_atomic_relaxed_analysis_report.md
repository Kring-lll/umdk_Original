# UMQ模块 __ATOMIC_RELAXED 使用分析与修复报告

## 1. 背景

对 `src/urpc/umq` 目录下所有 `__ATOMIC_RELAXED` 使用进行了系统性审查，共发现92处使用。在ARM等弱序内存架构上，`__ATOMIC_RELAXED` 不保证跨线程可见性和操作顺序，可能导致并发bug。x86因TSO（Total Store Ordering）特性，大部分RELAXED场景不会暴露问题，但代码的正确性不应依赖架构特性。

---

## 2. 风险分类标准

| 风险等级 | 定义 | 典型场景 |
|----------|------|----------|
| **高风险** | 可能导致功能故障（死锁、数据丢失、use-after-free） | 跨线程信号传递、引用计数释放、流控核心决策变量 |
| **中风险** | 可能导致偶发异常行为（撕裂值、误判状态） | 非原子写+原子读(data race UB)、销毁前状态检查 |
| **低风险** | 理论上不严谨但实际影响小 | 统计计数器、一致性补齐 |
| **无需修复** | RELAXED使用正确或上下文已提供ordering | CAS循环初始load、统计查询、mutex保护区域 |

---

## 3. 已修复问题（23处）

### 3.1 高风险修复（14处）

#### 3.1.1 need_return_credit 跨线程信号标志（3处）

monitor线程写flag + eventfd_write，poll线程先检查flag再决定是否读eventfd。RELAXED在ARM上不保证跨线程可见性，poll线程可能永远看不到true，导致credit永不归还。

| # | 文件:行号 | 原代码 | 修复后 | 修复原因 |
|---|-----------|--------|--------|----------|
| 1 | umq_ub_impl.c:459 | `__atomic_store_n(&cur_node->need_return_credit, true, __ATOMIC_RELAXED)` | `__ATOMIC_RELEASE` | 写端需RELEASE确保flag更新对读端可见 |
| 2 | umq_pro_ub.c:1764 | `__atomic_load_n(&checker->need_return_credit, __ATOMIC_RELAXED)` | `__ATOMIC_ACQUIRE` | 读端需ACQUIRE与写端RELEASE配对，确保看到最新flag |
| 3 | umq_pro_ub.c:1769 | `__atomic_store_n(&checker->need_return_credit, false, __ATOMIC_RELAXED)` | `__ATOMIC_RELEASE` | 清除flag需RELEASE确保monitor线程可见，避免重复触发 |

**影响分析**：ARM上RELAXED store可能被延迟或重排，poll线程长时间看不到need_return_credit=true，共享credit无法归还，最终导致发送端credit耗尽、流控死锁。

#### 3.1.2 jfr_ctx ref_cnt 引用计数put（1处）

ref_cnt降到0时调用umq_ub_jfr_ctx_destroy释放内存。RELAXED不保证之前对jfr_ctx的写操作对其他线程可见，且与同文件ACQUIRE(1647行)/RELEASE(1652行)不一致。

| # | 文件:行号 | 原代码 | 修复后 | 修复原因 |
|---|-----------|--------|--------|----------|
| 4 | umq_ub.c:1490 | `__atomic_sub_fetch(&queue->jfr_ctx[jetty_idx]->ref_cnt, 1, __ATOMIC_RELAXED)` | `__ATOMIC_ACQ_REL` | 引用计数递减需ACQ_REL：RELEASE保证之前写可见，ACQUIRE保证看到其他线程的递减结果，防止use-after-free |

**影响分析**：ACQUIRE确保递减前看到对象完整状态，RELEASE确保递减操作对其他线程可见。若其他线程get(ACQUIRE)后put(RELAXED)，另一个线程的get(ACQUIRE)可能看不到put的递减，误认为ref_cnt>0继续使用已释放对象。

#### 3.1.3 local_rx_posted 流控credit清理exchange（1处）

exchange取出旧值用于归还credit并清零。RELAXED可能取到不完整的旧值或清零不及时可见。

| # | 文件:行号 | 原代码 | 修复后 | 修复原因 |
|---|-----------|--------|--------|----------|
| 5 | umq_ub_flow_control.c:1077 | `__atomic_exchange_n(&fc->local_rx_posted, 0, __ATOMIC_RELAXED)` | `__ATOMIC_ACQ_REL` | ACQUIRE确保拿到其他线程写入的完整posted值，RELEASE确保清零对其他线程可见，避免credit计算错误 |

#### 3.1.4 rx_consumed 流控消耗计数操作（2处）

rx_consumed值直接影响credit申请/归还决策，RELAXED不保证增量及时可见。

| # | 文件:行号 | 原代码 | 修复后 | 修复原因 |
|---|-----------|--------|--------|----------|
| 6 | umq_ub_flow_control.c:1058 | `__atomic_fetch_add(var, count, __ATOMIC_RELAXED)` | `__ATOMIC_RELEASE` | 递增需RELEASE确保增量可见，后续exchange(ACQ_REL)才能取到完整值 |
| 7 | umq_ub_flow_control.c:1069 | `__atomic_exchange_n(var, 0, __ATOMIC_RELAXED)` | `__ATOMIC_ACQ_REL` | exchange需ACQ_REL确保取到完整旧值且清零可见 |

#### 3.1.5 require_rx_count 全链路写端与读端不配对（6处）

写端（fetch_add）全用RELAXED，但读端umq_flush_rx(3342行)用ACQUIRE。acquire-release必须配对才有效：RELAXED写+ACQUIRE读不构成同步关系。

| # | 文件:行号 | 原代码 | 修复后 | 修复原因 |
|---|-----------|--------|--------|----------|
| 8 | umq_inner.h:44 | `__atomic_exchange_n(require_rx_count, 0, __ATOMIC_RELAXED)` | `__ATOMIC_ACQUIRE` | exchange需ACQUIRE拿到最新值 |
| 9 | umq_inner.h:47 | `__atomic_load_n(require_rx_count, __ATOMIC_RELAXED)` | `__ATOMIC_ACQUIRE` | load需ACQUIRE与写端RELEASE配对 |
| 10 | umq_inner.h:52 | CAS: `__ATOMIC_RELAXED, __ATOMIC_RELAXED` | `__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE` | CAS需ACQ_REL(success)/ACQUIRE(failure)保证正确ordering |
| 11 | umq_ub.c:2839 | `__atomic_fetch_add(&queue->require_rx_count, rx_cnt, __ATOMIC_RELAXED)` | `__ATOMIC_RELEASE` | 写端需RELEASE与读端ACQUIRE配对 |
| 12 | umq_ub.c:2850 | `__atomic_fetch_add(&queue->require_rx_count, cur_batch_count, __ATOMIC_RELAXED)` | `__ATOMIC_RELEASE` | 同上 |
| 13 | umq_ub.c:2866 | `__atomic_fetch_add(&queue->require_rx_count, fail_count, __ATOMIC_RELAXED)` | `__ATOMIC_RELEASE` | 同上 |

#### 3.1.6 umq_ref exchange — jetty_node释放决策（1处）

| # | 文件:行号 | 原代码 | 修复后 | 修复原因 |
|---|-----------|--------|--------|----------|
| 14 | umq_pro_ub.c:1605 | `__atomic_exchange_n(&node->umq_ref, 0, __ATOMIC_RELAXED)` | `__ATOMIC_ACQ_REL` | 清零umq_ref需让其他线程看到状态变化，决定jetty_node是否可释放 |

---

### 3.2 中风险修复（5处）

#### 3.2.1 last_send 时间戳 — 非原子写+原子读=data race UB（3处）

C11标准下，非原子写+原子读构成data race，属于undefined behavior，可能读到撕裂值（64位变量只更新了一半）。

| # | 文件:行号 | 原代码 | 修复后 | 修复原因 |
|---|-----------|--------|--------|----------|
| 15 | umq_ub_flow_control.h:70 | `queue->checker->last_send = get_timestamp_us()` (非原子写) | `__atomic_store_n(&checker->last_send, ..., __ATOMIC_RELEASE)` | 消除data race UB，RELEASE确保写可见 |
| 16 | umq_ub_impl.c:455 | `__atomic_load_n(&cur_node->last_send, __ATOMIC_RELAXED)` | `__ATOMIC_ACQUIRE` | 与写端RELEASE配对，确保读到完整时间戳 |
| 17 | umq_ub_flow_control.c:850,853 | `queue->checker->last_send` (非原子读) | `__atomic_load_n(&checker->last_send, __ATOMIC_ACQUIRE)` | 消除data race UB，ACQUIRE确保读完整值 |

#### 3.2.2 jfr_ctx ref_cnt 销毁前检查（1处）

| # | 文件:行号 | 原代码 | 修复后 | 修复原因 |
|---|-----------|--------|--------|----------|
| 18 | umq_ub_impl.c:1395 | `__atomic_load_n(&queue->jfr_ctx[...]->ref_cnt, __ATOMIC_RELAXED)` | `__ATOMIC_ACQUIRE` | RELAXED可能看到过低旧值，误判ref_cnt==1允许销毁，实际其他线程已increment但未可见 |

#### 3.2.3 umq_inc_ref 引用计数递增（1处）

| # | 文件:行号 | 原代码 | 修复后 | 修复原因 |
|---|-----------|--------|--------|----------|
| 19 | umq_inner.h:61 | `__atomic_fetch_add(ref_cnt, n, __ATOMIC_RELAXED)` | `__ATOMIC_ACQUIRE` | 引用计数递增需ACQUIRE，确保递增线程看到对象完整状态后再使用 |

---

### 3.3 低风险修复（4处）

#### 3.3.1 umq_dec_ref — 与umq_inc_ref一致性补齐（1处）

| # | 文件:行号 | 原代码 | 修复后 | 修复原因 |
|---|-----------|--------|--------|----------|
| 20 | umq_inner.h:70 | `__atomic_fetch_sub(ref_cnt, n, __ATOMIC_RELAXED)` | `__ATOMIC_RELEASE` | 与umq_inc_ref(ACQUIRE)配对一致性，acquire-release需成对使用 |

#### 3.3.2 remote_rx_window 流控阈值决策load（1处）

| # | 文件:行号 | 原代码 | 修复后 | 修复原因 |
|---|-----------|--------|--------|----------|
| 21 | umq_ub_flow_control.c:401 | `__atomic_load_n(&fc->remote_rx_window, __ATOMIC_RELAXED)` | `__ATOMIC_ACQUIRE` | 流控阈值决策需看到最新窗口值，避免基于过期数据做credit请求决策 |

#### 3.3.3 CREDIT_POOL_IDLE credit可用性检查load（2处）

| # | 文件:行号 | 原代码 | 修复后 | 修复原因 |
|---|-----------|--------|--------|----------|
| 22 | umq_ub_flow_control.c:750 | `__atomic_load_n(&pool->stats_u16[CREDIT_POOL_IDLE], __ATOMIC_RELAXED)` | `__ATOMIC_ACQUIRE` | credit可用性检查需看到最新pool状态，确保credit请求/归还决策正确 |
| 23 | umq_ub_flow_control.c:972 | `__atomic_load_n(&pool->stats_u16[CREDIT_POOL_IDLE], __ATOMIC_RELAXED)` | `__ATOMIC_ACQUIRE` | 同上 |

---

## 4. 不需要修复的点（69处）

### 4.1 统计/观测计数器（约50处） — RELAXED合理

仅用于统计累加或查询展示，近似值即可，不控制程序逻辑决策路径。RELAXED是业界标准做法。

涵盖范围：
- packet_stats 相关（8处）：umq_ub_private.h:631, umq_ub_impl.c:2785, umq_ub_flow_control.c:453,438-447
- pool->stats_u16/u64 统计（8处）：umq_ub_flow_control.c:326-330,417,461,463,478
- flow_control_stats_query_atomic 中统计load（12处）：umq_ub_flow_control.c:424-435
- total_remote_rx_*, total_flow_controlled_wr 累加（4处）：umq_ub_flow_control.c:361,363,389,393
- jetty pool统计（8处）：umq_ub_jetty_pool.c:343,397,398,417,418,494,518-521
- qbuf pool统计（15处）：umq_qbuf_pool.c 的 exp_total_mem_pool_size, total_escape_buf_cnt, g_total_local_cap_*, g_total
- rx_consumed_load RELAXED load（1处）：umq_ub_flow_control.c:71 — lock_free=false分支辅助load

### 4.2 CAS循环初始load（7处） — 标准模式，正确

CAS循环初始load用RELAXED是正确模式：初始load是投机性读取减少无谓CAS开销，若值过时CAS失败重试，CAS本身（__ATOMIC_ACQ_REL）提供ordering。

- umq_ub_impl.c:384 — current_slot
- umq_ub_flow_control.c:79,117,134,162 — counter
- umq_ub_flow_control.c:336,372 — remote_rx_window

### 4.3 初始化/单线程/mutex保护上下文（3处）

- umq_ub_impl.c:869 — 初始化阶段单线程load
- umq_ub_impl.c:470 — mutex保护下store，mutex提供ordering
- umq_pro_ub.c:524 — prefill阶段累加

### 4.4 jetty_node_list.ref_cnt fetch_add（1处）

- umq_ub_jetty_pool.c:476 — 仅在node_list分配路径使用，后续操作在锁保护下完成，与482行ACQUIRE sub_fetch配对合理

---

## 5. 修复统计

| 类别 | 修复数量 | Commit |
|------|----------|--------|
| 高风险 | 14处 | ee6219eb |
| 中风险 | 5处 | ee6219eb |
| 低风险 | 4处 | e3be84ac |
| 不修复 | 69处 | — |
| **合计** | **23处修复 / 69处不修复 / 总计92处** | |

---

## 6. 架构影响说明

| 架构 | RELAXED影响 | 修复后效果 |
|------|-------------|------------|
| **x86 (TSO)** | RELAXED ≈ RELEASE(store) + ACQUIRE(load)，大多数场景不会暴露问题 | 无性能影响，仅增加代码正确性保障 |
| **ARM (弱序)** | RELAXED无ordering保证，store/load可被重排，上述高风险问题会真实暴露 | 修复后通过acquire-release配对确保跨线程可见性 |

**性能影响评估**：`__ATOMIC_ACQUIRE`/`__ATOMIC_RELEASE` 在ARM上仅插入轻量barrier（`dmb ish`），在x86上无额外指令开销（TSO已满足）。对性能影响极小，可忽略。
