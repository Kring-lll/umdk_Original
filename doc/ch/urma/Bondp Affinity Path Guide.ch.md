# urma_post_jetty_send_wr 在 bondp 场景的扩展字段用法与亲和路径选择说明

## 一、概述

在 bonding（聚合）设备场景下，`urma_post_jetty_send_wr` / `urma_post_jfs_wr` 支持应用通过 **WR 的驱动扩展字段（`has_drv_ext`）** 携带源/目的 chip id，从而在 **balance 负载均衡模式** 下指定亲和路径（affinity path），让特定业务流绑定到选定的 IODie / Port 物理链路上。

本文聚焦 `select_affinity_path` 的实现原理，并给出应用侧具体使用方案。

---

## 二、扩展字段的数据结构

### 2.1 基础 WR 与扩展标志位

`urma_jfs_wr_t`（见 `src/urma/lib/urma/core/include/urma_types.h`）是 jetty send / jfs 发送路径的统一 WR，本身不内嵌 chip id。扩展通过 **位标志 + 容器包容** 实现：

```c
typedef struct urma_jfs_wr {
    urma_opcode_t opcode;
    urma_jfs_wr_flag_t flag;       // 内含 has_drv_ext 位
    urma_target_jetty_t *tjetty;
    uint64_t user_ctx;
    union { urma_rw_wr_t rw; urma_send_wr_t send; urma_cas_wr_t cas; urma_faa_wr_t faa; };
    struct urma_jfs_wr *next;
} urma_jfs_wr_t;
```

`flag.bs.has_drv_ext` 置 1 表示该 WR 携带驱动扩展数据，bondp 据此把 WR 视为更大的 `bondp_jfs_wr_t`。

### 2.2 bondp 扩展 WR

bondp 通过 `CONTAINER_OF_FIELD` 把 `urma_jfs_wr_t` 作为 `base` 字段，在其后追加 chip id（见 `src/urma/lib/urma/bond/include/urma_ubagg.h`）：

```c
typedef struct bondp_jfs_wr {
    urma_jfs_wr_t base;   // 与 urma_jfs_wr_t 内存布局兼容
    uint32_t src_chip_id; // 源 chip id（1 或 2）
    uint32_t dst_chip_id; // 目的 chip id（1 或 2）
} bondp_jfs_wr_t;
```

**关键约束**：chip id 取值范围为 `BONDP_CHIP_ID_MIN(1)` ~ `BONDP_CHIP_ID_MAX(2)`（见 `src/urma/lib/urma/bond/bondp_datapath_schedule.h`），代表两颗 IODie。值会在发送校验阶段被检查（见 `src/urma/lib/urma/bond/bondp_datapath.c` 中 `post_send_check_valid`）。

---

## 三、调用链与扩展字段传递路径

```
urma_post_jetty_send_wr(jetty, wr, bad_wr)
  └─ bondp_post_jetty_send_wr                [bondp_datapath.c]
       ├─ post_send_check_wr_list_valid      // 校验每个 wr，含 chip_id 范围
       │    └─ post_send_check_valid
       │         └─ if (wr->flag.bs.has_drv_ext)
       │              CONTAINER_OF_FIELD → bondp_jfs_wr_t，校验 chip_id ∈ [1,2]
       │
       └─ bondp_post_send_wr_list_and_store / bondp_post_send_wr_no_store
            └─ schedule_send_wr(wr, bdp_comp, &send_idx, &target_idx)
                 ├─ if (!wr->flag.bs.has_drv_ext) → schedule_send(..., info=NULL)
                 └─ else CONTAINER_OF_FIELD → 取 src/dst_chip_id 构造 info
                      └─ schedule_send(tjetty, bdp_comp, &send_idx, &target_idx, &info)
                           └─ switch(bonding_mode)
                                └─ case BONDP_BONDING_MODE_BALANCE:
                                     schedule_send_balance(bdp_comp, bdp_tjetty, send_idx, target_idx, info)
                                          ├─ info != NULL → select_affinity_path(...)   ← 亲和路径
                                          ├─ 命中 0 条且未开 failover → URMA_FAIL
                                          ├─ 命中 0 条且开了 failover → 退化为全局最小负载选路
                                          │      select_least_load_path(...)
                                          └─ select_random_path(cnt) 在候选中随机选一条
```

