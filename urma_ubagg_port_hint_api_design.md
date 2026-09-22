# URMA ubagg 发送端口 hint 接口设计（草案）

> 状态：设计草案，待评审
> 相关文件：`umdk/src/urma/lib/urma/bond/include/urma_ubagg.h`、`bondp_datapath.c`、`bondp_datapath_schedule.c`、`bondp_cp_jetty.c`、`bondp_cp_tjetty.c`
> 适用范围：urma_ubagg（bond provider）balance 模式

## 1. 背景

现有 `urma_post_jetty_send_wr()` 由 bondp 内部按均衡策略（least-load + affinity）选择本端物理端口和远端物理端口，应用无法参与选路。某些外部应用已经通过 UVS 查询到本端/对端的 chip、die、port 信息，希望在自己的发包路径上表达一次选路意图。

本次新增一个发送接口，在保持 `urma_post_jetty_send_wr()` 语义不变的前提下，额外接收一份"端口 hint"，让应用指定本端（src）和对端（dst）的 chip/die/port。

## 2. 目标与范围

目标：

- 新增一个面向外部应用的发送 API，签名与 `urma_post_jetty_send_wr` 对齐，只多一个 hint 入参。
- hint 可分别指定 src、dst 两侧的 chip_id / die_id / port_idx，支持部分使能。
- hint 字段带有效标记（flag/mask），并提供后续扩展余量。
- 不改变现有 `urma_post_jetty_send_wr` 的行为与 ABI。

范围与非目标：

- 仅覆盖 `BONDP_BONDING_MODE_BALANCE`；standalone / active_backup 不在本次范围。
- 仅覆盖 jetty 发送路径；JFS 本期不做，但 hint 数据结构设计成可复用。
- 首批支持 WRITE / READ 类 opcode；SEND / ATOMIC 等先返回不支持。
- 不涉及新增健康状态查询接口、不涉及异步事件通道（回切感知走 CR，见第 8 节）。

## 3. 需求澄清结论

以下为设计前逐项确认的结论，作为后续实现与评审依据。

| # | 议题 | 结论 |
| --- | --- | --- |
| 1 | hint 的约束强度 | hint 不是硬绑定：优先按应用配置选路，bondp 有权纠正 |
| 2 | 部分使能 | 支持；未使能的字段回退到 bondp 内部调度 |
| 3 | 空值语义 | `hint == NULL` 或 `mask == 0` 等价于现有 `urma_post_jetty_send_wr` |
| 4 | src/dst 承载方式 | 一个外层结构体，内含 src 与 dst 两侧信息 |
| 5 | 坐标含义 | chip/die/port 取自 UVS 查询结果，不是 bondp 内部映射后的 index |
| 6 | 非法参数 | 报错（空指针、版本不支持、reserved 非 0、取值越界等） |
| 7 | 路径不可用 | 本端 port 合法时纠正，并记录 warn 日志 |
| 8 | 采纳优先级 | 默认优先采纳应用配置；负载均衡、优先级可以覆盖 |
| 9 | 回切场景 | 必须纠正（否则应用看到的可用端口只减不增，最终无端口可用） |
| 10 | 回切感知 | 应用通过 `urma_cr` 的 imm 感知本条 CR 实际使用的端口，不需要新增查询/事件接口 |
| 11 | bondp 内部重传 | balance 模式内部不主动重传，故 hint 不需要跨 post 保存复用 |
| 12 | 调用方 | 外部应用直接链接 `liburma_ubagg.so`，非 UMQ 等内部框架 |
| 13 | 模式/组件 | 仅 balance 模式、仅 jetty；JFS 优先级低但结构体要可复用 |
| 14 | opcode | 先覆盖 WRITE / READ，其余先返回不支持 |

## 4. 现状分析

### 4.1 已有 hint 通道（chip 级）

`bondp_jfs_wr_t` 已经是"WR + hint"的形态：

```c
typedef struct bondp_jfs_wr {
    urma_jfs_wr_t base;
    uint32_t src_chip_id;
    uint32_t dst_chip_id;
} bondp_jfs_wr_t;
```

