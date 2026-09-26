# Bond 按端口投递 WR 设计文档

| 项目 | 内容 |
| --- | --- |
| 仓库 | umdk（URMA） |
| 分支 | `port` |
| 对应提交 | `3e3e67f3` urma: add per-port send/recv WR posting and recv port report in CQE |
| 涉及模块 | `src/urma/lib/urma/bond/` |
| 文档定位 | 该提交的设计说明与实现走读，供接口使用者与维护者参考 |

## 修订记录

| 版本 | 日期 | 说明 |
| --- | --- | --- |
| v1.0 | 2026-09-26 | 首次编写，覆盖新增结构体/API、发送侧与接收侧实现方案、使用约束 |
| v1.1 | 2026-09-26 | 按评审意见将提示结构体由 `bondp_path_hint_t` 调整为 `bondp_wr_hint_t`（带 `version` / `size` / `mask`），并同步更新约束、示例与实现说明 |
| v1.2 | 2026-09-26 | 明确 `mask` 的必选 / 可选位：`CHIP_ID` 与 `PORT_ID` 必须置位，`DIE_ID` 非强制（未置位按缺省 1 处理） |
| v1.3 | 2026-09-26 | 补充说明同一命名约定适用于 modify 系列（ops 入口 `*_sched`，带 hint 的对外接口无后缀），供 `bondp_modify_jfr` 设计引用 |

## 目录