**核心传递规则**：
- **未携带扩展**（`has_drv_ext=0`）：`info=NULL`，balance 模式走全局最小负载选路（无亲和约束）。
- **携带扩展**（`has_drv_ext=1`）：构造 `info={src_chip_id, dst_chip_id}`，进入 `select_affinity_path` 做亲和选路。

---

## 四、`select_affinity_path` 实现原理

`select_affinity_path`（见 `src/urma/lib/urma/bond/bondp_datapath_schedule.c`）按 **bonding level（聚合层级）** 分两种策略：

### 4.1 IODie 层级（`BONDP_BONDING_LEVEL_IODIE`）

构造 **3 条优先级递减的候选路由**（`CHIP_ROUTE_NUM=3`），通过 `init_chip_priority`：

| 优先级 | src_chip_id | dst_chip_id | 含义 |
|--------|-------------|-------------|------|
| 0（最高） | 应用指定 src | 应用指定 dst | **完全亲和**：源/目的都走指定 Die |
| 1 | 应用指定 src | switch(dst) | 源端亲和，目的端切换到对侧 Die（半亲和） |
| 2（最低） | switch(src) | 应用指定 dst | 源端切换，目的端亲和 |

`switch_iodie` 在 1↔2 之间切换。然后 `select_path_by_priority` 按优先级顺序遍历，找到第一条 **同时满足** 以下条件的路径即返回：

1. `bdp_comp->valid[local_idx]`（源端 Die 对应的 jetty 有效）；
2. `target_path_available`（目的端 Die 对应的 target jetty 有效且已建立）；
3. `p_jetty[local_idx] != NULL`（防并发 failback 重建期空指针）；
4. RC 模式下 `remote_jetty` 需与 `p_tjetty[local_idx][target_idx]` 一致（连接已绑定）。

**原理**：先尝试完全匹配应用指定的源/目的 Die；若该 Die 路径不可用（故障/未建链/负载满），自动降级到「源端亲和 + 目的对侧」或「源对侧 + 目的亲和」，保证尽量贴合应用的亲和意图。

### 4.2 Port 层级（`BONDP_BONDING_LEVEL_PORT`）

IODie 已确定（由 chip id 给出），在每颗 Die 的多个 Port 间选路。遍历 `ACTIVE_PORT_PER_CHIP(2)` 条 failover 路由：

```c
for (int i = 0; i < ACTIVE_PORT_PER_CHIP; i++) {
    if (old_path->local_idx > 0 && old_path->target_idx > 0 &&
        !is_in_failover_route(info, i, old_path)) {
        continue;   // 已有路径且不在当前 failover 路由组内 → 跳过（避免迁移）
    }
    select_path_by_chip(bdp_comp, bdp_tjetty, info, i, &least_load_path[...]);
}
```

`select_path_by_chip` 从全局 `failover_route[src][dst][route_id]` 表中取出该 Die 对的 `URMA_FAILOVER_LINK_NUM` 条物理链路，逐条检查有效性，命中即作为候选。

**`old_path` 的作用**：`schedule_send_balance` 在调用 `select_affinity_path` 前，若 `*send_idx/*target_idx >= 0`（来自上层传入的上次路径），会构造 `old_path`。Port 层级用它判断「当前路径是否还在亲和路由组内」，**避免无谓的路径迁移**——只有在原路径失效时才换到同组内的其他链路。

### 4.3 候选选取与最终决策

`select_affinity_path` 返回候选数 `least_load_cnt` 后，`schedule_send_balance` 处理：

1. **`least_load_cnt == 0` 且未开 failover** → `URMA_FAIL`（亲和路径全不可用，直接失败）；
2. **`least_load_cnt == 0` 且开了 failover** → 退化为全局最小负载选路 `select_least_load_path`，保证可用性优先；
3. **`least_load_cnt > 0`** → `select_random_path(cnt)` 在候选中随机选一条，实现同优先级路径间的负载分散。

候选路径的 `least_load` 字段记录 `sqe_cnt`（该路径上未完成的发送 WR 数），用于后续最小负载比较。

---

## 五、应用使用方案

### 5.1 推荐方式：直接调用 `urma_write_affinity`（RM 模式写操作）

bondp 提供了封装好的 affinity API（见 `src/urma/lib/urma/bond/include/urma_ubagg.h`，实现在 `src/urma/lib/urma/bond/bondp_datapath.c`），应用无需手动构造扩展 WR：