应用通过 `wr->flag.bs.has_drv_ext = 1` 让 bondp 从 `CONTAINER_OF_FIELD` 取回这两个字段；`urma_write_affinity()` 就是这条通道的封装。限制是：只有 chip 粒度、且 hint 挂在 WR 自身（因此只能在 liburma 内部构造 WR 的场景使用），本需求要的是 die/port 粒度且由应用传入。

### 4.2 调度路径

```
bondp_post_jetty_send_wr / bondp_post_send_wr_list_and_store
  -> post_send_check_wr_list_valid            (含 chip_id 范围校验)
  -> schedule_send_wr(wr, comp, &send_idx, &target_idx)
  -> schedule_send(tjetty, comp, ..., info)   (按 mode 分发)
  -> schedule_send_balance(..., info)
       -> select_affinity_path()              (IODIE 级用 chip 优先级；PORT 级用 failover_route)
       -> filter_least_load_path() / select_random_path()
  -> comp_post_send(comp, send_idx, target_idx, ...)
```

一次调用（WR 链）只解析出一组 `(send_idx, target_idx)`，链上所有 WR 共用同一路径。这一点与"hint 作为一次调用的入参"天然吻合。

### 4.3 故障与回切

- 故障方向：CR 上出现 `is_failover_cr` 时，`valid[send_idx] = false`，`URMA_CR_LOC_ACCESS_ERR` 时置 `port_status_bad[send_idx] = true`，对应 `p_tjetty->valid` 置 false；满足条件时投递 failback 任务。
- 回切方向：健康检查按 `[local_idx, target_idx]` 路径恢复，重建完成后 `hc_valid[local_idx] = true` 且 `port_status_bad[local_idx] = false`。
- balance 模式下不在故障时主动重传，下一次 post 才重新选路；`try_failback()` 在 store 路径的 post 前调用，可能把已在 WR ring 里的 WR 改投到恢复后的路径。

### 4.4 CR 上的端口回传

CR 转换后，imm 的 vjetty_id 字段被复用为本端 port index：

```c
void bondp_set_cr_port_idx(urma_cr_t *cr, uint32_t port_idx);
uint32_t bondp_get_cr_port_idx(const urma_cr_t *cr);
```

应用侧已有 `bondp_get_cr_local_port_id(cr)` 把它转成 `bondp_port_id_t`（chip/die/port）。这是回切感知的落点：应用只要看 CR，就能知道这条 WR 实际落在哪个本端端口。

### 4.5 对外暴露方式

- `liburma_ubagg.so` 是独立共享库，`urma_ubagg.h` 安装到 `/usr/include/ub/umdk/urma/`。
- `urma_write_affinity()` 即定义在 bond 目录、由该库导出，外部应用直接链接使用，不改动 urma core 的 ops 表。
- 新接口按同样方式落地，不需要 core 侧新增 provider op。

## 5. 现有结构体评价

当前新增的两个结构体：

```c
typedef union bondp_port_id_flag {
    struct {
        uint16_t has_chip_id : 1;
        uint16_t has_die_id  : 1;
        uint16_t has_port_id : 1;
        uint16_t reserved    : 13;
    } bs;
    uint16_t value;
} bondp_port_id_flag_t;

typedef struct bondp_port_info {
    bondp_port_id_flag_t flag;
    bondp_port_id_t port_id;
} bondp_port_info_t;
```

`bondp_port_id_t`（chip 4bit / die 4bit / port 8bit + `value`）建议保留：与 `BONDP_USER_CTL_SET_BONDING_PORT`、`BONDP_USER_CTL_QUERY_PORT_STATUS` 使用同一套 UVS 坐标，`bondp_active_index_to_port_id()` 的输出可以原样回传，取值空间自洽。

`bondp_port_id_flag_t` 建议改造：把"哪些字段有效"从"每侧一个 16bit 位域 union"收敛为外层一个 64bit mask，理由：