- [1. 背景与目标](#1-背景与目标)
- [2. 新增结构体与 API 设计](#2-新增结构体与-api-设计)
- [3. 发送侧实现方案](#3-发送侧实现方案)
- [4. 接收侧实现方案](#4-接收侧实现方案)
- [5. 调用示例](#5-调用示例)
- [6. 实现注意点](#6-实现注意点)
- [7. 附录](#7-附录)

---

## 1. 背景与目标

### 1.1 背景

bond（`urma_ubagg`）把多个物理端口聚合成一个虚拟设备：应用看到的是一个虚拟 JFS/JFR/Jetty，数据面由 bond 在多个物理端口之间调度。该模型下应用无法表达"这次投递必须走某个成员端口"，也无法知道一次完成事件实际来自哪个物理端口。

本提交在保持既有调度语义不变的前提下，补齐两块能力：

1. **按端口投递**：允许应用在投递 WR 时指定 bond 成员端口。
2. **端口上报**：在完成事件（CQE）中携带该次操作实际使用的物理端口。

### 1.2 目标

| 目标 | 说明 |
| --- | --- |
| 端口亲和 | 应用能把一次投递绑定到指定成员端口，用于端口级路径亲和、性能隔离与故障定位 |
| 与既有调度共存 | 未使用 hint 的投递完全走原有 `active_backup` / `balance` / `standalone` 调度，行为不变 |
| 可观测 | 应用能知道这次投递实际落在哪个物理端口的 CQE 上 |
| 明确的失败语义 | 指定的端口不可用时返回错误，不做隐式改投，由应用决定是否重试 |

### 1.3 非目标

- 不改变 bond 的故障切换（failover）、健康检查（health check）、故障回切（failback）机制。
- 不提供发送侧指定**本端出口**端口的能力（详见 3.3 与 3.4）。
- 不提供接收侧指定**远端**端口的能力（接收路径本身不存在远端端口概念，详见 4.3）。

---

## 2. 新增结构体与 API 设计

### 2.1 投递提示结构体 bondp_wr_hint_t

> 命名说明：提交内原始实现使用的是 `bondp_path_hint_t`；本设计决定以 `bondp_wr_hint_t`（带 `version` / `size` / `mask`）作为对外类型，二者为等价替换关系。替换可行性分析见 2.1.3，代码落地清单见 7.3。

原实现（提交内当前代码，待替换）：

```c
typedef struct bondp_path_hint {
    union {
        struct {
            uint64_t has_src_port : 1;   /* 本端端口有效位：仅接收接口使用 */
            uint64_t has_dst_port : 1;   /* 远端端口有效位：仅发送接口使用 */
            uint64_t reserved     : 62;
        } bs;
        uint64_t value;
    } flag;
    bondp_port_id_t src_port_id;   /* 本端成员端口；发送路径不读取 */
    bondp_port_id_t dst_port_id;   /* 远端成员端口；接收路径不读取 */
    uint64_t reserved1;
    uint64_t reserved2;
} bondp_path_hint_t;
```

替换后（目标设计，新增内容位于公共头文件 [bondp_api.h](../../../src/urma/lib/urma/bond/include/bondp_api.h)）：

```c
#define BONDP_WR_HINT_VERSION_V0 0

/* 存在该宏即表示当前 liburma_ubagg 支持带 hint 的发送接口。
* 仅用于 #ifdef / #if defined() 判断，不要用 #if BONDP_HAS_... 判断取值。 */
#define BONDP_HAS_POST_JETTY_SEND_WR_HINT BONDP_HAS_POST_JETTY_SEND_WR_HINT

typedef enum bondp_wr_hint_mask {
    BONDP_WR_HINT_MASK_SRC_CHIP_ID = 1ULL << 0,
    BONDP_WR_HINT_MASK_SRC_DIE_ID = 1ULL << 1,
    BONDP_WR_HINT_MASK_SRC_PORT_ID = 1ULL << 2,
    BONDP_WR_HINT_MASK_DST_CHIP_ID = 1ULL << 3,
    BONDP_WR_HINT_MASK_DST_DIE_ID = 1ULL << 4,
    BONDP_WR_HINT_MASK_DST_PORT_ID = 1ULL << 5,
} bondp_wr_hint_mask_t;

#define BONDP_WR_HINT_MASK_ALL \
    (BONDP_WR_HINT_MASK_SRC_CHIP_ID | BONDP_WR_HINT_MASK_SRC_DIE_ID | \
     BONDP_WR_HINT_MASK_SRC_PORT_ID | BONDP_WR_HINT_MASK_DST_CHIP_ID | \
     BONDP_WR_HINT_MASK_DST_DIE_ID | BONDP_WR_HINT_MASK_DST_PORT_ID)

typedef struct bondp_wr_hint {
    uint32_t version; /* 必须为 BONDP_WR_HINT_VERSION_V0 */
    uint32_t size; /* 调用方头文件中的 sizeof(bondp_wr_hint_t) */
    uint64_t mask; /* 0 表示不指定，完全走 bondp 内部调度 */
    bondp_port_id_t src_id; /* chip/die/port 坐标；子字段有效性由 SRC_* 位决定 */
    bondp_port_id_t dst_id; /* chip/die/port 坐标；子字段有效性由 DST_* 位决定 */
    uint64_t reserved[4]; /* 必须为 0，预留给后续扩展 */
} bondp_wr_hint_t;
```

#### 2.1.1 字段设计要点

- 用 `mask` 逐位描述"哪些端口坐标子字段有效"，而不是用字段值 0 表示未指定：`bondp_port_id_t` 全 0 本身是非法端口（`chip_id` 合法范围从 1 开始），必须与"未指定"区分开；同时按子字段分级授权，便于只做芯片级/芯片+端口级亲和。
- `version` + `size` 提供结构体版本化能力：调用方在头文件中写入 `sizeof(bondp_wr_hint_t)`，库侧据此确定可安全读取的字节数，为后续字段扩展保留兼容路径。
- `reserved[4]` 必须为 0，预留后续扩展。
- `mask == 0` 表示**应用不指定端口**，由 bondp 内部调度：即由 liburma_ubagg（bond provider）内部的调度器按当前 bonding 模式选择本端出口与远端端口（`schedule_send()` / `schedule_send_fixed()` / `schedule_recv()` 等），驱动层不参与端口策略选择。该行为与不带提示调用 `urma_post_*_wr()` 完全一致。
- `mask` 的方向位与接口方向**强绑定**，且该约束直接来自当前实现，不是可选约定：每个方向只有一组有效位，对向位一律返回 `URMA_EINVAL`。

| 接口方向 | 允许置位的位 | 必须保持为 0 的位 | 置位非法位的结果 | 约束来源 |
| --- | --- | --- | --- | --- |
| 发送（`post_send`） | `BONDP_WR_HINT_MASK_DST_CHIP_ID` + `DST_PORT_ID`（必选），`DST_DIE_ID`（可选） | 全部 `SRC_*` | `URMA_EINVAL` | 发送接口**不接受应用指定的本端端口**：`active_backup` 模式下本端出口始终由 bondp 调度器按 `active_indices` 顺序选择（`balance` 下按负载选择），hint 只约束对端端口；应用若确实需要固定本端出口，只能通过创建对象时收窄端口集合（见 3.3） |
| 接收（`post_recv`） | `BONDP_WR_HINT_MASK_SRC_CHIP_ID` + `SRC_PORT_ID`（必选），`SRC_DIE_ID`（可选） | 全部 `DST_*` | `URMA_EINVAL` | 接收路径不存在远端端口语义，应用传入的对端端口信息在 `post_recv` 侧**永远不会生效** |

- `CHIP_ID` 与 `PORT_ID` 是必选位，缺任一返回 `URMA_EINVAL`；`DIE_ID` 不是强制位，未置位时不校验 `die_id`（按缺省 `1` 处理），置位时必须为 `1`（当前 `IODIE_NUM_PER_CHIP == 1`）。
- 对向坐标字段（发送侧的 `src_id`、接收侧的 `dst_id`）在对应实现路径上没有任何读取点。
- 调用方必须先整体清零再赋值（含 `reserved`）。

#### 2.1.2 与原 bondp_path_hint_t 的对应关系

| 场景 | 原表达（`bondp_path_hint_t`） | 替换后表达（`bondp_wr_hint_t`） | 等价性 |
| --- | --- | --- | --- |
| 发送钉住远端端口 | `has_dst_port = 1`，`dst_port_id = D` | `mask = DST_CHIP_ID \| DST_PORT_ID`（`DST_DIE_ID` 可选），`dst_id = D` | 等价 |
| 接收钉住本端端口 | `has_src_port = 1`，`src_port_id = S` | `mask = SRC_CHIP_ID \| SRC_PORT_ID`（`SRC_DIE_ID` 可选），`src_id = S` | 等价 |
| 发送/接收指定对向方向端口 | 发送置 `has_src_port`、接收置 `has_dst_port` → `URMA_EINVAL` | 发送置 `MASK_SRC_*`、接收置 `MASK_DST_*` → `URMA_EINVAL` | 等价 |
| 应用不指定端口、由 bondp 内部调度 | 传 `hint == NULL` → `URMA_EINVAL`，需改用 `urma_post_*_wr()` | `mask == 0` → 应用不指定端口，由 bondp 内部调度 | **语义变更**，已在 2.1.3 确认为按 bondp 内部调度投递 |

#### 2.1.3 替换可行性结论

**结论：可以替换。** 依据与前置决策如下。

| 维度 | 结论 |
| --- | --- |
| 兼容性成本 | 该类型随本提交新引入，提交仅存在于 `port` 与 `wh/port` 分支、未进入 `master`，属**未发布 API**，替换无 ABI/源码兼容负担 |
| 表达力 | `mask` 粒度细于原 `has_src_port` / `has_dst_port` 布尔位，是原设计的超集，原语义可按 2.1.2 完整映射 |
| 改动范围 | 集中在 `bondp_api.h`、`bondp_datapath.c`、`bondp_datapath_schedule.c/.h` 与文档，属机械替换加入口校验重写（清单见 7.3） |
| 风险 | 需要先把下列语义写死，否则会以"替换"之名引入新的模糊行为 |

已确认的语义点（直接沿用当前实现约束）：

1. **`mask == 0` 的含义**：表示**应用不指定端口**，由 bondp 内部调度，即由 liburma_ubagg 的调度器按 bonding 模式选择端口，与内核/驱动层的调度策略无关。建议统一为"`hint == NULL` 或 `mask == 0`"均按 bondp 内部调度投递，避免 `URMA_EINVAL` 与调度投递两套约定并存。
2. **`mask` 的方向绑定**：发送接口只接受 `DST_*`（对端端口信息），任何 `SRC_*` 置位返回 `URMA_EINVAL`；接收接口只接受 `SRC_*`（本端端口信息），任何 `DST_*` 置位返回 `URMA_EINVAL`。理由见 2.1.1 表中"约束来源"列。
3. **`mask` 的必选 / 可选位**：每个方向必须置位 `CHIP_ID` 与 `PORT_ID`；`DIE_ID` 可选，未置位时不校验 `die_id`（按缺省 `1` 处理），置位时必须为 `1`。因此"只钉到芯片级"（缺 `PORT_ID`）不被接受，直接返回 `URMA_EINVAL`。

仍需明确的语义点：

4. **`version` / `size` 校验规则**：V0 至少校验 `version == BONDP_WR_HINT_VERSION_V0`；`size` 需定义"大于/小于实现已知大小时如何处理"，并保证实现只读取 `min(size, sizeof)` 范围内的字节（`hint` 由调用方分配，越界读取属调用方内存）。

### 2.2 端口标识 bondp_port_id_t

复用 [urma_ubagg.h](../../../src/urma/lib/urma/bond/include/urma_ubagg.h) 中已有定义，语义与 `BONDP_USER_CTL_SET_BONDING_PORT` 完全一致：

| 字段 | 位宽 | 取值 | 说明 |
| --- | --- | --- | --- |
| `chip_id` | 4 | `[1, CHIP_NUM]`（当前 2） | 芯片号 |
| `die_id` | 4 | 固定 `1` | 当前 `IODIE_NUM_PER_CHIP == 1` |
| `port_idx` | 8 | `[0, PORT_NUM]`（当前 9），或 `UINT8_MAX` | `UINT8_MAX` 表示该芯片的 primary EID |

hint 中的端口 id 在接口入口会被 `convert_bond_port_id_to_active_index()`（`bondp_cp_jetty.c:543`）转换为内部 active index，该函数会校验：

- `chip_id` 非 0 且 `<= CHIP_NUM`；
- `die_id == 1`；
- `port_idx == UINT8_MAX` 或 `port_idx <= PORT_NUM`；
- 目标 index 对应的物理上下文存在（`p_ctxs[idx] != NULL`），即该端口在当前 bonding level 下真实可寻址。

转换失败时接口返回错误，不会退化为"按调度投递到其他端口"。

### 2.3 CQE 端口上报编码

在 [bondp_api.h](../../../src/urma/lib/urma/bond/include/bondp_api.h) 中新增：

```c
#define BONDP_IMM_PORT_ID_SHIFT     (22)
#define BONDP_IMM_PORT_ID_MASK      (0xFFFFULL << BONDP_IMM_PORT_ID_SHIFT)
#define BONDP_RECV_PORT_UNAVAILABLE (0)
```

约定：`imm_data` 的 `[22, 37]` 位承载端口信息，`0` 表示端口信息不可用。该编码对 bonding 上下文的**所有**接收 CQE 生效，包括通过普通调度接口投递的接收 WR。因此应用在把 `imm_data` 作为立即数语义使用前，必须先掩去这段位。

该 16 bit 字段与内部 `vjetty_id` / CR 端口索引字段位置重合，详见第 6 章。

### 2.4 API 列表

四个接口均在 [bondp_api.h](../../../src/urma/lib/urma/bond/include/bondp_api.h) 中声明。替换后 `bondp_path_hint_t` 不再对外暴露，统一使用 `bondp_wr_hint_t`：

```c
urma_status_t bondp_post_jfs_wr(urma_jfs_t *jfs, urma_jfs_wr_t *wr,
                                bondp_wr_hint_t *hint, urma_jfs_wr_t **bad_wr);
urma_status_t bondp_post_jetty_send_wr(urma_jetty_t *jetty, urma_jfs_wr_t *wr,
                                       bondp_wr_hint_t *hint, urma_jfs_wr_t **bad_wr);
urma_status_t bondp_post_jfr_wr(urma_jfr_t *jfr, urma_jfr_wr_t *wr,
                                bondp_wr_hint_t *hint, urma_jfr_wr_t **bad_wr);
urma_status_t bondp_post_jetty_recv_wr(urma_jetty_t *jetty, urma_jfr_wr_t *wr,
                                       bondp_wr_hint_t *hint, urma_jfr_wr_t **bad_wr);
```

| API | 对象 | hint 允许的方向位 | 语义 |
| --- | --- | --- | --- |
| `bondp_post_jfs_wr` | bond JFS | `BONDP_WR_HINT_MASK_DST_*` | 发送：消耗指定**远端**端口的 RQE |
| `bondp_post_jetty_send_wr` | bond Jetty | `BONDP_WR_HINT_MASK_DST_*` | 同上（Jetty 形态） |
| `bondp_post_jfr_wr` | bond JFR | `BONDP_WR_HINT_MASK_SRC_*` | 接收：只投到指定**本端**端口 |
| `bondp_post_jetty_recv_wr` | bond Jetty | `BONDP_WR_HINT_MASK_SRC_*` | 同上（Jetty 形态） |

配套公开接口：

```c
/* polling/flush 得到的 bonding CR 上查询实际端口，TX 表示出口、RX 表示入口 */
bondp_port_id_t bondp_get_cr_local_port_id(const urma_cr_t *cr);

/* 判断 bonding jetty 是否至少有一个可用的物理 jetty */
bool bondp_jetty_is_available(const urma_jetty_t *jetty);
```

### 2.5 ops 层改动与符号可见性

`bondp_provider_ops.c` 中数据面 ops 的挂载调整为：

```c
static urma_ops_t g_bond_ops = {
    /* ... */
    .post_jfs_wr         = bondp_post_jfs_wr_sched,
    .post_jfr_wr         = bondp_post_jfr_wr_sched,
    .post_jetty_send_wr  = bondp_post_jetty_send_wr_sched,
    .post_jetty_recv_wr  = bondp_post_jetty_recv_wr_sched,
    /* ... */
};
```

由此得到的行为约定：

- `urma_post_jetty_send_wr()` / `urma_post_jfs_wr()` 等公共 API 仍走 `*_sched` 版本（内部以 `hint == NULL` 调用），语义与本提交之前完全一致；替换后 `mask == 0` 应落到同一条调度路径，即"`hint == NULL`"与"`hint != NULL 且 mask == 0`"行为一致（见 2.1.3 决策 1）。
- 带 hint 的四个接口是 bond 专有符号，应用需要直接链接 `liburma_ubagg.so` 并 include `bondp_api.h`，**不能**通过公共 `urma_post_*_wr()` 传递 hint。
- 新增 `bondp_is_bond_ctx(ctx)`（判断 `ctx->ops == &g_bond_ops`），带 hint 的接口在入口做上下文强校验，避免非 bond 对象被 `CONTAINER_OF_FIELD` 误解。
- `bondp_api.h` 已加入 CMake 安装清单、`urma_bazel.sh` 打包列表与 `umdk.spec` 文件列表。
- 同一命名约定适用于 modify 系列：**ops 入口带 `_sched` 后缀**（如 `bondp_modify_jfr_sched`），**带 hint 的对外接口使用不带后缀的名字**（如 `bondp_modify_jfr`），与 `bondp_post_jetty_send_wr` / `bondp_post_jetty_send_wr_sched` 保持一致；具体见 [bond-modify-jfr-design.md](bond-modify-jfr-design.md) 4.1 节。

### 2.6 发送接口使用说明与约束

```c
bondp_wr_hint_t hint = {0};              /* 必须整体清零 */
hint.version = BONDP_WR_HINT_VERSION_V0;
hint.size    = sizeof(bondp_wr_hint_t);  /* 调用方头文件中的结构大小 */
hint.mask    = BONDP_WR_HINT_MASK_DST_CHIP_ID |   /* 必选 */
               BONDP_WR_HINT_MASK_DST_PORT_ID;    /* 必选 */
               /* DST_DIE_ID 可选：不置位时 die_id 按缺省 1 处理 */
hint.dst_id.bs.chip_id  = 1;             /* [1, CHIP_NUM] */
hint.dst_id.bs.port_idx = 0;             /* [0, PORT_NUM] 或 UINT8_MAX */

urma_jfs_wr_t *bad_wr = NULL;
urma_status_t st = bondp_post_jetty_send_wr(jetty, wr, &hint, &bad_wr);
```

| 约束 | 说明与依据 |
| --- | --- |
| **指定本端端口无效** | `mask` 中任何 `BONDP_WR_HINT_MASK_SRC_*` 位置位即返回 `URMA_EINVAL`。原因是发送接口不接受应用指定的本端端口：`active_backup` 模式下本端出口始终由 bondp 内部调度按 `active_indices` 顺序决定；`src_id` 在发送路径上也没有任何读取点（源实现：`bondp_datapath.c:880` 做方向校验，`bondp_datapath.c:1341` 是唯一读取点且位于接收接口）。如需固定本端出口，只能通过创建对象时收窄端口集合（见 3.3） |
| `hint` 不可为 NULL | 传入 NULL 返回 `URMA_EINVAL`；应用不想指定端口时可置 `mask = 0`（由 bondp 内部调度），或直接使用 `urma_post_*_wr()` |
| `mask` 必选位 | 必须置位 `DST_CHIP_ID` 与 `DST_PORT_ID`；`DST_DIE_ID` 可省（不置位时 `die_id` 按缺省 1 处理），置位时必须为 1 |
| `version` / `size` 必须合法 | `version` 必须为 `BONDP_WR_HINT_VERSION_V0`；`size` 必须与实现约定的结构大小兼容，否则 `URMA_EINVAL` |
| `wr->tjetty` 必须非空 | 为空返回 `URMA_EINVAL`（`bondp_datapath.c:875`） |
| `bad_wr` 必须非空 | 为空返回 `URMA_EINVAL` |
| `dst_id` 必须是可达的远端成员端口 | 非法/不可达/提交时已失效均返回 `URMA_EAGAIN`，由应用决定重试；`mask` 未覆盖的坐标子字段按未指定处理 |
| 本端出口不可指定 | 实际出口由调度器选择，只能从事后 CQE 观察，详见 3.3 |
| 固定端口的 WR 不改投 | 指定端口失效时返回错误 CQE，不会经由备份的远端端口重发 |

### 2.7 接收接口使用说明与约束

```c
bondp_wr_hint_t hint = {0};              /* 必须整体清零 */
hint.version = BONDP_WR_HINT_VERSION_V0;
hint.size    = sizeof(bondp_wr_hint_t);
hint.mask    = BONDP_WR_HINT_MASK_SRC_CHIP_ID |   /* 必选 */
               BONDP_WR_HINT_MASK_SRC_PORT_ID;    /* 必选 */
               /* SRC_DIE_ID 可选：不置位时 die_id 按缺省 1 处理 */
hint.src_id.bs.chip_id  = 1;             /* [1, CHIP_NUM] */
hint.src_id.bs.port_idx = 0;             /* [0, PORT_NUM] 或 UINT8_MAX */

urma_jfr_wr_t *bad_wr = NULL;
urma_status_t st = bondp_post_jetty_recv_wr(jetty, wr, &hint, &bad_wr);
```

| 约束 | 说明与依据 |
| --- | --- |
| **指定远端端口无效** | `mask` 中任何 `BONDP_WR_HINT_MASK_DST_*` 位置位即返回 `URMA_EINVAL`。接收路径不存在远端端口语义，该信息在 `post_recv` 侧永远不会生效；`dst_id` 在接收路径上也没有任何读取点（`bondp_datapath.c:1336`） |
| `src_id` 必须是本对象成员端口 | 非法坐标返回 `URMA_EINVAL`；非成员（`members[idx] == NULL`）同样返回 `URMA_EINVAL` |
| `mask` 必选位 | 必须置位 `SRC_CHIP_ID` 与 `SRC_PORT_ID`；`SRC_DIE_ID` 可省（不置位时 `die_id` 按缺省 1 处理），置位时必须为 1 |
| `version` / `size` 必须合法 | 同发送侧 |
| 端口路径不可用 → `URMA_EAGAIN` | `valid[recv_idx] == 0` 时返回 `URMA_EAGAIN`，应用需重试 |
| 钉住的接收 WR 不改投 | 端口故障时已排队的 RQE 被底层以错误 CQE 冲刷，应用需重新投递 |
| `hint` 不可为 NULL、`bad_wr` 不可为空 | 同发送侧；`mask == 0` 表示应用不指定本端端口，由 bondp 内部调度 |

---

## 3. 发送侧实现方案

### 3.1 调用链

```text
bondp_post_jetty_send_wr / bondp_post_jfs_wr
  +- bondp_post_send_wr_by_port()            # hint 方向校验、tjetty 校验
       +- bondp_post_send_wr()                # use_cnt / deleting / WR 列表校验
            +- standalone:  bondp_post_send_wr_no_store(..., hint)
            +- active_backup / balance: bondp_post_send_wr_list_and_store(..., hint)
                 +- try_failback()                                  # 发布已完成的 failback，与 hint 无关
                 +- schedule_send_wr() -> schedule_send(..., hint)
                 |      +- 远端已钉住（mask 含 DST_*）: schedule_send_fixed() -> *send_idx
                 +- 拷贝 WR、克隆 import-free user_tseg、编码 MSN
                 +- check_jfs_wr_path(wr, send_idx, target_idx)       # 路径绑定校验
                 +- convert_jfs_vwr_to_pwr(..., send_idx, target_idx) # 虚拟形态 -> 物理形态
                 +- [send_lock] 提交 entry_type / send_idx / target_idx / fixed_port
                 |              再次校验 valid[send_idx]
                 |              comp_post_send(send_idx, target_idx)
                 +- convert_jfs_pwr_to_vwr()                          # 还原虚拟形态后留存
```

关键函数职责：

| 函数 | 职责 |
| --- | --- |
| `bondp_post_send_wr_by_port` | 校验 hint 的 `version` / `size` / `mask` 方向、`wr->tjetty` 非空，转交下一层 |
| `bondp_post_send_wr` | 对象引用计数、删除中判定、WR 列表合法性校验，分流 standalone / 多设备路径 |
| `schedule_send_wr` / `schedule_send` | hint 解析与端口选择入口 |
| `schedule_send_fixed` | 在钉住远端端口的前提下选择本端出口 |
| `check_jfs_wr_path` | 校验该 (本端, 远端) 路径能否承载此 WR |
| `convert_jfs_vwr_to_pwr` | 把 SGE 映射到该路径的 `p_tseg`/token，写入远端物理 jetty 信息 |
| `comp_post_send` | 用 `p_jetty[send_idx]` / `p_jfs[send_idx]` 下发，并维护 `sqe_cnt[send_idx][target_idx]` |

### 3.2 与 pjetty / ptjetty 的链路对接

一次发送投递由两个索引共同确定：

- `send_idx`：本端物理端口索引，决定使用哪个 `p_jetty[send_idx]` / `p_jfs[send_idx]` 承载；
- `target_idx`：远端物理端口索引，决定使用 `bdp_tjetty->p_tjettys[]` 中的哪条 `p_tjetty` 作为目标。

| 步骤 | 接口 / 数据 | 作用 |
| --- | --- | --- |
| 选本端出口 | `schedule_send_fixed()` -> `*send_idx` | 选中本端物理端口 |
| 固定远端 | hint 的 `dst_id`（`mask` 需置位 `DST_*`）-> `fixed_target_idx` -> `*target_idx` | 钉住远端物理端口 |
| 取远端物理对象 | `bondp_find_p_tjetty(bdp_tjetty, local, remote)` / `bdp_tjetty->p_tjettys[k].p_tjetty` | 该路径的物理 target jetty |
| 路径绑定校验 | `check_jfs_wr_path(wr, send_idx, target_idx)` | 校验路径能否承载该 WR（含 import-free `user_tseg` 的 opcode 约束） |
| 物理化转换 | `convert_jfs_vwr_to_pwr(wr, send_idx, target_idx, ...)` | 解析该路径的 tseg/token 与远端 jetty 信息 |
| 下发 | `comp_post_send(comp, send_idx, target_idx, ...)` | Jetty 走 `urma_post_jetty_send_wr(p_jetty[send_idx])`，JFS 走 `urma_post_jfs_wr(p_jfs[send_idx])` |
| 记账 | `sqe_cnt[send_idx][target_idx]++`；`send_wr_buf` 记录 `send_idx` / `target_idx` / `fixed_port` | 供故障切换、RNR 重试与完成回收使用 |
| 完成回收 | CR 的 `user_ctx`（即 `wr_id`）反查 entry，校验 `send_idx` / `target_idx` / `entry_type` 一致 | 防止陈旧 CQE 造成重复回收 |

### 3.3 端口选择原则

| 维度 | 由谁决定 | 应用能否指定 | 失效行为 |
| --- | --- | --- | --- |
| 远端端口 `target_idx` | hint 的 `dst_id`（`mask` 含 `DST_*`），强绑定 | 能 | 无可用路径 -> `URMA_EAGAIN`；已发出后路径失败 -> 错误 CQE；**绝不改投其他远端端口** |
| 本端出口 `send_idx` | `schedule_send_fixed()`（远端已钉住）/ `schedule_send_active_backup()`（`mask == 0`，应用未指定，由 bondp 内部调度） | **不能** | 自动换到下一个可用本端端口；全都不可用才报错 |

`schedule_send_fixed()` 的本端出口算法（`bondp_datapath_schedule.c:465`）：

```text
按 comp->active_indices 顺序遍历候选本端端口 loc_idx：
    在 bdp_tjetty->p_tjettys[] 中找到第一条 local_indice == loc_idx 且 p_tjetty != NULL 的记录
    remote_indice != fixed_target_idx          -> 换下一个候选本端端口
    !p_tjetty->valid 或 !comp->valid[loc_idx]  -> 换下一个候选本端端口
    Jetty 且 p_jetty[loc_idx] == NULL          -> 换下一个候选本端端口
    否则                                       -> *send_idx = loc_idx，返回 0
所有候选均不满足 -> 返回 -1
```

原则归纳：

1. 远端端口是硬约束，本端出口是软约束（择优而非承诺）。
2. 本端出口的优先级等于对象创建时的 `active_indices` 顺序；`active_backup` 下天然是最靠前的可用端口。
3. 应用只能在 CQE 返回后用 `bondp_get_cr_local_port_id(cr)` 得知实际出口。
4. 若应用确实需要固定本端出口，当前实现的唯一途径是**创建对象时收窄端口集合**（`bondp_jetty_cfg_t.port_ids` / `bondp_jfs_cfg_t.port_ids`，置 `flag.bs.has_drv_ext`）。该能力是对象级而非 per-WR 级，且仅当该 ctx 未通过 `BONDP_USER_CTL_SET_BONDING_PORT` 配置过时才可收窄；一旦配置过，创建对象的端口集合必须与该配置逐项一致，此时只能通过 ctx 级配置整体收窄（对所有对象生效）。收窄端口集合会牺牲故障切换能力。
5. 上述算法针对"远端端口被完整钉住"（`DST_PORT_ID` 置位）的场景。若 2.1.3 决策 2 允许只置位 `DST_CHIP_ID` 等部分 mask，则需要在本函数之外新增"同一芯片内按 `active_indices` 顺序选端口"的分支，届时"固定端口"的语义需一并重新定义。

### 3.4 何时返回错误

| 阶段 | 条件 | 返回 |
| --- | --- | --- |
| 参数校验 | 对象/上下文非 bond、`wr == NULL`、`bad_wr == NULL`、comp 类型不符 | `URMA_EINVAL` |
| hint 校验 | `hint == NULL`、`version` / `size` 非法、`mask` 不含任何 `DST_*` 位、`mask` 含 `SRC_*` 位 | `URMA_EINVAL` |
| 参数校验 | `wr->tjetty == NULL` | `URMA_EINVAL` |
| 对象状态 | comp 正在删除 | `URMA_EAGAIN` |
| WR 列表校验 | 超过 `BONDP_BATCH_POST_MAX_NUM`（256）、SGE 数超限、tseg 正在删除等 | `URMA_EINVAL` 等 |
| 路径选择 | `dst_id` 非法或无可寻址物理上下文 | `URMA_EAGAIN` |
| 路径选择 | 没有任何本端端口能到达该远端端口 | `URMA_EAGAIN` |
| 路径校验 | `check_jfs_wr_path()` 失败（`p_tjetty` 为空、本地 SGE 使用 `user_tseg`、跨路径 SGE 不匹配等） | `URMA_EINVAL` |
| 资源 | `jfs_wr_buf` 条目不足 | `URMA_ENOMEM` |
| 加锁后复检 | `valid[send_idx] == 0` | `URMA_EAGAIN`（WR 被释放回收，不换端口重试） |
| 下发 | `urma_post_*_wr()` 返回失败 | 原始错误码直接上抛，不做 3 次换端口重试（`bondp_datapath.c:814`） |

对比说明：无 hint 投递在 `valid[send_idx] == 0` 或调度失败时会重试最多 `BONDP_POST_SEND_MAX_RETRY`（3）次并可能更换端口；**带 hint 的投递把重试职责交给应用**，这是"端口亲和性换可用性"的直接体现。

补充说明：`mask == 0` 表示应用不指定端口、由 bondp 内部调度（见 2.1.3 决策 1），因此它不具备上述"钉住"语义，不会进入 `fixed_port` 分支，其重试与换端口行为与 `urma_post_*_wr()` 完全一致。

### 3.5 运行期故障与恢复对固定端口 WR 的影响

`fixed_port` 字段（`bondp_wr_buf.h` 中 `jfs_wr_entry.fixed_port`）的写入与使用：

```c
/* 提交时写入：只有钉住远端端口的 WR 才是 fixed_port */
wr_entries[i]->fixed_port = (hint != NULL &&
                             (hint->mask & BONDP_WR_HINT_MASK_DST_PORT_ID) != 0);
```

`fixed_port` 的判定与"钉住的粒度"强相关：本文按"`DST_PORT_ID` 置位即视为远端端口被完整钉住"实现。若 2.1.3 决策 2 允许仅钉到芯片级（只置位 `DST_CHIP_ID`），需要重新定义该字段语义（例如新增"允许在同芯片内改投"的中间态），否则会出现"只钉芯片却按钉端口禁用改投"的行为偏差。

| 位置 | 行为 |
| --- | --- |
| `bondp_datapath.c:1458` `resend_matched_jfs_wrs()` | 跳过 `fixed_port && new_target_idx != old_target_idx` 的条目 |
| `bondp_datapath.c:1792` failover CR 处理 | 走 `schedule_send_fixed()`，保持 `target_idx` 不变，只换 `send_idx` |
| `bondp_datapath.c:377` `failback_resend_wr()` | `fixed_port` 条目直接跳过，不参与 failback 重路由 |
| `bondp_datapath.c:751` 等 | 写入与清理 |

自上而下看，没有任何一处会把 `fixed_port` 的 WR 重新调度到另一个 `target_idx`。

故障发生后的状态流转：

```text
底层 CR 属于 {LOC_ACCESS_ERR, LOC_LEN_ERR, ACK_TIMEOUT_ERR}    # is_failover_cr
 +- valid[send_idx] = false
 +- LOC_ACCESS_ERR -> port_status_bad[send_idx] = true          # 供 QUERY_PORT_STATUS 查询
 +- p_tjetty(local, remote).valid = false（含健康检查状态同步）
 +- fixed_port WR：schedule_send_fixed() 只换本端出口；换不到 -> 错误 CQE 交给应用
 +- 非 fixed_port WR：schedule_send() 可能连远端端口一起更换
 +- 触发 failback 任务（+2000ms 重建该本端 pjetty）-> rebuild_done
      -> 健康探测成功 -> hc_valid -> 下一次 send post 的 try_failback() 中恢复 valid[send_idx]
```

需要注意：`valid[]` 的恢复发布**只发生在发送投递路径**（`try_failback()` 仅在 `bondp_post_send_wr_list_and_store()` 中被调用），且依赖 `enable_health_check` 与 `enable_failback` 同时使能。

---

## 4. 接收侧实现方案

### 4.1 调用链

```text
bondp_post_jetty_recv_wr / bondp_post_jfr_wr
  +- bondp_post_recv_wr_by_port()        # hint 方向校验 -> recv_idx 转换 -> 成员/可用性校验
       +- bondp_post_recv_wr(..., fixed_recv_idx = recv_idx)
            +- comp 删除中 -> URMA_EAGAIN
            +- post_recv_check_wr_list_valid()
            +- standalone:  bondp_post_recv_wr_no_store(..., fixed_recv_idx)
            +- active_backup / balance:
                 +- msn_enable  -> bondp_post_recv_wr_list_and_store(..., fixed_recv_idx)
                 +- 否则        -> bondp_post_recv_wr_list_without_backup(..., fixed_recv_idx)
```

`bondp_post_recv_wr_by_port` 的校验顺序：

```text
1. hint != NULL && version / size 合法 && mask 非 0 且只含 SRC_* 位，
   并且含 SRC_CHIP_ID + SRC_PORT_ID（SRC_DIE_ID 可选）                 否则 URMA_EINVAL
2. convert_bond_port_id_to_active_index(ctx, hint->src_id, &recv_idx)  失败 URMA_EINVAL
3. recv_idx < URMA_UBAGG_DEV_MAX_NUM && comp->members[recv_idx] != NULL   否则 URMA_EINVAL
4. atomic_load(&comp->valid[recv_idx]) == true                   否则 URMA_EAGAIN
5. bondp_post_recv_wr(comp, wr, bad_wr, (int)recv_idx)
```

### 4.2 与 pjetty 的链路对接

固定端口投递在两条多设备路径中的处理完全一致：

```c
if (fixed_recv_idx >= 0) {
    recv_wr_cnt[fixed_recv_idx] = (uint32_t)wr_count;  /* 整批 WR 都投到该端口 */
    post_indices = fixed_indices;                      /* 覆盖为单元素数组 */
    post_index_count = 1;                              /* 绕过 active_indices 调度 */
}
```

随后对每个 WR：

```c
wr_entry->recv_idx = (uint32_t)recv_idx;               /* 记账，CR 回收时使用 */
copy_jfr_wr(cur, pwr, ...);                            /* 拷贝到 entry 内的固定 slot */
convert_jfr_vwr_to_pwr(pwr, recv_idx);                 /* 绑定该索引的物理接收对象 */
comp_post_recv(bdp_comp, recv_idx, &wr_entries[k]->wr, &bad_pwr, recv_cnt);
```

`comp_post_recv()` 的下发目标：

```c
comp->comp_type == BONDP_COMP_JETTY ? urma_post_jetty_recv_wr(comp->p_jetty[recv_idx], ...)
                                    : urma_post_jfr_wr(comp->p_jfr[recv_idx], ...);
get_recv_count_comp(comp)->rqe_cnt[recv_idx] += wr_count;   /* 仅成功部分累加 */
```

接收侧的端口选择最终落地为两件事：`post_indices` 收敛为单元素、`convert_jfr_vwr_to_pwr()` 的索引固定为该端口。**不经过 `schedule_recv()` / `schedule_recv_n()`，也没有任何重路由逻辑。**

### 4.3 端口选择原则

| 维度 | 由谁决定 | 应用能否指定 | 说明 |
| --- | --- | --- | --- |
| 本端接收端口 `recv_idx` | hint 的 `src_id`（`mask` 含 `SRC_*`），强绑定 | 能 | 整批 WR 全部投到该端口对应的 `p_jfr` / `p_jetty` |
| 远端端口 | **不存在该概念** | 不能 | 接收路径没有 target 语义，传入的对端端口信息在 `post_recv` 侧永远不会生效：`dst_id` 不被读取，`mask` 置 `DST_*` 位属非法参数并返回 `URMA_EINVAL` |

对比说明：无 hint 时（`fixed_recv_idx == -1`）走 `schedule_recv()`；`active_backup` 下取第一个可用本端端口，`balance` 下按 `rqe_cnt` 最少者分配（`schedule_recv_n` 支持多端口分摊）。

### 4.4 使用约束与错误码

| 条件 | 返回 |
| --- | --- |
| 对象/上下文非 bond、`wr == NULL`、`bad_wr == NULL`、comp 类型不符 | `URMA_EINVAL` |
| `hint == NULL`、`version` / `size` 非法、`mask` 不含任何 `SRC_*` 位、`mask` 含 `DST_*` 位 | `URMA_EINVAL` |
| `src_id` 无法转换为 active index（含 `chip_id` / `die_id` / `port_idx` 非法、物理上下文不存在） | `URMA_EINVAL` |
| `recv_idx` 超范围或 `comp->members[recv_idx] == NULL`（非本对象成员端口） | `URMA_EINVAL` |
| `valid[recv_idx] == 0` | `URMA_EAGAIN` |
| comp 正在删除 | `URMA_EAGAIN` |
| WR 列表越界、SGE 数超限 | `URMA_EINVAL` |
| recv WR buffer 不足 | `URMA_ENOMEM` |
| 底层 `urma_post_*_recv_wr()` 失败 | 原始错误码，已接收部分通过 `bad_wr` 与 `rqe_cnt` 补偿 |

行为约束：

1. 钉住的接收 WR 不会被改投到其他本端端口；端口故障时已排队的 RQE 由底层以错误 CQE 冲刷，应用需要重新投递。
2. 端口未恢复前重新投递会持续返回 `URMA_EAGAIN`；恢复判定与发送侧共用同一套 `valid[]`（见 3.5）。
3. 接收侧使用 `recv_wr_buf` 记账：entry 中记录 `recv_idx`，CQE 到达时以 `user_ctx`（即 `wr_id`）反查 entry，扣减 `rqe_cnt[recv_idx]`；MSN 去重逻辑判定"窗口外/重复"时，用同一个 `recv_idx` 重新投递该 WR 以 rearm，**rearm 不改变端口绑定**。

### 4.5 接收 CQE 的端口上报

处理链路（以有备份的多设备路径为例）：

```c
convert_pcr_to_vcr(cr, bdp_ctx, &msn);      /* 物理 CR -> 虚拟 CR，vjetty_id 字段被取出后清零 */
bondp_stamp_recv_port(cr, idx);             /* idx = 产生该 CQE 的 p_jfc 索引 = 物理端口索引 */
/* bondp_stamp_recv_port 内部： */
/*   bondp_port_id_t port_id = bondp_idx_to_port_id(idx);                        */
/*   cr->imm_data |= (uint64_t)port_id.value << BONDP_IMM_PORT_ID_SHIFT;         */
cr->local_id = recv_comp->v_jetty.jetty_id.id;   /* 还原虚拟 jetty id */
```

该编码对 bonding 上下文的所有接收 CQE 生效（包含普通调度接口投递的接收 WR）。

应用侧读取方式（同时覆盖 TX 与 RX）：

```c
bondp_port_id_t p = bondp_get_cr_local_port_id(cr);
if (p.value != UINT16_MAX) {
    /* p.bs.chip_id / p.bs.die_id / p.bs.port_idx 为该 CQE 对应的物理端口 */
}
```

端口状态查询通道：`urma_user_ctl(BONDP_USER_CTL_QUERY_PORT_STATUS)` 返回每个 enabled 端口的 `GOOD` / `BAD`。其中 `BAD` 仅由本端 `LOC_ACCESS_ERR` 置位，并且需要健康探测成功、`rebuild_done` 被消费后才清除。

---

## 5. 调用示例

### 5.1 发送：钉住远端端口，本端出口由 bond 决定

```c
bondp_wr_hint_t hint = {0};
hint.version = BONDP_WR_HINT_VERSION_V0;
hint.size    = sizeof(bondp_wr_hint_t);
hint.mask    = BONDP_WR_HINT_MASK_DST_CHIP_ID |
               BONDP_WR_HINT_MASK_DST_PORT_ID;   /* DST_DIE_ID 可选 */
hint.dst_id.bs.chip_id  = 1;
hint.dst_id.bs.port_idx = UINT8_MAX;   /* primary EID */

urma_jfs_wr_t *bad_wr = NULL;
urma_status_t st;
do {
    st = bondp_post_jetty_send_wr(jetty, wr, &hint, &bad_wr);
    if (st == URMA_EAGAIN) {
        usleep(1000);   /* 路径未就绪（含指定端口失效/未恢复）：退避后重投同一 hint */
    }
} while (st == URMA_EAGAIN);
```

### 5.2 接收：钉住本端端口，远端端口在接收路径上无效

```c
bondp_wr_hint_t hint = {0};
hint.version = BONDP_WR_HINT_VERSION_V0;
hint.size    = sizeof(bondp_wr_hint_t);
hint.mask    = BONDP_WR_HINT_MASK_SRC_CHIP_ID |
               BONDP_WR_HINT_MASK_SRC_PORT_ID;   /* SRC_DIE_ID 可选 */
hint.src_id.bs.chip_id  = 1;
hint.src_id.bs.port_idx = UINT8_MAX;

urma_jfr_wr_t *bad_wr = NULL;
urma_status_t st = bondp_post_jetty_recv_wr(jetty, &wr, &hint, &bad_wr);
```

### 5.3 观测实际端口

```c
urma_cr_t cr;
int n = urma_poll_jfc(jfc, 1, &cr);
if (n > 0) {
    /* TX CQE：实际发送出口；RX CQE：实际接收入口 */
    bondp_port_id_t actual = bondp_get_cr_local_port_id(&cr);
}
```

### 5.4 应用不指定端口（mask = 0，由 bondp 内部调度）

替换后 `mask == 0` 表示"应用不指定端口，由 bondp 内部调度"（由 liburma_ubagg 的调度器按 bonding 模式选择端口），与 `urma_post_*_wr()` 行为一致，可用于在同一套接口内切换"钉端口"与"交给 bond"两种模式：

```c
bondp_wr_hint_t hint = {0};
hint.version = BONDP_WR_HINT_VERSION_V0;
hint.size    = sizeof(bondp_wr_hint_t);
hint.mask    = 0;   /* 应用不指定端口，由 bondp 内部调度 */

urma_jfs_wr_t *bad_wr = NULL;
urma_status_t st = bondp_post_jetty_send_wr(jetty, wr, &hint, &bad_wr);
```

---

## 6. 实现注意点

以下为走读过程中记录的事实性观察，建议与提交作者确认设计意图。

1. **`imm_data[22, 37]` 双写**：`bondp_stamp_recv_port()` 写入打包后的 `bondp_port_id_t`，但 CR 返回给应用之前，`bondp_set_cr_port_idx(pcr, idx)`（`bondp_datapath.c:2025` / `bondp_datapath.c:2130`）会用裸 active index 覆盖同一段位（`IMM_VJETTY_ID_SHIFT` 同为 22、宽度同为 16）。因此按文档直接对 `imm_data` 掩位解析得到的是索引而非完整 `bondp_port_id_t`。应用侧应统一改用 `bondp_get_cr_local_port_id()`。
2. **`valid[]` 的恢复依赖发送投递**：`try_failback()` 只在 `bondp_post_send_wr_list_and_store()` 中调用。纯接收应用在端口故障恢复后可能长期收到 `URMA_EAGAIN`，需要在同一对象上保持一定的发送投递。
3. **健康检查只覆盖 JETTY 类型条目**：`hc_set_local_idx_jettys_hc_valid()` 过滤 `item->key.type != JETTY`，纯 JFS/JFR 对象的 `hc_valid[]` 不会被置位，因而不会经由 `try_failback()` 发布端口恢复。
4. **`schedule_send_fixed()` 内层 `break` 语义**：每个本端候选只考察 `p_tjettys[]` 中第一条属于它的记录。1:1 映射（`active_backup` 常态）下无影响；一个本端端口对应多个远端端口时可能提前跳过合法候选，表现为 `URMA_EAGAIN`，但不会误投到其他远端端口。
5. **带 hint 的发送关闭了 3 次换端口重试**：重试策略完全由应用承担，应用需要把 `URMA_EAGAIN` 当作"路径暂不可用"处理。
6. **`mask` 粒度与 `fixed_port` 语义耦合**：`fixed_port` 决定"是否禁止改投其他远端端口"，当前实现等价于"远端端口被完整钉住"。若替换后允许只置位 `DST_CHIP_ID` 等部分 mask，必须同步定义"钉芯片是否算固定端口"，否则会出现行为与命名不符。
7. **`version` / `size` 与头库一致性**：`size` 反映的是"调用方头文件中的结构大小"，实现侧必须按 `size` 限制读取范围；特性宏 `BONDP_HAS_POST_JETTY_SEND_WR_HINT` 只能用于 `#ifdef`/`#if defined()` 判断，它反映**头文件**能力，不代表运行时库版本，头库混装时仍需依赖 `version` / `size` 校验兜底。

---

## 7. 附录

### 7.1 关键常量

| 常量 | 值 | 说明 |
| --- | --- | --- |
| `BONDP_MAX_SGE_NUM` | 32 | 单个 WR 的 SGE 上限 |
| `BONDP_MAX_WR_LIST_NUM` | 256 | WR 链长度上限 |
| `BONDP_BATCH_POST_MAX_NUM` | 256 | 批量投递上限 |
| `BONDP_POST_SEND_MAX_RETRY` | 3 | 无 hint 发送时的换端口重试次数 |
| `BONDP_IMM_PORT_ID_SHIFT` | 22 | 端口信息在 `imm_data` 中的起始位 |
| `BONDP_FB_REBUILD_DELAY_MS` | 2000 | failback 重建 pjetty 的延时 |
| 健康探测默认周期 | 1000 ms | `BONDP_HC_DEFAULT_PROBE_INTERVAL_MS` |
| `CHIP_NUM` / `PORT_NUM` | 2 / 9 | 芯片数与单芯片端口数 |

### 7.2 主要改动文件

| 文件 | 改动 |
| --- | --- |
| `src/urma/lib/urma/bond/include/bondp_api.h` | 新增：hint 结构体、4 个 API、`imm_data` 端口宏 |
| `src/urma/lib/urma/bond/bondp_datapath.c` | hint 收发实现、`schedule_send_fixed` 调用、`fixed_port` 记账、CQE 端口上报、failover/failback 适配 |
| `src/urma/lib/urma/bond/bondp_datapath_schedule.c` / `.h` | `schedule_send()` 增加 hint 参数、新增 `schedule_send_fixed()` |
| `src/urma/lib/urma/bond/bondp_provider_ops.c` / `.h` | ops 指向 `_sched` 版本、新增 `bondp_is_bond_ctx()` |
| `src/urma/lib/urma/bond/bondp_wr_buf.h` | 新增 `jfs_wr_entry.fixed_port` |
| `src/urma/lib/urma/bond/bondp_datapath.h` | 投递接口改名 `*_sched` |
| `src/urma/lib/urma/bond/CMakeLists.txt`、`src/urma/urma_bazel.sh`、`umdk.spec` | `bondp_api.h` 安装与打包 |
| `doc/ch/urma/URMA API Guide.ch.md`、`doc/en/urma/URMA API Guide.md` | 新增"Bond 按端口投递 WR"章节与端口上报说明 |

### 7.3 bondp_path_hint_t -> bondp_wr_hint_t 替换落地清单

替换属"同分支内未发布 API 的类型收敛"，不涉及兼容层。落地改动点如下：

| 文件 | 替换前 | 替换后 |
| --- | --- | --- |
| `src/urma/lib/urma/bond/include/bondp_api.h` | `typedef struct bondp_path_hint {...} bondp_path_hint_t;`；4 个原型用 `bondp_path_hint_t *hint` | 改为 `bondp_wr_hint_t`：新增 `BONDP_WR_HINT_VERSION_V0`、`bondp_wr_hint_mask_t`、`BONDP_WR_HINT_MASK_ALL`、`BONDP_HAS_POST_JETTY_SEND_WR_HINT`；4 个原型改用 `bondp_wr_hint_t *hint` |
| `src/urma/lib/urma/bond/bondp_datapath.c` | 10 处引用：`bondp_post_send_wr_by_port()` / `bondp_post_recv_wr_by_port()` / `bondp_post_jfs_wr()` / `bondp_post_jetty_send_wr()` / `bondp_post_jfr_wr()` / `bondp_post_jetty_recv_wr()` 签名，`hint->flag.bs.has_dst_port` / `has_src_port` 方向校验，`hint->dst_port_id` / `hint->src_port_id` 取值，`fixed_port` 写入 | 签名改为 `bondp_wr_hint_t *`；方向校验改为 `mask` 位判断；取值改为 `hint->dst_id` / `hint->src_id`；`fixed_port` 改为按 `BONDP_WR_HINT_MASK_DST_PORT_ID` 判断；新增 `version` / `size` 校验 |
| `src/urma/lib/urma/bond/bondp_datapath_schedule.c` | `schedule_send()` 形参 `bondp_path_hint_t *hint`，读取 `hint->dst_port_id` | 形参改为 `bondp_wr_hint_t *hint`，读取 `hint->dst_id`；`mask` 未置任何 `DST_*` 时返回调度结果 |
| `src/urma/lib/urma/bond/bondp_datapath_schedule.h` | `#include "bondp_api.h"` + `schedule_send(..., bondp_path_hint_t *hint)` 声明 | 类型改名 |
| `doc/ch/urma/URMA API Guide.ch.md` | 5 处（接口原型、`bondp_path_hint_t` 说明、字段置位规则、错误码说明） | 全部改为 `bondp_wr_hint_t`，并补充 `version` / `size` / `mask` 使用要求 |
| `doc/en/urma/URMA API Guide.md` | 5 处（同上） | 同上 |
| `doc/ch/urma/bond-port-wr-design.md` | 本文档 | 已按 `bondp_wr_hint_t` 更新 |

替换后的自检项：

1. 全仓检索 `bondp_path_hint_t` 应无残留（含文档、示例、测试用例）。
2. 调用方示例必须同时设置 `version`、`size`、`mask` 三项，且 `mask` 至少包含 `CHIP_ID` 与 `PORT_ID`（`DIE_ID` 可省）；缺必选位即退回 `URMA_EINVAL`。
3. 发送接口置位任何 `SRC_*`、接收接口置位任何 `DST_*` 必须被拒绝。
4. `mask == 0` 的投递必须与 `urma_post_*_wr()` 落到同一调度路径，且不产生 `fixed_port` 记账。
5. 旧类型名若在分支内其它提交/评审材料中被引用，需要一并更新，避免同一分支内出现两套命名。