```c
urma_status_t urma_write_affinity(
    urma_jfs_t *jfs, urma_target_jetty_t *target_jfr,
    urma_target_seg_t *dst_tseg, urma_target_seg_t *src_tseg,
    uint64_t dst, uint64_t src, uint32_t len,
    urma_jfs_wr_flag_t flag, uint64_t user_ctx,
    uint32_t src_chip_id, uint32_t dst_chip_id);
```

内部自动：构造 `bondp_jfs_wr_t`、置 `base.flag.bs.has_drv_ext = 1`、填 `src/dst_chip_id`，再调用 `post_jfs_wr`。**应用只需指定 `src_chip_id`/`dst_chip_id`（1 或 2）**。

#### 示例：把一笔写操作绑定到「源 Die 1 → 目的 Die 1」亲和路径

```c
urma_jfs_wr_flag_t flag = {0};
flag.bs.signaled = 1;          // 需要完成事件
urma_status_t ret = urma_write_affinity(
    jfs,                       // 已创建的 jfs（bonding 设备上）
    target_jfr,                // 已 import 的远端 jfr
    dst_tseg, src_tseg,        // 目的/源内存段
    dst_addr, src_addr, len,
    flag, (uint64_t)wr_id,
    /*src_chip_id=*/1,          // 源端走 Die 1
    /*dst_chip_id=*/1);         // 目的端走 Die 1 → 完全亲和
```

### 5.2 jetty send 场景（RC 连接模式）

`urma_post_jetty_send_wr` 走同一 `bondp_post_jetty_send_wr` → `schedule_send_wr` 链路。若应用希望 jetty send 也带亲和，需**手动构造扩展 WR**（bondp 未提供 jetty 专用 helper）：

```c
bondp_jfs_wr_t bwr = {0};
urma_jfs_wr_t *base = &bwr.base;

base->opcode = URMA_OPC_SEND;          // 或 WRITE 等业务 opcode
base->flag.bs.has_drv_ext = 1;         // ★ 关键：声明携带扩展
base->user_ctx = (uint64_t)wr_id;
base->rw.src.num_sge = 1;
base->rw.src.sge = &src_sge;
base->tjetty = (urma_target_jetty_t *)target_jetty;
base->next = NULL;

bwr.src_chip_id = 1;                   // ★ 源 Die
bwr.dst_chip_id = 2;                   // ★ 目的 Die

urma_jfs_wr_t *bad_wr = NULL;
urma_status_t ret = urma_post_jetty_send_wr(jetty, base, &bad_wr);
```

**注意**：必须确保 `jetty` 创建在 bonding 设备上（设备名以 `bonding_dev` 前缀，见 `urma_is_bonding_dev`），且 bonding_mode 已配置为 `BONDP_BONDING_MODE_BALANCE`、bonding_level 为 `IODIE` 或 `PORT`，否则扩展字段不生效。

### 5.3 chip id 取值约定

| chip_id 值 | 含义 |
|-----------|------|
| 1 | IODie 0（第一颗） |
| 2 | IODie 1（第二颗） |

非法值（0 或 >2）会在 `post_send_check_valid` 阶段被拒绝并返回 `URMA_EINVAL`。

### 5.4 行为语义总结

| 场景 | has_drv_ext | balance 行为 |
|------|-------------|--------------|
| 普通 send | 0 | 全局最小负载选路，无亲和约束 |
| 亲和 send | 1 | 按 `select_affinity_path` 选 Die/Port 亲和路径 |
| 亲和路径全失效 + 未开 failover | 1 | `URMA_FAIL` |
| 亲和路径全失效 + 已开 failover | 1 | 退化为全局最小负载（保可用） |

---

## 六、设计原理小结

1. **零侵入扩展**：通过 `has_drv_ext` 位 + 容器包容，让基础 `urma_jfs_wr_t` 与扩展 `bondp_jfs_wr_t` 布局兼容，非 bonding 驱动不受影响。
2. **优先级降级选路**（IODie 层）：完全亲和 → 源端亲和 → 目的亲和，三档自动降级，兼顾亲和意图与可用性。
3. **failover 路由组约束**（Port 层）：用 `old_path` + `is_in_failover_route` 把路径迁移限制在同组内，避免跨组抖动。
4. **同优先级负载分散**：候选集非空时用 `select_random_path` 随机选，避免单条链路过载。
5. **可用性兜底**：亲和全失效时，是否退化由 `enable_failover` 开关决定——强亲和场景关闭 failover 即可让应用感知失败。