- 与现有 provider 风格一致（`bondp_set_ctx_cfg_in.mask`、`urma_bond_jetty_ext.mask`、`bondp_rjetty_ext_*` 都是 mask 决定字段有效性）。
- src/dst 两侧六个字段可以放进同一个 64bit 字，后续加 `has_eid` / `has_vjetty_id` / `has_path_id` 等只是加 bit，结构体布局不变。
- 位域 union 是 ABI 可见布局，跨编译器/跨版本风险高于裸 `uint64_t`；且两侧各留 13bit 会让扩展位分散。

## 6. 接口设计

### 6.1 数据结构

```c
#define BONDP_WR_PORT_HINT_VERSION_V0 0

/* 存在该宏即表示当前 liburma_ubagg 支持带 port hint 的发送接口。
 * 仅用于 #ifdef / #if defined() 判断，不要用 #if BONDP_HAS_... 判断取值。 */
#define BONDP_HAS_POST_JETTY_SEND_WR_PORT_HINT BONDP_HAS_POST_JETTY_SEND_WR_PORT_HINT

typedef enum bondp_wr_port_hint_mask {
    BONDP_WR_PORT_HINT_MASK_SRC_CHIP_ID = 1ULL << 0,
    BONDP_WR_PORT_HINT_MASK_SRC_DIE_ID  = 1ULL << 1,
    BONDP_WR_PORT_HINT_MASK_SRC_PORT_ID = 1ULL << 2,
    BONDP_WR_PORT_HINT_MASK_DST_CHIP_ID = 1ULL << 3,
    BONDP_WR_PORT_HINT_MASK_DST_DIE_ID  = 1ULL << 4,
    BONDP_WR_PORT_HINT_MASK_DST_PORT_ID = 1ULL << 5,
} bondp_wr_port_hint_mask_t;

#define BONDP_WR_PORT_HINT_MASK_ALL \
    (BONDP_WR_PORT_HINT_MASK_SRC_CHIP_ID | BONDP_WR_PORT_HINT_MASK_SRC_DIE_ID | \
     BONDP_WR_PORT_HINT_MASK_SRC_PORT_ID | BONDP_WR_PORT_HINT_MASK_DST_CHIP_ID | \
     BONDP_WR_PORT_HINT_MASK_DST_DIE_ID  | BONDP_WR_PORT_HINT_MASK_DST_PORT_ID)

typedef struct bondp_wr_port_hint {
    uint32_t version;             /* 必须为 BONDP_WR_PORT_HINT_VERSION_V0 */
    uint32_t size;                /* 调用方头文件中的 sizeof(bondp_wr_port_hint_t) */
    uint64_t mask;                /* 0 表示不指定，完全走 bondp 内部调度 */
    bondp_port_id_t src_port_id;  /* 各子字段是否有效由 SRC_* 位决定 */
    bondp_port_id_t dst_port_id;  /* 各子字段是否有效由 DST_* 位决定 */
    uint64_t reserved[4];         /* 必须为 0，预留给后续扩展 */
} bondp_wr_port_hint_t;
```

设计取舍：

- 未使能的子字段取值一律忽略，因此"不指定 port_idx"与"port_idx = 0"不会混同。
- `chip_id` 的合法范围是 `[1, CHIP_NUM]`，未使能时遗留的 0 值天然不可能被误判为合法值。
- 两级扩展余量：bit 级余量在 `mask`（64bit 里还剩 58 个 bit），结构体级余量在 `reserved[4]`。
- `version` 只在不兼容语义变更时递增；`size` 负责前向兼容，provider 只读 `min(size, sizeof)` 的部分。

### 6.2 函数原型

```c
urma_status_t urma_post_jetty_send_wr_with_port_hint(urma_jetty_t *jetty, urma_jfs_wr_t *wr,
                                                     urma_jfs_wr_t **bad_wr,
                                                     const bondp_wr_port_hint_t *hint);
```

不修改 `urma_post_jetty_send_wr` 的签名（避免 ABI 破坏），也不改动 urma core 的 `urma_provider_ops_t`；新符号由 `liburma_ubagg.so` 导出，与 `urma_write_affinity` 一致。

### 6.3 使用示例

