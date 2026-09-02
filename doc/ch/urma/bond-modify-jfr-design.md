# urma 聚合设备 modify_jfr 按本端端口配置接收阈值 设计

| 项目 | 内容 |
| --- | --- |
| 仓库 | umdk（URMA） |
| 模块 | `src/urma/lib/urma/bond/` |
| 需求 | urma 聚合设备的 `modify_jfr` 支持应用指定**本端**端口，按端口配置接收阈值 `rx_threshold` |
| 依赖类型 | `bondp_wr_hint_t`（见 [bond-port-wr-design.md](bond-port-wr-design.md) 第 2 章） |

## 目录

- [1. 需求与范围](#1-需求与范围)
- [2. 现状分析](#2-现状分析)
- [3. 接口设计](#3-接口设计)
- [4. 实现方案](#4-实现方案)
- [5. 影响面与兼容性](#5-影响面与兼容性)
- [6. 已知限制](#6-已知限制)
- [7. 测试建议](#7-测试建议)
- [8. 附录](#8-附录)

---

## 1. 需求与范围

### 1.1 需求描述

聚合（bond）设备的 `modify_jfr` 目前只能对**全部**成员端口统一生效。本设计新增一个接口，使应用能够指定**本端端口**，只对指定端口对应的物理 JFR 下发修改，本版本用于配置接收阈值 `rx_threshold`。

端口信息由 `bondp_wr_hint_t` 承载，复用其 `version` / `size` / `mask` 可扩展设计。

### 1.2 范围

| 维度 | 范围 |
| --- | --- |
| 属性 | 仅 `JFR_RX_THRESHOLD`（接收阈值） |
| 端口方向 | 仅本端端口（hint 的 `SRC_*`） |
| 对象 | 仅 JFR；`modify_jetty` / `modify_jfs` 保持现状 |
| 并发 | 新增接口具备与删除操作互斥的保护，见 4.5 |

### 1.3 目标

| 目标 | 说明 |
| --- | --- |
| 按本端端口配置阈值 | 应用可指定单个本端端口，只对 `p_jfr[该端口]` 下发 `urma_modify_jfr()`，其他成员端口不受影响 |
| 复用可扩展 hint | 端口信息由 `bondp_wr_hint_t` 承载，遵守该类型的版本、方向、必选/可选位规则 |
| 不触碰接收调度 | 不修改 `schedule_recv()`、`post_recv` 的端口选择、`valid[]`，也不改变健康检查与 failover/failback 逻辑 |
| 默认行为不变 | 应用不指定端口时，行为与现有 `urma_modify_jfr()` 完全一致 |
| 语义明确 | 端口解析失败、非成员端口、非法 hint、非法 `attr` 均返回明确错误，不产生隐式副作用 |

### 1.4 非目标

- 不支持配置 JFR 状态（`JFR_STATE`），不改变 JFR 状态机相关行为。若需要按端口 drain / 复位，涉及与接收调度、Jetty `modify_to_error` 的联动，属独立需求。
- 不改变 bond 的接收端口调度：`schedule_recv()`、`post_recv` 的端口选择、`valid[]` 均不受本能力影响。
- 不改变既有 `urma_modify_jfr()` 的行为与实现逻辑（仅函数命名调整，见 4.7）。
- 不改变 `urma_query_jfr()` 的聚合语义（见 2.4、3.7）。
- 不提供 `modify_jetty` / `modify_jfs` 的按端口版本。

---

## 2. 现状分析

### 2.1 现有调用链

```text
应用: urma_modify_jfr(jfr, attr)                       # core/include/urma_api.h:320
  └─ urma_cp_api.c: urma_modify_jfr()                  # 参数校验 + ops 分发
       └─ ops->modify_jfr(jfr, attr)
            └─ bondp_provider_ops.c: .modify_jfr = bondp_modify_jfr_sched
                 └─ bondp_cp_jetty.c: bondp_modify_jfr_sched()          # 1243 行
                      for (i = 0; i < URMA_UBAGG_DEV_MAX_NUM; i++)
                          if (p_jfr[i] != NULL)
                              urma_modify_jfr(p_jfr[i], attr);           # 逐个物理 JFR
```

现有实现要点（`bondp_cp_jetty.c:1243`）：

- 遍历**全部**物理 JFR，逐个下发；不具备端口选择能力。
- 失败不中断遍历：记录 `final_ret` 后继续，最终返回最后一次失败的错误码。
- 无 `deleting` / 引用计数保护，也不加锁。

### 2.2 物理 JFR 布局

| 项 | 说明 | 代码位置 |
| --- | --- | --- |
| `p_jfr[]` | 稀疏数组，仅 `enabled_indices[]` 中的下标非空；PORT 级 bonding 下下标可接近 `URMA_UBAGG_DEV_MAX_NUM` | `bondp_create_pjfr()`（`bondp_cp_jetty.c:998`） |
| `members[]` | 与 `p_jfr[]` 共用同一 union，`members[i] != NULL` 等价于 `p_jfr[i] != NULL` | `bondp_types.h` 的 `bondp_comp_t` |
| `valid[]` | 创建 pjfr 时置 `true`；仅由发送侧故障 CR 清除（见 4.6） | `bondp_cp_jetty.c:1013` |
| `enabled_indices[]` / `enabled_count` | 该 JFR 的成员端口集合（来自 ctx 端口配置或创建对象时的 `port_ids`） | `init_active_indices_ex()` |

`p_jfr[i]` 指向**物理上下文**上的 JFR（`urma_ctx` 为 `bdp_ctx->p_ctxs[i]`），对它调用 `urma_modify_jfr()` 会走物理设备 ops，不会再次进入 bond 层。

### 2.3 Jetty 共享 JFR 的物理接线

聚合设备要求 Jetty 使用共享 JFR。创建物理 Jetty 时按端口取该端口自己的物理 JFR（`bondp_cp_jetty.c:1377`）：

```c
p_cfg.shared.jfr = bdp_jfr->p_jfr[i];   /* i 为该 Jetty 的成员端口 */
```

因此对某个端口的物理 JFR 做 modify，会同时影响**该端口上**引用它的 Jetty 接收侧。

### 2.4 查询的聚合语义

`bondp_query_jfr()`（`bondp_cp_jetty.c:1272`）对全部物理 JFR 做聚合：

- `state`：只要有一个端口为 `URMA_JFR_STATE_READY`，返回 `READY`；
- `rx_threshold`：返回所有非 0 值中的最小值；全为 0 时返回 0。

即：按端口配置阈值后，`urma_query_jfr()` 只能给出聚合结果，无法据此读出单个端口的配置值。

### 2.5 现状局限

1. 无法只对单个本端端口调整 `rx_threshold`（例如只为某端口做流控阈值微调）——本需求要解决的问题。
2. 无法只把单个本端端口的 JFR 置 `ERROR` 做局部冲刷，当前只能整 JFR 一起冲刷（不在本设计范围）。
3. 无法在端口粒度上做局部复位（置 `READY`）（不在本设计范围）。

---

## 3. 接口设计

### 3.1 接口定义

在公共头文件 `bondp_api.h` 中新增（与 `bondp_post_*_wr()` 系列并列）：

```c
/**
 * 按本端端口配置 bond JFR 的接收阈值。
 * @param[in] jfr:  bond 上下文中的 JFR 对象；
 * @param[in] attr: 待修改属性；本接口要求 attr->mask == JFR_RX_THRESHOLD；
 * @param[in] hint: 路径提示；仅支持本端端口（SRC_*），远端端口位必须为 0；
 *                  mask == 0 表示应用不指定端口，作用范围为全部成员端口；
 * @return URMA_SUCCESS 成功；其他值见 3.4
 */
urma_status_t bondp_modify_jfr(urma_jfr_t *jfr, urma_jfr_attr_t *attr,
                               bondp_wr_hint_t *hint);
```

命名约定：ops 入口带 `_sched` 后缀，带 hint 的对外接口使用不带后缀的名字（与 `bondp_post_jetty_send_wr` / `bondp_post_jetty_send_wr_sched` 一致，见 [bond-port-wr-design.md](bond-port-wr-design.md) 第 2.5 节）。

| 角色 | 函数名 | 说明 |
| --- | --- | --- |
| 对外接口（本设计） | `bondp_modify_jfr(jfr, attr, hint)` | 声明在 `bondp_api.h`，带 hint |
| ops 入口 | `bondp_modify_jfr_sched(jfr, attr)` | 全端口实现，挂载到 `g_bond_ops.modify_jfr`，供 `urma_modify_jfr()` 调用 |

### 3.2 参数约束

| 参数 | 约束 |
| --- | --- |
| `jfr` | 非空；`jfr->urma_ctx` 必须属于 bond 设备（`bondp_is_bond_ctx()`）；对象类型必须为 `BONDP_COMP_JFR` |
| `attr` | 非空；`attr->mask` 必须等于 `JFR_RX_THRESHOLD`。`rx_threshold` 的合法范围由底层校验：`udma_verify_modify_jfr()` 要求 `rx_threshold <= 该端口 JFR 深度（wqe_cnt）`（`udma_u_jfr.c:343`） |
| `hint` | 非空；`version` / `size` / `mask` 规则见 3.3 |

### 3.3 hint 使用规则

`bondp_wr_hint_t` 的方向与位规则在本接口上的落地：

| 项 | 规则 | 理由 |
| --- | --- | --- |
| 必选位 | `BONDP_WR_HINT_MASK_SRC_CHIP_ID` + `BONDP_WR_HINT_MASK_SRC_PORT_ID` | 至少要明确"哪颗芯片的哪个端口" |
| 可选位 | `BONDP_WR_HINT_MASK_SRC_DIE_ID`；未置位时不校验 `die_id`，按缺省 `1` 处理 | `die_id` 不是强制参数；当前实现只支持 `die_id == 1`（`IODIE_NUM_PER_CHIP == 1`） |
| 必须为 0 | 全部 `BONDP_WR_HINT_MASK_DST_*` | JFR 是本地接收对象，不存在"远端端口"语义，传入不生效 |
| `mask == 0` | 应用不指定端口，作用范围为全部成员端口 | 与 `urma_modify_jfr()` 行为保持一致 |
| `version` | 必须为 `BONDP_WR_HINT_VERSION_V0` | 结构体版本化 |
| `size` | 必须与实现约定的结构大小一致 | 防止越界读取调用方内存 |
| 非法组合 | 缺 `CHIP_ID` 或缺 `PORT_ID`；置位 `DIE_ID` 但 `die_id != 1` | 参数非法，直接拒绝 |

`src_id` 字段语义沿用 `bondp_port_id_t`：`chip_id ∈ [1, CHIP_NUM]`、`port_idx ∈ [0, PORT_NUM]` 或 `UINT8_MAX`（primary EID）；`die_id` 仅在置位 `SRC_DIE_ID` 时校验（必须为 1），未置位时按 1 处理。

### 3.4 返回值与错误码

| 场景 | 返回 |
| --- | --- |
| 成功 | `URMA_SUCCESS` |
| `jfr` / `attr` / `hint` 为空 | `URMA_EINVAL` |
| 上下文非 bond 或对象类型不符 | `URMA_EINVAL` |
| `attr->mask` 不含 `JFR_RX_THRESHOLD`，或包含 `JFR_STATE` 等其他位（仅 `mask != 0` 的单端口路径校验） | `URMA_EINVAL` |
| `hint->version` / `hint->size` 非法 | `URMA_EINVAL` |
| `hint->mask` 含任何 `DST_*` 位 | `URMA_EINVAL` |
| `hint->mask` 缺少 `SRC_CHIP_ID` 或 `SRC_PORT_ID` | `URMA_EINVAL` |
| `hint->mask` 置位 `SRC_DIE_ID` 但 `src_id.bs.die_id != 1` | `URMA_EINVAL` |
| `src_id` 坐标非法（`chip_id` / `port_idx` 越界，或该物理上下文不存在） | `URMA_EINVAL` |
| 指定端口不是该 JFR 的成员端口（`p_jfr[idx] == NULL`） | `URMA_EINVAL` |
| `rx_threshold` 超过该端口 JFR 深度（由底层校验） | 透传底层 `URMA_EINVAL` |
| 对象正在删除（`deleting == true`） | `URMA_EAGAIN` |
| 底层 `urma_modify_jfr(p_jfr[idx], attr)` 失败 | 透传底层错误码 |

单端口路径不做"失败后尝试其他端口"的动作：只有被指定的端口会被修改。

### 3.5 与 urma_modify_jfr 的关系

| 调用方式 | 生效范围 | 说明 |
| --- | --- | --- |
| `urma_modify_jfr(jfr, attr)` | 全部成员端口 | 经 core ops 调用 `bondp_modify_jfr_sched(jfr, attr)`，行为不变 |
| `bondp_modify_jfr(jfr, attr, hint)`，`mask != 0` | 指定的单个本端端口 | 本设计新增能力 |
| `bondp_modify_jfr(jfr, attr, hint)`，`mask == 0` | 全部成员端口 | 复用 `bondp_modify_jfr_sched()` 的遍历逻辑，行为等价 |

### 3.6 使用示例

```c
/* 场景一：只把某个本端端口的接收阈值配置为 16 */
urma_jfr_attr_t attr = {0};
attr.mask = JFR_RX_THRESHOLD;
attr.rx_threshold = 16;                   /* 必须 <= 该端口 JFR 深度 */

bondp_wr_hint_t hint = {0};
hint.version = BONDP_WR_HINT_VERSION_V0;
hint.size    = sizeof(bondp_wr_hint_t);
hint.mask    = BONDP_WR_HINT_MASK_SRC_CHIP_ID |   /* 必选 */
               BONDP_WR_HINT_MASK_SRC_PORT_ID;    /* 必选 */
               /* SRC_DIE_ID 可选：不置位时 die_id 按缺省 1 处理 */
hint.src_id.bs.chip_id  = 1;
hint.src_id.bs.port_idx = UINT8_MAX;      /* primary EID */

urma_status_t st = bondp_modify_jfr(jfr, &attr, &hint);

/* 场景二：应用不指定端口（作用范围为全部成员端口，等价 urma_modify_jfr） */
bondp_wr_hint_t all = {0};
all.version = BONDP_WR_HINT_VERSION_V0;
all.size    = sizeof(bondp_wr_hint_t);
all.mask    = 0;
(void)bondp_modify_jfr(jfr, &attr, &all);
```

### 3.7 查询与可观测性

`urma_query_jfr()` 返回的 `rx_threshold` 是**所有成员端口的非零最小值**（见 2.4）。按端口配置后：

- 应用无法通过 `urma_query_jfr()` 读出单个端口的配置值；
- 建议应用侧记录本端的按端口配置值；若只对单端口做了配置，聚合值即该端口值，可据此校验。

---

## 4. 实现方案

### 4.1 总体流程

```text
bondp_modify_jfr(jfr, attr, hint)
  +- 参数与对象校验（jfr / attr / hint、bond 上下文、comp_type）
  +- 控制面并发防护：use_cnt++，若 deleting 则 use_cnt-- 并返回 URMA_EAGAIN
  +- hint 的 version / size 校验
  +- mask == 0 ?
  |    +- 是 -> ret = bondp_modify_jfr_sched(jfr, attr)   # 全端口遍历，不额外限制 attr
  |    +- 否 -> 继续
  +- attr 校验（要求 mask == JFR_RX_THRESHOLD）
  +- hint 方向位与坐标校验（3.3 的规则）
  +- convert_bond_port_id_to_active_index(ctx, hint->src_id, &idx)
  +- idx 越界或 p_jfr[idx] == NULL -> URMA_EINVAL
  +- ret = urma_modify_jfr(p_jfr[idx], attr)
  +- 释放引用（use_cnt--），return ret
```

说明：`mask == 0` 分支不额外限制 `attr->mask`，以保证与现有 `urma_modify_jfr()` 行为完全一致（非法 `attr` 由底层返回错误）；`JFR_RX_THRESHOLD` 的限定只作用于单端口路径。并发防护对所有返回路径生效，实现时使用统一出口释放引用，避免漏放导致 `urma_delete_jfr()` 长期返回 `URMA_EAGAIN`。

### 4.2 hint 解析 helper

新增内部静态函数：

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
| 3 | `mask` 含 `SRC_CHIP_ID` 与 `SRC_PORT_ID`（缺任一即失败）；若置位 `SRC_DIE_ID` 则 `die_id` 必须为 1 | `URMA_EINVAL` |
| 4 | `convert_bond_port_id_to_active_index(bdp_ctx, hint->src_id, local_idx)` | `URMA_EINVAL` |

该 helper 与 `bondp_post_recv_wr_by_port()` 的现有校验（`bondp_datapath.c:1336`）语义一致，可供后续 `modify_jetty` / `modify_jfs` 或 `post_*` 复用。

### 4.3 接口实现

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

    /* 控制面并发防护：先占引用再检查删除标记。
     * 与 bondp_delete_jfr() 的 deleting + use_cnt 机制互斥：删除侧置 deleting
     * 后若发现 use_cnt > 0 会回滚 deleting 并返回 URMA_EAGAIN，由调用方重试删除 */
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

    /* 只支持按端口配置接收阈值，不支持按端口配置 JFR 状态 */
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

要点：

- 复用 `bondp_is_bond_ctx()` 做上下文校验，避免非 bond 对象被 `CONTAINER_OF_FIELD` 误解。
- 成员判定用 `p_jfr[idx] == NULL`，与 `post_recv` 路径用 `members[idx] == NULL` 等价（同一 union）。
- 不校验 `valid[idx]`，理由见 4.6。
- 并发防护的写法与 `bondp_post_*_wr()` 系列一致：先 `use_cnt++`，再判 `deleting`，所有出口统一 `use_cnt--`。

### 4.4 `mask == 0` 分支

直接调用 `bondp_modify_jfr_sched(jfr, attr)`：

- 保持"遍历全部非空 `p_jfr`、失败不中断、返回最后一次错误"的既有语义；
- 不引入第二套"全端口"实现，避免行为分叉。

### 4.5 并发与生命周期

删除流程（`bondp_delete_jfr()`，`bondp_cp_jetty.c:1188`）会置 `deleting = true` 并检查 `use_cnt`：若 `use_cnt > 0`，回滚 `deleting` 并返回 `URMA_EAGAIN`，由调用方重试删除。基于该机制，本接口采用如下防护：

| 场景 | 方案 |
| --- | --- |
| 与 `urma_delete_jfr()` 并发 | 入口先 `atomic_fetch_add(&bdp_jfr->use_cnt.atomic_cnt, 1)`，再判 `atomic_load(&bdp_jfr->deleting)`；若正在删除则撤销引用并返回 `URMA_EAGAIN`；所有出口统一释放引用。检查顺序与 `bondp_post_*_wr()` 系列一致 |
| 与其他 modify 并发 | 不引入锁；`use_cnt` 只解决与删除的竞争，不提供 modify 之间的互斥，由调用方序列化 |
| 与数据面并发 | 单端口 modify 只操作物理 JFR 对象，不新增同步 |

### 4.6 与 valid[] / 接收路径的关系

本接口只配置 `rx_threshold`，属纯控制面操作，不读取也不修改任何调度相关状态：不校验 `valid[]`，不触碰 `schedule_recv()`、`sqe_cnt`、`rqe_cnt`。

对 JFR 对象，`valid[]` 在创建时置 `true`，之后不会被清除：

- `valid[]` 仅由发送侧故障 CR 处理路径 `handle_send_cr_with_store()`（`bondp_datapath.c:1751`）清除；
- 纯 JFR 对象不承载发送路径（发送走 JFS/Jetty），因此其 `valid[]` 恒为 `true`；
- failback 只重建 Jetty 的 `p_jetty`（`bondp_rebuild_pjetty()`），不涉及 `p_jfr`。

因此：`valid[idx]` 校验对 JFR 不产生行为差异；端口物理故障时 `p_jfr[idx]` 对象仍存在，阈值配置的成败由底层 `urma_modify_jfr()` 决定，错误码直接透传。

### 4.7 改动清单

| 文件 | 改动 |
| --- | --- |
| `src/urma/lib/urma/bond/include/bondp_api.h` | 新增 `bondp_modify_jfr(jfr, attr, hint)` 声明与文档注释 |
| `src/urma/lib/urma/bond/bondp_cp_jetty.c` | 现有全端口实现定名 `bondp_modify_jfr_sched()`；新增 `bondp_hint_parse_local_idx()` 与 3 参 `bondp_modify_jfr()` |
| `src/urma/lib/urma/bond/bondp_cp_jetty.h` | 声明 `bondp_modify_jfr_sched()`；如需跨模块调用 3 参接口则另加声明 |
| `src/urma/lib/urma/bond/bondp_provider_ops.c` | `.modify_jfr = bondp_modify_jfr_sched` |
| `doc/ch/urma/URMA API Guide.ch.md`、`doc/en/urma/URMA API Guide.md` | 新增接口章节（放在 bond 按端口投递章节附近） |
| `doc/ch/urma/bond-modify-jfr-design.md` | 本文档 |
| `umdk.spec` / CMake / bazel | 无需改动（`bondp_api.h` 已在安装与打包清单中） |

前置依赖：本设计依赖 `bondp_wr_hint_t` 类型与配套宏先落地（替换说明见 [bond-port-wr-design.md](bond-port-wr-design.md)）。

---

## 5. 影响面与兼容性

| 维度 | 结论 |
| --- | --- |
| 现有 API | `urma_modify_jfr()` 行为与调用链不变；其 ops 入口定名为 `bondp_modify_jfr_sched(jfr, attr)` |
| 符号变化 | `bondp_modify_jfr` 由 3 参接口占用（C 无重载，等同于替换该符号）；全端口实现使用 `bondp_modify_jfr_sched`。两者此前只声明在未安装的内部头 `bondp_cp_jetty.h`，对外无兼容承诺 |
| ABI | 新增导出符号 `bondp_modify_jfr`（3 参），`bondp_api.h` 已安装；无版本脚本限制，符号默认导出 |
| 数据结构 | 不新增字段；`bondp_comp_t`、`urma_jfr_t` 不变 |
| 数据面路径 | 不涉及 `post_send` / `post_recv` 代码路径，无性能影响 |
| 接收调度 | 无调度侧改动：`schedule_recv()`、`valid[]`、健康检查与 failover/failback 均不变 |
| 控制面并发 | 新增接口与 `urma_delete_jfr()` 通过 `use_cnt` + `deleting` 互斥：删除期间调用返回 `URMA_EAGAIN`；反之删除会因 `use_cnt > 0` 返回 `URMA_EAGAIN` 由调用方重试 |
| Jetty 共享 JFR | 按端口配置的 `rx_threshold` 作用于该端口的物理 JFR，因此只影响该端口物理 Jetty 的接收侧（见 2.3） |

---

## 6. 已知限制

1. 既有 `urma_modify_jfr()`（ops 入口 `bondp_modify_jfr_sched()`）不带并发防护，与 `urma_delete_jfr()` 并发时仍存在使用正在释放的 `p_jfr[]` 的窗口；本设计只保证新增接口具备防护能力。若后续要求两条路径保护力度一致，可在 `bondp_modify_jfr_sched()` 内补同样的 `use_cnt` + `deleting` 判定。
2. `urma_query_jfr()` 只返回聚合值（`rx_threshold` 取所有端口的最小非零值），不具备按端口读取能力（见 3.7）。
3. 按端口配置作用于物理 JFR，会连带影响该端口物理 Jetty 的接收侧，属预期语义。

---

## 7. 测试建议

### 7.1 单元测试

| 用例 | 预期 |
| --- | --- |
| `jfr` / `attr` / `hint` 为 NULL | `URMA_EINVAL` |
| 非 bond 上下文对象传入 | `URMA_EINVAL` |
| `hint->version` / `size` 不合法 | `URMA_EINVAL` |
| `hint->mask` 含 `DST_*` 位 | `URMA_EINVAL` |
| `hint->mask` 只置 `SRC_CHIP_ID`（缺 `SRC_PORT_ID`） | `URMA_EINVAL` |
| `hint->mask` 置 `SRC_CHIP_ID` + `SRC_PORT_ID`、不置 `SRC_DIE_ID` | 正常生效（`die_id` 按缺省 1 处理） |
| `hint->mask` 置 `SRC_DIE_ID` 且 `die_id = 2` | `URMA_EINVAL` |
| `src_id` 坐标越界（`chip_id = 0`、`port_idx > PORT_NUM`） | `URMA_EINVAL` |
| `src_id` 对应端口不是该 JFR 成员 | `URMA_EINVAL` |
| `attr->mask` 含 `JFR_STATE` | `URMA_EINVAL` |
| `attr->mask` 不含 `JFR_RX_THRESHOLD`（如为 0） | `URMA_EINVAL` |
| `hint->mask == 0` | 与 `urma_modify_jfr()` 等价（全部端口生效） |
| 合法单端口 + `JFR_RX_THRESHOLD` | 只有该端口物理 JFR 的阈值变化，其他端口不变 |
| `rx_threshold` 大于该端口 JFR 深度 | 透传底层 `URMA_EINVAL`（`udma_verify_modify_jfr`） |
| 同一端口连续配置两次不同阈值 | 以最后一次为准 |
| 对象正在删除（`deleting == true`）时调用 | `URMA_EAGAIN` |

### 7.2 集成测试

1. **查询聚合验证**：只对单端口配置阈值后调用 `urma_query_jfr()`，确认返回值为"所有端口中的最小非零值"（2.4 / 3.7）。
2. **Jetty 共享 JFR 联动**：对某端口 JFR 配置阈值，确认只有该端口物理 Jetty 的接收按新阈值生效，其他端口不受影响。
3. **业务流量验证**：在绑定该端口的收包路径上验证阈值生效，边界行为与单端口 `urma_modify_jfr()` 一致。
4. **故障端口上调用**：在端口故障（`valid == 0`、`port_status_bad == true`）状态下调用，确认错误码来自底层而非被上层拦截。
5. **并发**：一边调用 `bondp_modify_jfr()`、一边调用 `urma_delete_jfr()`，确认二者不会同时进入（一方返回 `URMA_EAGAIN`），不出现越界访问；多轮重复后 `use_cnt` 能回到 0，`urma_delete_jfr()` 最终成功。

---

## 8. 附录

### 8.1 相关代码位置

| 内容 | 位置 |
| --- | --- |
| core API | `src/urma/lib/urma/core/include/urma_api.h:320`、`src/urma/lib/urma/core/urma_cp_api.c:1137` |
| ops 定义与挂载 | `src/urma/lib/urma/core/include/urma_provider.h:88`、`src/urma/lib/urma/bond/bondp_provider_ops.c:55` |
| bond 现有实现 | `src/urma/lib/urma/bond/bondp_cp_jetty.c:1243`（`bondp_modify_jfr_sched`）、`:1272`（`bondp_query_jfr`）、`:1691`（`bondp_modify_jetty`） |
| 物理 JFR 创建/删除 | `src/urma/lib/urma/bond/bondp_cp_jetty.c:998`（`bondp_create_pjfr`）、`:1025`（`bondp_delete_pjfr`）、`:1188`（`bondp_delete_jfr`） |
| 物理 Jetty 共享 JFR | `src/urma/lib/urma/bond/bondp_cp_jetty.c:1377` |
| 端口 id 转换 | `src/urma/lib/urma/bond/bondp_cp_jetty.c:543`（`convert_bond_port_id_to_active_index`） |
| hint 结构与规则 | `bondp_api.h`、`doc/ch/urma/bond-port-wr-design.md` 第 2 章 |
| 属性与状态枚举 | `src/urma/lib/urma/core/include/urma_types.h:667`（`urma_jfr_attr_t`）、`urma_opcode.h`（`urma_jfr_state_t`） |
| 底层实现参考 | `src/urma/hw/udma/udma_u_jfr.c:363`（`udma_u_modify_jfr`）、`:343`（`udma_verify_modify_jfr`） |

### 8.2 术语

| 术语 | 含义 |
| --- | --- |
| 虚拟/逻辑对象 | 应用直接持有的 bond 对象（`v_jfr`、`v_jetty`） |
| 物理对象 | 每个成员端口上的真实对象（`p_jfr[i]`、`p_jetty[i]`） |
| 本端端口 | 应用侧 bond 设备的成员端口，对应 hint 的 `SRC_*` |
| 远端端口 | 对端 bond 设备的成员端口，对应 hint 的 `DST_*`；在 JFR 相关接口中无意义 |
