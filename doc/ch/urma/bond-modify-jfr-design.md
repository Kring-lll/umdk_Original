# urma 聚合设备 JFR 接收阈值配置与限流事件端口上报 设计

| 项目 | 内容 |
| --- | --- |
| 仓库 | umdk（URMA） |
| 模块 | `src/urma/lib/urma/bond/` |
| 需求 | ① JFR 支持按**本端端口**配置接收阈值 `rx_threshold`；② `URMA_EVENT_JFR_LIMIT` 异步事件携带上报该事件的**本端端口** |
| 端口标识 | 复用 `bondp_port_id_t`（`urma_ubagg.h`） |
| 依赖类型 | `bondp_wr_hint_t`（规则见 [bond-port-wr-design.md](bond-port-wr-design.md) 第 2 章） |

## 目录

- [1. 需求与背景](#1-需求与背景)
- [2. 现状分析](#2-现状分析)
- [3. 接口设计](#3-接口设计)
- [4. 实现方案](#4-实现方案)
- [5. 影响面与兼容性](#5-影响面与兼容性)
- [6. 已知限制](#6-已知限制)
- [7. 测试建议](#7-测试建议)
- [8. 附录](#8-附录)

---

## 1. 需求与背景

### 1.1 应用场景（RNR 处理）

应用为消除 RNR（receiver not ready）带来的收包停顿，采用"配置水线 + 事件驱动补充 RQE"的流程：

```text
1) 应用调用 bondp_modify_jfr()，为指定的本端端口配置接收阈值 rx_threshold（水线）
2) 该端口物理 JFR 的可用 RQE 降到水线以下时，驱动上报 URMA_EVENT_JFR_LIMIT
3) 应用调用 bondp_get_async_event() 取到事件，并确认是哪个本端端口触限
4) 应用在该端口上及时 post recv WR，补充 RQE，避免对端因 RNR 重传
```

本设计的两个能力正是为该流程服务：

| 流程步骤 | 对应能力 |
| --- | --- |
| 步骤 1 | 按本端端口配置水线：`bondp_modify_jfr()` |
| 步骤 3 | 事件携带本端端口，使补充 RQE 能落到正确端口：`bondp_get_async_event()` |

### 1.2 范围

| 维度 | 范围 |
| --- | --- |
| 属性 | 仅接收阈值 `JFR_RX_THRESHOLD`，不支持配置 JFR 状态 |
| 端口方向 | 仅本端端口（hint 的 `SRC_*`） |
| 对象 | 仅 JFR；`modify_jetty` / `modify_jfs` 保持现状 |
| 事件 | 仅 `URMA_EVENT_JFR_LIMIT` 携带端口信息，其他事件类型不携带 |
| 接口 | 新增 `bondp_modify_jfr()` 与 `bondp_get_async_event()`；`urma_modify_jfr()` / `urma_get_async_event()` / `urma_ack_async_event()` 行为不变 |
| 模块边界 | 仅改动 `src/urma/lib/urma/bond/` 与接口文档；不修改 UMQ / URPC 等消费方，不修改公共结构体 |

### 1.3 目标

| 目标 | 说明 |
| --- | --- |
| 按本端端口配置水线 | 只对指定端口的 `p_jfr` 下发 `urma_modify_jfr()`，其他成员端口不受影响 |
| 一次调用拿到事件与端口 | 应用无需额外查询即可把补充 RQE 的动作落到触限端口 |
| 出参约定唯一可判空 | 端口出参总是被写入，用固定哨兵值表示"无端口信息" |
| 不改变既有行为 | 既有接口语义不变；既有消费方无需修改 |
| 事件不丢失、不阻塞 | 不引入事件暂存或过滤，避免未 ack 事件阻塞资源删除 |

### 1.4 非目标

- 不支持按端口配置 JFR 状态（`JFR_STATE`），不改变 JFR 状态机与接收调度。
- 不改变 bond 的接收端口选择：`schedule_recv()`、`post_recv` 的端口选择、`valid[]` 均不受影响。
- `URMA_EVENT_JFR_ERR`、`URMA_EVENT_JETTY_LIMIT`、`URMA_EVENT_JETTY_ERR` 等事件不携带端口信息。
- 不提供 `modify_jetty` / `modify_jfs` 的按端口版本。
- 不新增批量获取接口，不改变"一次取一个事件"的既有语义。

---

## 2. 现状分析

### 2.1 modify_jfr 现有实现

```text
应用: urma_modify_jfr(jfr, attr)                       # core/include/urma_api.h:320
  └─ urma_cp_api.c: urma_modify_jfr()                  # 参数校验 + ops 分发
       └─ ops->modify_jfr(jfr, attr)
            └─ bondp_provider_ops.c: .modify_jfr = bondp_modify_jfr_sched
                 └─ bondp_cp_jetty.c: bondp_modify_jfr_sched()       # 1243 行
                      for (i = 0; i < URMA_UBAGG_DEV_MAX_NUM; i++)
                          if (p_jfr[i] != NULL)
                              urma_modify_jfr(p_jfr[i], attr);        # 逐个物理 JFR
```

要点：遍历**全部**物理 JFR 逐个下发，不具备端口选择能力；失败不中断遍历，返回最后一次失败的错误码；无 `deleting` / 引用计数保护，也不加锁。

### 2.2 物理 JFR 布局与 Jetty 共享 JFR

| 项 | 说明 | 代码位置 |
| --- | --- | --- |
| `p_jfr[]` | 稀疏数组，仅 `enabled_indices[]` 中的下标非空；PORT 级 bonding 下下标可接近 `URMA_UBAGG_DEV_MAX_NUM` | `bondp_create_pjfr()`（`bondp_cp_jetty.c:998`） |
| `members[]` | 与 `p_jfr[]` 共用同一 union，`members[i] != NULL` 等价 `p_jfr[i] != NULL` | `bondp_types.h` 的 `bondp_comp_t` |
| `valid[]` | 创建时置 `true`；仅由发送侧故障 CR 清除（见 4.2.6） | `bondp_cp_jetty.c:1013` |
| 物理 JFR 的 `user_ctx` | 创建时被写成所属 bond JFR 组件指针 | `bondp_cp_jetty.c:1011` |

聚合设备要求 Jetty 使用共享 JFR，创建物理 Jetty 时按端口取该端口自己的物理 JFR（`bondp_cp_jetty.c:1377`）：

```c
p_cfg.shared.jfr = bdp_jfr->p_jfr[i];   /* i 为该 Jetty 的成员端口 */
```

因此按端口配置水线会同时影响**该端口上**引用它的 Jetty 接收侧。

### 2.3 查询的聚合语义

`bondp_query_jfr()`（`bondp_cp_jetty.c:1272`）对全部物理 JFR 做聚合：`state` 只要有一个端口为 `READY` 就返回 `READY`；`rx_threshold` 返回所有非 0 值中的**最小值**。即按端口配置后，查询接口只能给出聚合结果，无法读出单个端口的配置值。

### 2.4 异步事件链路与端口信息丢失

```text
内核（uburma）上报 async event
  └─ 物理 ctx 的 async_fd 可读（bond 把各物理 ctx 的 async_fd 聚合到自己的 epoll fd，
     epoll_event.data.ptr = 物理 ctx，bondp_provider_ops.c:203-210、359-372）
  └─ bondp_get_async_event_sched(ctx, event)                # bondp_dp_interrupt.c:242
       ├─ epoll_wait(ctx->async_fd, ...)                    # 超时 0
       ├─ urma_get_async_event(物理 ctx, p_event)
       │     └─ urma_cmd_get_async_event()                  # urma_cmd.c:2602
       │           ioctl 取回 {event_type, event_data}
       │           JFR 类事件：element.jfr = (urma_jfr_t *)event_data   # 物理 JFR 指针
       ├─ init_elment_vjetty(v_event, p_event)              # 物理对象 → 虚拟对象
       └─ v_event->priv = p_event                           # 物理事件暂存，供 ack 转发与释放
  └─ 应用：urma_get_async_event() 取事件，urma_ack_async_event() 应答
```

映射阶段只输出虚拟对象指针（对 JFR 类事件读取物理 JFR 的 `jfr_cfg.user_ctx` 得到虚拟 JFR），**没有任何端口标识**：应用无法区分是哪个成员端口触限。

### 2.5 端口信息的可推导性

端口信息可在 bond 层用户态推导，无需内核/驱动配合：

| 事实 | 代码位置 |
| --- | --- |
| 创建 JFR 时把用户态对象指针交给内核，事件回传同一指针 | `urma_cmd.c:734`：`arg.in.urma_jfr = (uint64_t)(void *)jfr; /* for async event */` |
| 事件中的 `element.jfr` 即物理 `urma_jfr_t*` | `urma_cmd.c:2622` |
| 物理 JFR 的 `jfr_cfg.user_ctx` 指向所属 bond JFR 组件 | `bondp_cp_jetty.c:1011` |
| 每个端口的物理 JFR 指针保存在 `bdp_jfr->p_jfr[i]` | `bondp_cp_jetty.c:1012` |

### 2.6 现状局限

1. 无法只对单个本端端口调整 `rx_threshold`——本设计要解决的问题。
2. `URMA_EVENT_JFR_LIMIT` 无法定位触限端口，导致补充 RQE 的动作无法精确落到该端口——本设计要解决的问题。
3. 无法只对单个端口做 drain / 局部复位（不在本设计范围）。

---

## 3. 接口设计

### 3.1 新增接口与命名约定

两个接口均声明在公共头文件 `bondp_api.h`：

```c
/* ① 按本端端口配置接收阈值 */
urma_status_t bondp_modify_jfr(urma_jfr_t *jfr, urma_jfr_attr_t *attr,
                               bondp_wr_hint_t *hint);

/* ② 获取异步事件并返回其对应本端端口 */
urma_status_t bondp_get_async_event(urma_context_t *ctx, urma_async_event_t *event,
                                    bondp_port_id_t *port_id);
```

命名约定（ops 入口带 `_sched` 后缀，带附加能力的对外接口使用不带后缀的名字）：

| 角色 | 函数名 | 说明 |
| --- | --- | --- |
| 对外接口 | `bondp_modify_jfr(jfr, attr, hint)` | 带 hint，按端口配置水线 |
| ops 入口 | `bondp_modify_jfr_sched(jfr, attr)` | 全端口实现，挂载到 `g_bond_ops.modify_jfr` |
| 对外接口 | `bondp_get_async_event(ctx, event, port_id)` | 带端口出参 |
| ops 入口 | `bondp_get_async_event_sched(ctx, event)` | 挂载到 `g_bond_ops.get_async_event`，供 `urma_get_async_event()` 调用；不返回端口信息 |

两个名字（`bondp_modify_jfr`、`bondp_get_async_event`）此前由 2 参的 ops 实现占用；C 语言不支持重载，因此原实现分别定名为 `bondp_modify_jfr_sched`、`bondp_get_async_event_sched`（与 `bondp_post_*_sched` 的处理方式一致）。

### 3.2 bondp_modify_jfr

原型与注释：

```c
/**
 * 按本端端口配置 bond JFR 的接收阈值。
 * @param[in] jfr:  bond 上下文中的 JFR 对象；
 * @param[in] attr: 待修改属性；要求 attr->mask == JFR_RX_THRESHOLD；
 * @param[in] hint: 路径提示；仅支持本端端口（SRC_*），远端端口位必须为 0；
 *                  mask == 0 表示应用不指定端口，作用范围为全部成员端口；
 * @return URMA_SUCCESS 成功；其他值见 3.2.4
 */
urma_status_t bondp_modify_jfr(urma_jfr_t *jfr, urma_jfr_attr_t *attr,
                               bondp_wr_hint_t *hint);
```

#### 3.2.1 参数约束

| 参数 | 约束 |
| --- | --- |
| `jfr` | 非空；上下文必须属于 bond 设备（`bondp_is_bond_ctx()`）；对象类型必须为 `BONDP_COMP_JFR` |
| `attr` | 非空；`attr->mask` 必须等于 `JFR_RX_THRESHOLD`；阈值合法性由底层校验（`rx_threshold <= 该端口 JFR 深度 wqe_cnt`，`udma_verify_modify_jfr()`） |
| `hint` | 非空；`version` / `size` / `mask` 规则见 3.2.2 |

#### 3.2.2 hint 规则

| 项 | 规则 | 说明 |
| --- | --- | --- |
| 必选位 | `SRC_CHIP_ID` + `SRC_PORT_ID` | 至少明确"哪颗芯片的哪个端口" |
| 可选位 | `SRC_DIE_ID` | `die_id` 非强制：未置位时不校验（按缺省 1 处理），置位时必须为 1 |
| 必须为 0 | 全部 `DST_*` | JFR 是本地接收对象，不存在远端端口语义 |
| `mask == 0` | 应用不指定端口，作用范围为全部成员端口 | 与 `urma_modify_jfr()` 行为一致 |
| `version` / `size` | 必须为 `BONDP_WR_HINT_VERSION_V0` 与约定大小 | 结构体版本化与防越界读取 |

`src_id` 语义沿用 `bondp_port_id_t`：`chip_id ∈ [1, CHIP_NUM]`、`port_idx ∈ [0, PORT_NUM]` 或 `UINT8_MAX`（primary EID）。

#### 3.2.3 使用示例

```c
urma_jfr_attr_t attr = {0};
attr.mask = JFR_RX_THRESHOLD;
attr.rx_threshold = 16;                   /* 必须 <= 该端口 JFR 深度 */

bondp_wr_hint_t hint = {0};
hint.version = BONDP_WR_HINT_VERSION_V0;
hint.size    = sizeof(bondp_wr_hint_t);
hint.mask    = BONDP_WR_HINT_MASK_SRC_CHIP_ID |   /* 必选 */
               BONDP_WR_HINT_MASK_SRC_PORT_ID;    /* 必选；SRC_DIE_ID 可省 */
hint.src_id.bs.chip_id  = 1;
hint.src_id.bs.port_idx = UINT8_MAX;      /* primary EID */

urma_status_t st = bondp_modify_jfr(jfr, &attr, &hint);
```

#### 3.2.4 返回值与错误码

| 场景 | 返回 |
| --- | --- |
| 成功 | `URMA_SUCCESS` |
| `jfr` / `attr` / `hint` 为空、上下文非 bond、对象类型不符 | `URMA_EINVAL` |
| `attr->mask` 不含 `JFR_RX_THRESHOLD`，或含 `JFR_STATE` 等其他位（仅单端口路径校验） | `URMA_EINVAL` |
| `hint->version` / `size` 非法、`mask` 含 `DST_*`、缺少 `SRC_CHIP_ID` 或 `SRC_PORT_ID`、`SRC_DIE_ID` 置位但 `die_id != 1` | `URMA_EINVAL` |
| `src_id` 坐标非法，或指定端口不是该 JFR 成员（`p_jfr[idx] == NULL`） | `URMA_EINVAL` |
| `rx_threshold` 超过该端口 JFR 深度（底层校验） | 透传底层 `URMA_EINVAL` |
| 对象正在删除（`deleting == true`） | `URMA_EAGAIN` |
| 底层 `urma_modify_jfr(p_jfr[idx], attr)` 失败 | 透传底层错误码 |

单端口路径不做"失败后尝试其他端口"：只有被指定的端口会被修改。

### 3.3 bondp_get_async_event

原型与注释：

```c
/**
 * 获取 bond 上下文的异步事件，并返回该事件对应的本端端口信息。
 * @param[in]  ctx:     bond 设备的上下文；
 * @param[out] event:   异步事件，语义与 urma_get_async_event() 一致；
 * @param[out] port_id: 事件对应的本端端口；仅当 event->event_type 为
 *                      URMA_EVENT_JFR_LIMIT 时可能有效，其余情况或无法判定时
 *                      写入 UINT16_MAX；不可为 NULL；
 * @return URMA_SUCCESS 成功；其他值见 3.3.3
 */
urma_status_t bondp_get_async_event(urma_context_t *ctx, urma_async_event_t *event,
                                    bondp_port_id_t *port_id);
```

#### 3.3.1 出参约定

`port_id` 为必填出参，**每次调用都会被写入一次**：

| 场景 | `*port_id` |
| --- | --- |
| `URMA_EVENT_JFR_LIMIT` 且成功解析出端口 | 该端口的 `bondp_port_id_t` |
| `URMA_EVENT_JFR_LIMIT` 但无法解析出端口 | `UINT16_MAX` |
| 其他事件类型（`JFR_ERR`、`JETTY_LIMIT`、`JETTY_ERR`、`JFC_ERR`、`JFS_ERR`、`PORT_*`、`DEV_FATAL` 等） | `UINT16_MAX` |
| 接口返回失败（含当前无事件可读） | `UINT16_MAX` |

哨兵取值：`bondp_port_id_t.value == 0` 不是合法端口（`chip_id` 从 1 开始），但 `0` 已被 `BONDP_RECV_PORT_UNAVAILABLE` 用作 CQE 立即数端口字段的"不可用"约定；本接口与公开访问器 `bondp_get_cr_local_port_id()` 保持一致，统一用 **`UINT16_MAX`** 表示"无端口信息"（`bondp_cp_jetty.c:54`）。

#### 3.3.2 使用示例

```c
urma_async_event_t event = {0};
bondp_port_id_t port_id = {0};

urma_status_t st = bondp_get_async_event(ctx, &event, &port_id);
if (st == URMA_SUCCESS) {
    if (event.event_type == URMA_EVENT_JFR_LIMIT && port_id.value != UINT16_MAX) {
        /* port_id.bs.chip_id / die_id / port_idx 即触限的本端端口 */
    }
    urma_ack_async_event(&event);      /* 应答接口不变 */
}
```

#### 3.3.3 返回值与错误码

| 场景 | 返回 |
| --- | --- |
| 成功取到一个异步事件 | `URMA_SUCCESS` |
| `ctx` / `event` / `port_id` 为 NULL、或 `ctx` 不是 bond 上下文、或 `ctx->async_fd < 0` | `URMA_EINVAL` |
| 当前没有待处理事件，或 epoll 上报错误 | `URMA_FAIL`（与现有实现一致：无事件也返回 `URMA_FAIL`） |
| 物理事件对象分配失败 | `URMA_ENOMEM` |
| 底层 `urma_get_async_event()` 失败 | 透传底层错误码 |
| 物理对象无法映射为虚拟对象（如 `user_ctx` 为 0） | `URMA_EINVAL`（与现有实现一致） |

返回值语义与现有 ops 入口完全一致，本接口只是多一个端口出参。

### 3.4 与既有接口的关系

| 接口 | 行为 |
| --- | --- |
| `urma_modify_jfr(jfr, attr)` | 保持不变：经 ops 调用 `bondp_modify_jfr_sched()`，作用于全部成员端口 |
| `bondp_modify_jfr(jfr, attr, hint)` | `mask != 0` 时作用于指定端口；`mask == 0` 时等价于 `urma_modify_jfr()` |
| `bondp_get_async_event(ctx, event, port_id)` | 超集语义：可返回任意事件类型，仅 `JFR_LIMIT` 时端口有意义 |
| `urma_get_async_event(ctx, event)` | 保持不变：返回同样的事件，但不提供端口信息 |
| `urma_ack_async_event(event)` | 保持不变：按 `event->urma_ctx->ops` 分发到 bond 的 ack 实现 |

设计约束：`bondp_get_async_event()` 不做事件类型过滤、不在库内暂存事件。事件一旦从内核取走就必须由应用 ack；若库内扣留（或过滤掉）某条事件而不交给应用，`urma_cmd_delete_jfr()` 中的 `wait_async_event_ack()`（`urma_cmd.c:1017`、`:118`）会一直等待 `acked == reported`，导致删除阻塞。因此"新接口只返回 `JFR_LIMIT`、其余类型交给旧接口"的分工方式不采用。

### 3.5 RNR 处理完整示例

```c
/* 步骤 1：为本端端口配置水线 */
urma_jfr_attr_t attr = {0};
attr.mask = JFR_RX_THRESHOLD;
attr.rx_threshold = 16;
(void)bondp_modify_jfr(jfr, &attr, &hint);       /* hint 见 3.2.3 */

/* 步骤 2/3：事件循环，识别触限端口 */
for (;;) {
    urma_async_event_t event = {0};
    bondp_port_id_t port_id = {0};
    if (bondp_get_async_event(ctx, &event, &port_id) != URMA_SUCCESS) {
        continue;
    }
    if (event.event_type == URMA_EVENT_JFR_LIMIT && port_id.value != UINT16_MAX) {
        /* 步骤 4：在该端口补充 RQE（post recv WR），避免对端 RNR 重传 */
    }
    urma_ack_async_event(&event);
}
```

---

## 4. 实现方案

### 4.1 端口标识与下标映射（公共约定）

| 约定 | 说明 | 代码位置 |
| --- | --- | --- |
| 端口标识 | `bondp_port_id_t`：`chip_id ∈ [1, CHIP_NUM]`、`die_id` 固定 `1`、`port_idx ∈ [0, PORT_NUM]` 或 `UINT8_MAX`（primary EID） | `urma_ubagg.h` |
| 下标 → 端口 | `bondp_active_index_to_port_id()`，与 `BONDP_USER_CTL_QUERY_PORT_STATUS` 使用同一套映射 | `bondp_cp_jetty.c:588`（声明于 `bondp_cp_tjetty.h:74`） |
| 端口 → 下标 | `convert_bond_port_id_to_active_index()`，校验坐标合法性与物理上下文存在 | `bondp_cp_jetty.c:543` |
| 无效端口哨兵 | `UINT16_MAX`（`value == 0` 不是合法端口，但 `0` 已用于 CQE 立即数的"不可用"约定） | `bondp_cp_jetty.c:54` |

### 4.2 bondp_modify_jfr 实现

#### 4.2.1 总体流程

```text
bondp_modify_jfr(jfr, attr, hint)
  +- 参数与对象校验（jfr / attr / hint、bond 上下文、comp_type）
  +- 控制面并发防护：use_cnt++，若 deleting 则 use_cnt-- 并返回 URMA_EAGAIN
  +- hint 的 version / size 校验
  +- mask == 0 ?
  |    +- 是 -> ret = bondp_modify_jfr_sched(jfr, attr)   # 全端口遍历，不额外限制 attr
  |    +- 否 -> 继续
  +- attr 校验（要求 mask == JFR_RX_THRESHOLD）
  +- hint 方向位与坐标校验（3.2.2 的规则）
  +- convert_bond_port_id_to_active_index(ctx, hint->src_id, &idx)
  +- idx 越界或 p_jfr[idx] == NULL -> URMA_EINVAL
  +- ret = urma_modify_jfr(p_jfr[idx], attr)
  +- 释放引用（use_cnt--），return ret
```

`mask == 0` 分支不额外限制 `attr->mask`，以保证与既有 `urma_modify_jfr()` 行为完全一致（非法 `attr` 由底层返回错误）；`JFR_RX_THRESHOLD` 的限定只作用于单端口路径。

#### 4.2.2 hint 解析 helper

```c
/* 校验 hint 的 version/size/mask，并把本端端口解析为 active index。
 * 要求 SRC_CHIP_ID 与 SRC_PORT_ID 置位，SRC_DIE_ID 可选（缺省 1）；
 * hint->mask == 0 由调用方单独处理。 */
static urma_status_t bondp_hint_parse_local_idx(const bondp_context_t *bdp_ctx,
                                                const bondp_wr_hint_t *hint,
                                                uint32_t *local_idx);
```

| 步骤 | 内容 | 失败返回 |
| --- | --- | --- |
| 1 | `hint != NULL`、`version == BONDP_WR_HINT_VERSION_V0`、`size` 合法 | `URMA_EINVAL` |
| 2 | `mask` 中无 `DST_*` 位 | `URMA_EINVAL` |
| 3 | `mask` 含 `SRC_CHIP_ID` 与 `SRC_PORT_ID`；置位 `SRC_DIE_ID` 时 `die_id` 必须为 1 | `URMA_EINVAL` |
| 4 | `convert_bond_port_id_to_active_index(bdp_ctx, hint->src_id, local_idx)` | `URMA_EINVAL` |

该 helper 与 `bondp_post_recv_wr_by_port()` 的现有校验（`bondp_datapath.c:1336`）语义一致，可供后续 `modify_jetty` / `modify_jfs` 或 `post_*` 复用。

#### 4.2.3 接口实现

```c
urma_status_t bondp_modify_jfr(urma_jfr_t *jfr, urma_jfr_attr_t *attr,
                               bondp_wr_hint_t *hint)
{
    if (jfr == NULL || jfr->urma_ctx == NULL || attr == NULL || hint == NULL) {
        return URMA_EINVAL;
    }
    if (!bondp_is_bond_ctx(jfr->urma_ctx)) {
        URMA_LOG_ERR("bondp_modify_jfr is only supported on bond JFR.\n");
        return URMA_EINVAL;
    }
    bondp_comp_t *bdp_jfr = CONTAINER_OF_FIELD(jfr, bondp_comp_t, v_jfr);
    if (bdp_jfr->comp_type != BONDP_COMP_JFR) {
        return URMA_EINVAL;
    }

    /* 控制面并发防护：与 bondp_delete_jfr() 的 deleting + use_cnt 机制互斥 */
    atomic_fetch_add(&bdp_jfr->use_cnt.atomic_cnt, 1);
    if (atomic_load(&bdp_jfr->deleting)) {
        atomic_fetch_sub(&bdp_jfr->use_cnt.atomic_cnt, 1);
        return URMA_EAGAIN;
    }

    urma_status_t result = URMA_SUCCESS;

    if (hint->mask == 0) {
        result = bondp_modify_jfr_sched(jfr, attr);
        goto OUT;
    }

    /* 只支持按端口配置接收阈值 */
    if (attr->mask != JFR_RX_THRESHOLD) {
        URMA_LOG_ERR("Only JFR_RX_THRESHOLD is supported by bondp_modify_jfr.\n");
        result = URMA_EINVAL;
        goto OUT;
    }

    uint32_t idx = 0;
    result = bondp_hint_parse_local_idx(bdp_jfr->bondp_ctx, hint, &idx);
    if (result != URMA_SUCCESS) {
        goto OUT;
    }
    if (idx >= URMA_UBAGG_DEV_MAX_NUM || bdp_jfr->p_jfr[idx] == NULL) {
        URMA_LOG_ERR("Port is not a member of this jfr, idx=%u, jfr_id=%u\n",
                     idx, bdp_jfr->v_jfr.jfr_id.id);
        result = URMA_EINVAL;
        goto OUT;
    }
    result = urma_modify_jfr(bdp_jfr->p_jfr[idx], attr);

OUT:
    atomic_fetch_sub(&bdp_jfr->use_cnt.atomic_cnt, 1);
    return result;
}
```

要点：复用 `bondp_is_bond_ctx()` 做上下文校验；成员判定用 `p_jfr[idx] == NULL`（与 `post_recv` 用 `members[idx] == NULL` 等价，同一 union）；不校验 `valid[idx]`（见 4.2.6）；所有出口统一释放引用。

#### 4.2.4 `mask == 0` 分支

直接调用 `bondp_modify_jfr_sched(jfr, attr)`，保持"遍历全部非空 `p_jfr`、失败不中断、返回最后一次错误"的既有语义，不引入第二套全端口实现。

#### 4.2.5 并发与生命周期

删除流程（`bondp_delete_jfr()`，`bondp_cp_jetty.c:1188`）置 `deleting = true` 并检查 `use_cnt`：若 `use_cnt > 0` 则回滚 `deleting` 并返回 `URMA_EAGAIN`，由调用方重试删除。

| 场景 | 方案 |
| --- | --- |
| 与 `urma_delete_jfr()` 并发 | 入口先 `use_cnt++`，再判 `deleting`；正在删除则撤销引用并返回 `URMA_EAGAIN`；所有出口统一释放引用 |
| 与其他 modify 并发 | 不引入锁；`use_cnt` 只解决与删除的竞争，modify 之间的互斥由调用方保证 |
| 与数据面并发 | 只操作物理 JFR 对象，不新增同步 |

#### 4.2.6 与 valid[] / 接收路径的关系

本接口只配置 `rx_threshold`，属纯控制面操作：不校验 `valid[]`，不触碰 `schedule_recv()`、`sqe_cnt`、`rqe_cnt`。

对 JFR 对象，`valid[]` 创建时置 `true` 后不会被清除（仅由发送侧故障 CR 路径 `handle_send_cr_with_store()` 清除，`bondp_datapath.c:1751`；failback 只重建 Jetty 的 `p_jetty`）。因此 `valid[idx]` 校验对 JFR 不产生行为差异；端口物理故障时 `p_jfr[idx]` 对象仍存在，阈值配置的成败由底层决定，错误码直接透传。

### 4.3 bondp_get_async_event 实现

#### 4.3.1 总体流程

```text
bondp_get_async_event(ctx, event, port_id)              # 对外接口
  +- ctx / event / port_id 非空校验、bond 上下文校验
  +- *port_id = UINT16_MAX                              # 出参总被写入
  +- bondp_get_async_event_internal(ctx, event, port_id)
       +- epoll_wait(ctx->async_fd)                     # 与现有实现一致
       +- urma_get_async_event(物理 ctx, p_event)       # 取物理事件
       +- init_elment_vjetty(v_event, p_event, port_id) # 映射 + 可选端口填充
       +- v_event->priv = p_event

bondp_get_async_event_sched(ctx, event)                 # ops 入口
  +- bondp_get_async_event_internal(ctx, event, NULL)   # 不填充端口
```

#### 4.3.2 端口推导

```c
/* 由物理 jfr 反查其端口；查询失败返回 UINT16_MAX（无效端口） */
static bondp_port_id_t bondp_pjfr_to_port_id(const urma_jfr_t *p_jfr)
{
    bondp_port_id_t invalid = {.value = UINT16_MAX};
    if (p_jfr == NULL) {
        return invalid;
    }
    /* 物理 JFR 的 user_ctx 在创建时被写成 bond JFR 组件指针 */
    bondp_comp_t *bdp_jfr = (bondp_comp_t *)(uintptr_t)p_jfr->jfr_cfg.user_ctx;
    if (bdp_jfr == NULL) {
        return invalid;
    }
    for (uint32_t i = 0; i < URMA_UBAGG_DEV_MAX_NUM; ++i) {
        if (bdp_jfr->p_jfr[i] == p_jfr) {
            return bondp_active_index_to_port_id(i);   /* 与按端口状态查询同一映射 */
        }
    }
    return invalid;
}
```

要点：仅在 `URMA_EVENT_JFR_LIMIT` 上执行；复杂度 `O(URMA_UBAGG_DEV_MAX_NUM)` 指针比较，异步事件频率低，开销可忽略；`p_jfr[]` 为稀疏数组，未使用下标为 NULL，不会误命中。

#### 4.3.3 内部实现结构

```c
/* 内部实现：取一个事件并完成物理 → 虚拟映射；port_id 为可选出参 */
static urma_status_t bondp_get_async_event_internal(urma_context_t *ctx, urma_async_event_t *event,
                                                    bondp_port_id_t *port_id);

/* ops 入口：g_bond_ops.get_async_event，供 urma_get_async_event() 调用 */
urma_status_t bondp_get_async_event_sched(urma_context_t *ctx, urma_async_event_t *event)
{
    return bondp_get_async_event_internal(ctx, event, NULL);
}

/* 对外接口：附带端口信息 */
urma_status_t bondp_get_async_event(urma_context_t *ctx, urma_async_event_t *event,
                                    bondp_port_id_t *port_id)
{
    if (ctx == NULL || event == NULL || port_id == NULL) {
        return URMA_EINVAL;
    }
    if (!bondp_is_bond_ctx(ctx)) {
        URMA_LOG_ERR("bondp_get_async_event is only supported on bond context.\n");
        return URMA_EINVAL;
    }
    port_id->value = UINT16_MAX;   /* 出参总被写入：默认无端口信息 */
    return bondp_get_async_event_internal(ctx, event, port_id);
}
```

映射函数增加可选出参，并把 `JFR_LIMIT` 与 `JFR_ERR` 分支拆开（其余分支不变）：

```c
static int init_elment_vjetty(urma_async_event_t *v_event, urma_async_event_t *p_event,
                              bondp_port_id_t *port_id)   /* port_id 可为 NULL */
{
    switch (p_event->event_type) {
        case URMA_EVENT_JFR_ERR:
            v_event->element.jfr = (urma_jfr_t *)get_jetty_and_ret(
                p_event->element.jfr->jfr_cfg.user_ctx, &ret);
            break;   /* 不提供端口信息，保持哨兵 */
        case URMA_EVENT_JFR_LIMIT:
            v_event->element.jfr = (urma_jfr_t *)get_jetty_and_ret(
                p_event->element.jfr->jfr_cfg.user_ctx, &ret);
            if (port_id != NULL) {
                *port_id = bondp_pjfr_to_port_id(p_event->element.jfr);
            }
            break;
        default:
            /* 其余分支与现有实现一致，不填充端口 */
            break;
    }
    return ret;
}
```

#### 4.3.4 事件类型与端口填充

| 事件类型 | `element` 映射来源 | 是否填充端口 |
| --- | --- | --- |
| `URMA_EVENT_JFR_LIMIT` | 物理 JFR 的 `jfr_cfg.user_ctx` → 虚拟 JFR | 是（解析失败时保持 `UINT16_MAX`） |
| `URMA_EVENT_JFR_ERR` | 同上 → 虚拟 JFR | 否 |
| `URMA_EVENT_JETTY_LIMIT` / `URMA_EVENT_JETTY_ERR` | 物理 Jetty 的 `jetty_cfg.user_ctx` → 虚拟 Jetty | 否 |
| `URMA_EVENT_JFC_ERR` / `JFS_ERR` / `JETTY_GRP_ERR` | 各自物理对象 | 否 |
| `URMA_EVENT_PORT_ACTIVE` / `PORT_DOWN` / `DEV_FATAL` / `EID_CHANGE` | 非对象类事件 | 否 |

### 4.4 改动清单

| 文件 | 改动 |
| --- | --- |
| `src/urma/lib/urma/bond/include/bondp_api.h` | 新增 `bondp_modify_jfr()` 与 `bondp_get_async_event()` 声明及注释（含哨兵约定） |
| `src/urma/lib/urma/bond/bondp_cp_jetty.c` | 现有全端口实现定名 `bondp_modify_jfr_sched()`；新增 `bondp_hint_parse_local_idx()` 与 3 参 `bondp_modify_jfr()` |
| `src/urma/lib/urma/bond/bondp_cp_jetty.h` | 声明 `bondp_modify_jfr_sched()`；3 参接口如需跨模块调用则另加声明 |
| `src/urma/lib/urma/bond/bondp_dp_interrupt.c` | 现有实现改为 `bondp_get_async_event_internal()`（新增可选端口出参）；新增 ops 入口 `bondp_get_async_event_sched()` 与对外 `bondp_get_async_event()`；`init_elment_vjetty()` 增加可选出参并拆分 `JFR_ERR` / `JFR_LIMIT`；新增 `bondp_pjfr_to_port_id()`；新增 include：`bondp_cp_tjetty.h`、`bondp_provider_ops.h` |
| `src/urma/lib/urma/bond/bondp_dp_interrupt.h` | 声明改名为 `bondp_get_async_event_sched()`；`bondp_ack_async_event()` 不变 |
| `src/urma/lib/urma/bond/bondp_provider_ops.c` | `.modify_jfr = bondp_modify_jfr_sched`、`.get_async_event = bondp_get_async_event_sched` |
| `doc/ch/urma/URMA API Guide.ch.md`、`doc/en/urma/URMA API Guide.md` | 新增两个接口章节：原型、参数、返回值、哨兵约定、示例 |
| `doc/ch/urma/bond-modify-jfr-design.md` | 本文档 |
| `umdk.spec` / CMake / bazel | 无需改动（`bondp_api.h` 已在安装与打包清单中） |

前置依赖：`bondp_wr_hint_t` 类型与配套宏先落地（替换说明见 [bond-port-wr-design.md](bond-port-wr-design.md)）。

---

## 5. 影响面与兼容性

| 维度 | 结论 |
| --- | --- |
| 现有 API | `urma_modify_jfr()` / `urma_get_async_event()` / `urma_ack_async_event()` 行为与调用链不变；其 ops 入口定名为 `bondp_modify_jfr_sched()` / `bondp_get_async_event_sched()` |
| 符号变化 | `bondp_modify_jfr` 与 `bondp_get_async_event` 由新的对外接口占用（C 无重载）；原 2 参实现分别改名加 `_sched` 后缀。三者此前只声明在未安装的内部头（`bondp_cp_jetty.h` / `bondp_dp_interrupt.h`），对外无兼容承诺 |
| 新增符号 | `bondp_modify_jfr`（3 参）、`bondp_get_async_event`（3 参），随 `bondp_api.h` 一起发布 |
| 公共结构体 | `urma_async_event_t`、`urma_jfr_t` 均不变，无 ABI 变更 |
| 数据面路径 | 不涉及 `post_send` / `post_recv` 代码路径，无性能影响 |
| 接收调度 | 无调度侧改动：`schedule_recv()`、`valid[]`、健康检查与 failover/failback 均不变 |
| 控制面并发 | `bondp_modify_jfr()` 与 `urma_delete_jfr()` 通过 `use_cnt` + `deleting` 互斥 |
| 事件可靠性 | 事件不丢弃、不暂存，全部交给应用 ack；不出现"事件被扣留导致删除阻塞" |
| 性能 | 仅 `JFR_LIMIT` 事件增加一次 `O(URMA_UBAGG_DEV_MAX_NUM)` 指针比较 |
| 消费方 | UMQ（`umq_ub_impl.c:2719`）、URPC framework（`provider_ops_jetty.c:397`）继续使用传统接口，本期不改动 |

---

## 6. 已知限制

1. 既有 `urma_modify_jfr()`（ops 入口 `bondp_modify_jfr_sched()`）不带并发防护，与 `urma_delete_jfr()` 并发时的窗口保留现状；本设计只保证新增接口具备防护。
2. `urma_query_jfr()` 只返回聚合值（`rx_threshold` 取所有端口的最小非零值），不具备按端口读取能力。
3. 按端口配置作用于物理 JFR，会连带影响该端口物理 Jetty 的接收侧。
4. 仅 `URMA_EVENT_JFR_LIMIT` 提供端口信息；`JFR_ERR` / `JETTY_LIMIT` / `JETTY_ERR` 不提供；应用继续使用 `urma_get_async_event()` 时无端口信息。
5. 事件路径读取物理 JFR 的 `user_ctx` 与 `p_jfr[]` 存在既有并发竞态（与删除并发时），本期不引入额外同步机制。
6. 不提供批量获取接口，与现有"一次取一个事件"的语义保持一致。

---

## 7. 测试建议

### 7.1 阈值配置（单元测试）

| 用例 | 预期 |
| --- | --- |
| `jfr` / `attr` / `hint` 为 NULL、上下文非 bond、`attr->mask` 含 `JFR_STATE` 或不含 `JFR_RX_THRESHOLD` | `URMA_EINVAL` |
| `hint->version` / `size` 非法、`mask` 含 `DST_*`、缺 `SRC_CHIP_ID` 或 `SRC_PORT_ID` | `URMA_EINVAL` |
| `mask` 置 `SRC_CHIP_ID` + `SRC_PORT_ID`、不置 `SRC_DIE_ID` | 正常生效（`die_id` 按缺省 1 处理） |
| `mask` 置 `SRC_DIE_ID` 且 `die_id = 2` | `URMA_EINVAL` |
| `src_id` 坐标越界，或端口不是该 JFR 成员 | `URMA_EINVAL` |
| `hint->mask == 0` | 与 `urma_modify_jfr()` 等价（全部端口生效） |
| 合法单端口 + `JFR_RX_THRESHOLD` | 只有该端口物理 JFR 阈值变化，其他端口不变 |
| `rx_threshold` 大于该端口 JFR 深度 | 透传底层 `URMA_EINVAL` |
| 对象正在删除（`deleting == true`）时调用 | `URMA_EAGAIN` |

### 7.2 事件端口（单元测试）

| 用例 | 预期 |
| --- | --- |
| `ctx` / `event` / `port_id` 任一为 NULL、上下文非 bond | `URMA_EINVAL` |
| 当前无事件可读 | `URMA_FAIL`，`port_id == UINT16_MAX` |
| 事件为 `URMA_EVENT_JFR_LIMIT` 且端口可解析 | `URMA_SUCCESS`，`port_id` 的 `chip_id` / `die_id` / `port_idx` 与实际端口一致 |
| 事件为 `URMA_EVENT_JFR_LIMIT` 但 `p_jfr[]` 中匹配不到 | `URMA_SUCCESS`，`port_id == UINT16_MAX` |
| 事件为 `JFR_ERR` / `JETTY_LIMIT` / `JETTY_ERR` / `PORT_*` / `DEV_FATAL` | `URMA_SUCCESS`，`port_id == UINT16_MAX` |
| 通过 ops 入口（`urma_get_async_event()`）取事件 | 行为与改造前一致，无端口信息 |

### 7.3 集成测试

1. **RNR 端到端**：按 3.5 的流程配置水线 → 触发 `JFR_LIMIT` → 识别端口并补充 RQE，确认数据面无 RNR 重传/停顿。
2. **单端口触限**：只让一个成员端口触限，验证事件携带的端口与实际一致。
3. **多端口依次触限**：两个成员端口先后触限，验证每个事件各自携带正确端口，事件无丢失。
4. **ack 后继续上报**：ack 后同一端口可继续上报新的 `JFR_LIMIT`；连续取事件并逐个 ack 后，`urma_delete_jfr()` / `bondp_delete_jfr()` 正常返回，不因等待 ack 卡住。
5. **查询聚合**：只对单端口配置阈值后调用 `urma_query_jfr()`，确认返回值为所有端口中的最小非零值。
6. **Jetty 共享 JFR 联动**：对某端口 JFR 配置阈值，确认只有该端口物理 Jetty 的接收按新阈值生效。
7. **旧接口兼容**：仅使用 `urma_modify_jfr()` / `urma_get_async_event()` 时，功能与改造前一致。

---

## 8. 附录

### 8.1 相关代码位置

| 内容 | 位置 |
| --- | --- |
| core modify 接口 | `src/urma/lib/urma/core/include/urma_api.h:320`、`src/urma/lib/urma/core/urma_cp_api.c:1137` |
| bond modify 实现 | `src/urma/lib/urma/bond/bondp_cp_jetty.c:1243`（`bondp_modify_jfr_sched`）、`:1272`（`bondp_query_jfr`）、`:1691`（`bondp_modify_jetty`） |
| 物理 JFR 创建 / 删除 / user_ctx | `src/urma/lib/urma/bond/bondp_cp_jetty.c:998`、`:1011-1013`、`:1025`（delete pjfr）、`:1188`（`bondp_delete_jfr`） |
| 物理 Jetty 共享 JFR | `src/urma/lib/urma/bond/bondp_cp_jetty.c:1377` |
| 端口 id 互转 | `src/urma/lib/urma/bond/bondp_cp_jetty.c:543`（端口 → 下标）、`:588`（下标 → 端口，声明于 `bondp_cp_tjetty.h:74`） |
| 无效端口哨兵 | `src/urma/lib/urma/bond/bondp_cp_jetty.c:54`（`bondp_get_cr_local_port_id`） |
| bond 异步事件 | `src/urma/lib/urma/bond/bondp_dp_interrupt.c:242`（获取）、`:205`（映射）、`:288`（应答）；声明 `bondp_dp_interrupt.h:26` |
| bond epoll 聚合物理 async_fd | `src/urma/lib/urma/bond/bondp_provider_ops.c:203-210`、`:359-372` |
| ops 挂载 | `src/urma/lib/urma/bond/bondp_provider_ops.c:55`（`modify_jfr`）、`:91`（`get_async_event`）、`:92`（`ack_async_event`） |
| core 事件接口与分发 | `src/urma/lib/urma/core/urma_cmd.c:2602`（取）、`:2643`（应答）、`urma_cp_api.c:3404` / `:3417`（分发） |
| 删除等待 ack | `src/urma/lib/urma/core/urma_cmd.c:1017`（`urma_cmd_delete_jfr`）、`:118`（`wait_async_event_ack`） |
| 内核回传用户态对象指针 | `src/urma/lib/urma/core/urma_cmd.c:734`（create）、`:2622`（事件） |
| 属性与状态枚举 | `src/urma/lib/urma/core/include/urma_types.h:667`（`urma_jfr_attr_t`）、`urma_opcode.h`（`urma_jfr_state_t`） |
| 底层阈值校验 | `src/urma/hw/udma/udma_u_jfr.c:363`（`udma_u_modify_jfr`）、`:343`（`udma_verify_modify_jfr`） |
| 端口 hint 结构 | `bondp_api.h`、`doc/ch/urma/bond-port-wr-design.md` |

### 8.2 术语

| 术语 | 含义 |
| --- | --- |
| 虚拟对象 | 应用直接持有的 bond 对象（`v_jfr`、`v_jetty` 等） |
| 物理对象 | 每个成员端口上的真实对象（`p_jfr[i]`、`p_jetty[i]`） |
| 本端端口 | 应用侧 bond 设备的成员端口，用 `bondp_port_id_t` 表示 |
| 水线 | 接收阈值 `rx_threshold`：可用 RQE 降到该值以下时上报 `JFR_LIMIT` |
| 超集语义 | 新接口可返回任意事件类型，仅在特定类型上提供附加信息，不做事件类型过滤 |