```c
bondp_wr_port_hint_t hint = {0};
hint.version = BONDP_WR_PORT_HINT_VERSION_V0;
hint.size    = sizeof(hint);
hint.mask    = BONDP_WR_PORT_HINT_MASK_ALL;
hint.src_port_id.bs.chip_id  = src_chip;   /* 来自 UVS 查询 */
hint.src_port_id.bs.die_id   = src_die;
hint.src_port_id.bs.port_idx = src_port;
hint.dst_port_id.bs.chip_id  = dst_chip;
hint.dst_port_id.bs.die_id   = dst_die;
hint.dst_port_id.bs.port_idx = dst_port;

urma_jfs_wr_t *bad_wr = NULL;
urma_status_t ret = urma_post_jetty_send_wr_with_port_hint(jetty, wr, &bad_wr, &hint);
```

只关心本端时可只置 `SRC_*` 位，只关心对端时只置 `DST_*` 位；不需要任何 hint 时传 `NULL` 或 `mask = 0`。

## 7. 语义与规则

### 7.1 必须报错的情况

校验在进入调度前完成，不改变 WR 状态：

| 情况 | 返回值 |
| --- | --- |
| `jetty` / `wr` 为空 | `URMA_EINVAL` |
| `hint->version` 不支持 | `URMA_EINVAL` |
| `hint->size` 小于 V0 定义长度 | `URMA_EINVAL` |
| `hint->mask` 含本版本未定义的 bit | `URMA_EINVAL` |
| `hint->reserved[0..3]` 非 0 | `URMA_EINVAL` |
| 已使能字段取值越界：`chip_id ∉ [1, CHIP_NUM]`、`die_id` 非当前支持值、`port_idx > PORT_NUM` | `URMA_EINVAL` |
| 本端已使能字段无法映射到本 jetty 的使能端口集合（见 12.1 待确认） | `URMA_EINVAL` |
| opcode 非 WRITE / READ 且 `mask != 0` | 待确认（当前可选 `URMA_EINVAL` 或 `URMA_FAIL`） |
| 非 balance 模式且 `mask != 0` | 待确认（同上） |

注：现有 `urma_status_t` 只有 `URMA_EINVAL` / `URMA_ENOMEM` / `URMA_FAIL` 等，没有 `ENOTSUP`，不建议为此改公共状态码枚举。

### 7.2 纠正并继续发送的情况

以下情况"取值在正常范围内，但应用不可感知"，bondp 纠正后继续下发，并打 warn 日志：

- src 合法但该本端端口当前 `valid == false`（health BAD、`p_jetty` 为空、RC 传输模式下 `remote_jetty` 与 `p_tjetty` 不匹配）。
- dst 合法但对端该 target 不在本 tjetty 的 slave 集合内、`connected == false` 或 `p_tjetty` 不可用。
- src/dst 组合只有 failover 路由可达（`select_path_by_chip` 能命中，但直接路径不可用）。
- **回切场景**：bondp 已恢复本端端口，而应用 hint 指向另一个端口时必须纠正，否则应用观察到的可用端口集合只减不增。
- 因负载均衡/优先级需要偏离 hint（日志级别见 12.4）。

### 7.3 采纳优先级

1. 应用 hint（使能字段）优先；
2. hint 对应的路径不可用、或受负载/优先级影响时，bondp 可以收窄或放宽候选集；
3. 回切必须纠正，属于第 2 条中的强制项；
4. hint 缺省维度（未使能字段）完全由 bondp 内部调度决定。

建议的 warn 日志字段：`vjetty_id`、`tjetty_id`、`opcode`、`hint(mask, src_port_id, dst_port_id)`、`resolved(local_idx→port_id, target_idx)`、`reason`。

## 8. 回切感知方案

这是设计阶段暴露出的主要难点：应用自己指定端口后，如何知道故障端口已回切以便重新使用。

结论：由 bondp 侧兜底纠正，应用侧通过 CR 感知，不新增查询接口或事件通道。

链路如下：

1. 健康检查恢复某条 `[local_idx, target_idx]` 路径后，`hc_valid[local_idx] = true`、`port_status_bad[local_idx] = false`。
2. 应用下一次 post 时即使 hint 指向故障端口，bondp 也会改投到已恢复的端口（7.2 的强制纠正项），并打 warn 日志。
3. 该 WR 的 CR 通过 imm 回传实际使用的本端 port index（`bondp_set_cr_port_idx`），应用用 `bondp_get_cr_local_port_id()` 即可读到真实的 chip/die/port。
4. 应用据此把该端口重新纳入候选集合，避免可用端口只减不增。

补充说明：

- 应用不做纠正逻辑也不影响正确性，仅是"更快重新使用恢复端口"。
- 应用若希望在 post 之前就知道端口状态，仍可轮询 `BONDP_USER_CTL_QUERY_PORT_STATUS`（现有能力，本次不改）。
- 当前 CR 只回传本端端口，未回传对端 target 端口；若应用需要感知"实际对端端口"，本次不覆盖（见 12.5）。

## 9. 实现落点

### 9.1 新增/修改点

| 位置 | 内容 |
| --- | --- |
| `bond/include/urma_ubagg.h` | 新增 `bondp_wr_port_hint_t`、mask 枚举、版本宏、能力宏与函数声明 |
| `bondp_datapath.c` | 新增 API 实现（参照 `urma_write_affinity` 的位置）；hint 校验与归一化 |
| `bondp_datapath.c` | 现有 post 入口保持签名不变，新增带 filter 的内部入口，二者共用同一套拷贝/编码/下发逻辑 |
| `bondp_datapath_schedule.c/h` | `schedule_send*` 增加 filter 参数；balance 分支先尝试 hint 指定的路径对 |
| 可选：新建 `bondp_dp_port_hint.c/h` | 把校验、坐标转换、日志收敛到独立文件（CMake 用 `aux_source_directory`，新文件自动纳入） |

### 9.2 内部归一化

```c
typedef struct bondp_path_filter {
    uint32_t mask;        /* 本端/对端是否已解析出有效 index */
    uint32_t local_idx;
    uint32_t target_idx;
} bondp_path_filter_t;
```

- src：复用 `convert_bond_port_id_to_active_index()` 完成 UVS 坐标 → `local_idx`（该函数已校验 chip_id、die_id、port_idx 与 `p_ctxs` 存活）。
- dst：UVS 坐标 → `target_idx` 需要新增映射（见 12.3），解析结果需同时满足"存在于本 tjetty 的 slave 集合"与 `connected[local_idx][target_idx]`。

### 9.3 调度改造

`schedule_send_balance()` 中，filter 存在时先做一次直接命中判断：

```
if (filter 指定了 src 且 local_idx 有效 && \
    filter 指定了 dst 且 p_tjetty(local_idx, target_idx) 可用 && \
    RC 模式下 remote_jetty 匹配) {
    *send_idx = local_idx; *target_idx = target_idx;
    return 0;   /* 不再走 select_random_path，保证 hint 命中时是确定性的 */
}
/* 未命中：丢弃不可用的那一维 + warn，回落到 select_affinity_path()/least-load */
```

只使能一维时，另一维仍在候选集合内按现有 least-load 逻辑挑选。多维都不成立时整份 hint 作废（warn），走原有内部调度，不返回失败。

### 9.4 与现有 chip hint 的关系

- `urma_write_affinity` 与 `wr.flag.bs.has_drv_ext` 的 chip 级 hint 保持可用，行为不变。
- 新接口的 port 级 hint 是 chip 级 hint 的收窄形式；同一次调用同时存在两种 hint 时的优先级建议在 12.6 中确认。
- 既有 `try_failback()` 在 store 路径 post 前执行，因此本次 post 的 hint 解析发生在 failback 完成之后，能看到最新的端口可用性。
- 由于 balance 模式不做内部重传，hint 不需要写入 `wr_entry->info` 供后续复用；`bondp_resend_jfs_wr` 仍按解析后的 `send_idx/target_idx` 工作。

## 10. 兼容性与版本策略

### 10.1 能力宏机制

`urma_ubagg.h` 现有写法：

```c
/* For version compatibility */
#define BONDP_HAS_DATAPATH_QUERY BONDP_HAS_DATAPATH_QUERY
```

含义：宏**存在**即代表能力存在，本质是自助定义。应用只能这样用：

```c
#ifdef BONDP_HAS_POST_JETTY_SEND_WR_PORT_HINT
    /* 该 SDK 头文件支持新接口 */
#endif
```

不能写成 `#if BONDP_HAS_...`：自助定义展开后仍是标识符，在 `#if` 求值中按 0 处理，会得到错误结论。

### 10.2 三层兼容

| 场景 | 表现 | 处理方式 |
| --- | --- | --- |
| 新应用 + 新库 | 正常 | 编译期能力宏 + 运行期 version/size 校验 |
| 老应用 + 新库 | 正常 | 新增符号不影响既有符号解析 |
| 新应用 + 老库 | 链接或加载失败 | 应用如需在旧库上运行，用 `dlopen`/`dlsym` 探测符号，或按能力宏做构建期分支 |

结构体本身按"只追加"演进：新增字段追加在 `reserved` 之前由 `size` 覆盖，或占用 `reserved`；provider 只读 `min(size, sizeof)`，并在 `version` 不匹配时返回 `URMA_EINVAL`。

## 11. 测试建议

参数与校验：

- `NULL` hint 与 `mask = 0` 行为与 `urma_post_jetty_send_wr` 完全一致。
- version/size 非法、`mask` 含未知 bit、`reserved` 非 0、chip/die/port 越界 → `URMA_EINVAL`。
- SEND / ATOMIC、非 balance 模式带 hint → 按 12.2 的结论返回错误。

功能：

- 路径健康时 hint 被采纳：用 CR imm（`bondp_get_cr_local_port_id`）核对实际本端端口等于 hint。
- 本端端口 BAD 时纠正：校验 CR 端口与 warn 日志。
- 回切场景：hint 指向 A，bondp 已恢复 B，确认实际落在 B（CR imm）且日志为 warn。
- 只给 dst、只给 src、只给 chip 等部分使能组合各一例。
- 同一个 WR 链（多 WR）共用同一解析路径。

性能：

- 带 hint 与不带 hint 的 post 路径做微基准对比，确认新增开销仅为若干分支与一次坐标转换，无额外分配与锁。

## 12. 待确认问题

1. **本端 port 在 UVS 范围内、但不在本 jetty 使能集合内**：按非法参数返回 `URMA_EINVAL`（倾向），还是纠正 + warn？端口集合由应用自己配置，应用可感知，倾向前者。
2. **错误码选择**：非 balance 模式、SEND/ATOMIC 带 hint 时用 `URMA_EINVAL` 还是 `URMA_FAIL`。当前没有 `ENOTSUP`，倾向 `URMA_EINVAL` 并正确填写 `bad_wr`。
3. **dst 坐标映射**：UVS 坐标 → `target_idx` 的实现方式。可假设两端 chip/die/port→index 公式对称，或用 UVS 查询结果里的 eid 去匹配 rjetty ext 的 slave 条目（倾向前者更简单、后者更稳健）；需确认 UVS 查询结果是否携带 eid。
4. **日志级别**：回切纠正必须 warn；负载/优先级偏离 hint 的频次可能很高，建议降为 debug，避免刷日志。
5. **对端端口回传**：是否需要让应用知道这条 WR 实际使用的对端端口。当前 CR imm 只回传本端 port index，若需要则要额外设计（本次可不做）。
6. **与 `urma_write_affinity` 的关系**：建议保留原接口，二者独立；同一次 post 同时存在 chip 级与 port 级 hint 时，建议以 port 级 hint 为准并打 warn（待确认）。
7. **命名确认**：`urma_post_jetty_send_wr_with_port_hint` / `bondp_wr_port_hint_t` / `BONDP_WR_PORT_HINT_MASK_*` 是否沿用，或改用 `_ex`、`_v2` 之类后缀。
8. **结构体形态最终确认**：单个 64bit `mask`（本方案）与"每侧独立 flag"方案的取舍，见第 5 节。
