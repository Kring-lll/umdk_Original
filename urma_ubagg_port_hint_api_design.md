# URMA ubagg 发送 hint 接口设计（草案）

> 状态：设计草案，待评审
> 相关文件：`umdk/src/urma/lib/urma/bond/include/urma_ubagg.h`、`bondp_datapath.c`、`bondp_datapath_schedule.c`、`bondp_cp_jetty.c`、`bondp_cp_tjetty.c`、`bondp_env.c`、`bondp_provider_ops.c`
> 适用范围：urma_ubagg（bond provider）balance 模式
> 代码基线：`master` = `7ab0e5ec`（rebase 后的上游最新），特性分支 `br_9_20_port` = `3aa1e952`
> 最近一次修订：随 rebase 对齐基线代码，更新 4.2 / 4.3 / 5 / 7.1 / 8 / 11 / 12，新增 7.4 与第 13 节《与基线代码的对齐记录》；6.1 命名去掉 `port`（`bondp_wr_hint_t` / `BONDP_WR_HINT_*` / `urma_post_jetty_send_wr_with_hint`，字段 `src_id` / `dst_id`）；第 9 章整章重写为《实现方案（含参考代码）》，并补充 9.2.3 的三个端口集合说明、9.3.2 的选路函数复用重构建议、9.5 日志一览、9.6 落地拆分顺序，11 节补充可执行的测试构造步骤

## 1. 背景

现有 `urma_post_jetty_send_wr()` 由 bondp 内部按均衡策略（least-load + affinity）选择本端物理端口和远端物理端口，应用无法参与选路。某些外部应用已经通过 UVS 查询到本端/对端的 chip、die、port 信息，希望在自己的发包路径上表达一次选路意图。

本次新增一个发送接口，在保持 `urma_post_jetty_send_wr()` 语义不变的前提下，额外接收一份"选路 hint"，让应用指定本端（src）和对端（dst）的 chip/die/port。

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
bondp_post_jetty_send_wr(:805) / bondp_post_jfs_wr(:835)     # bondp_datapath.c
  -> post_send_check_wr_list_valid(:325)      (含 has_drv_ext 的 chip_id 范围校验)
  -> for (retry = 0; retry < BONDP_POST_SEND_MAX_RETRY(:32); retry++)
       -> schedule_send_wr(wr, comp, &send_idx, &target_idx)(:351)
       -> schedule_send(tjetty, comp, ..., info)(schedule.c:425)   (按 bonding_mode 分发)
       -> schedule_send_balance(..., info)(schedule.c:322)
            -> select_affinity_path()(schedule.c:291)   (仅 info != NULL 时进入)
                 · IODIE 级：select_path_by_priority()  (chip 优先级；local_idx = src_chip_id - 1)
                 · PORT 级：select_path_by_chip()       (failover_route；必要时 is_in_failover_route() 过滤)
            -> filter_least_load_path()(schedule.c:119)
            -> 候选为空时回落 select_least_load_path()(schedule.c:140)  (min/max 按 bonding_level 取值域)
            -> select_random_path()(schedule.c:182)    选中最终路径
       -> 拷贝/编码完成后复查 atomic_load(&comp->valid[send_idx])，为 false 则 continue 重试
  -> comp_post_send(comp, send_idx, target_idx, ...)(:117)
```

要点（均为基线代码事实，直接影响 hint 的实现方式）：

- 一次调用（WR 链）只解析出一组 `(send_idx, target_idx)`，链上所有 WR 共用同一路径。这与"hint 作为一次调用的入参"天然吻合。
- **调度发生在重试循环内**：`send_idx/target_idx` 每轮都从 `-1` 重新解析；若 `comp->valid[send_idx]` 在拷贝之后变成 false，会 `continue` 重试（最多 `BONDP_POST_SEND_MAX_RETRY = 3` 次）。因此 hint 的解析必须幂等且每轮重做，不能跨轮缓存。
- **affinity 分支有 `info != NULL` 前置条件**：`info == NULL` 时不走 `select_affinity_path()`，直接落到 `select_least_load_path()` 的 least-load 回落；PORT 级 bonding 下若只有 port 坐标而不合成 `info`，affinity 通路根本不会启用。见 9.3。
- **`old_path` 是现成的"偏好路径"入口**：`schedule_send_balance()` 用调用方传入的 `*send_idx/*target_idx` 填充 `old_path`，PORT 级下若 `old_path.local_idx > 0 && old_path.target_idx > 0` 才做 `is_in_failover_route()` 过滤。post 路径初值为 -1，故实际为 `{0,0}`，该过滤在 post 路径上不生效（只有重传/回传路径会带真实旧路径）。
- `is_single_dev_mode()`（即 standalone）走 `bondp_post_send_wr_no_store()`，其余（active_backup / balance）走 `bondp_post_send_wr_list_and_store()`；两条下游实现都在每轮重试内调用同一个 `schedule_send_wr()`。本需求只覆盖 balance，因此实际落点是 store 路径，但调度改造对两条路径同时生效。

### 4.3 故障与回切

状态是四层发布的，hint 的"可用性判定"必须按同一套语义理解（否则会出现"看得到却选不中"或反之）：

| 层 | 位置 | 语义 |
| --- | --- | --- |
| 健康链路 | `bondp_hc_path_t.valid`（`bondp_dp_health.c`，按 `[local_idx, target_idx]`） | 健康检查探针的唯一真源；`hc_process_probe_cr()` 用一次 `atomic_exchange(&path->valid, ok)` 既写入又不丢"上一次是否可用" |
| 物理路径 | `bondp_p_target_jetty_t.valid`（按 `[local_idx, target_idx]`） | 由健康链路同步（`bondp_hc_tjetty_sync_valid()` / `hc_set_tjetty_list_target_valid()`），等价于"该 path 可直接送包" |
| 本端端口 | `bondp_comp_t.valid[local_idx]` | 故障时置 false；回切由 `bondp_fb_switch_path()` 在 `try_failback()` 内恢复为 true |
| 端口健康快照 | `bondp_context_t.port_status_bad[local_idx]` | 对外可见的 GOOD/BAD（`BONDP_USER_CTL_QUERY_PORT_STATUS` 直接读它） |

- 故障方向（`bondp_datapath.c` CR 转换，`is_failover_cr` 分支）：`valid[send_idx] = false`；`cr->status == URMA_CR_LOC_ACCESS_ERR` 时置 `port_status_bad[send_idx] = true`；`bondp_hc_tjetty_sync_valid()` + 把命中的 `p_tjetty->valid` 置 false；`is_need_rebuild_jetty() && enable_failback` 时投递 failback 任务，否则直接 `rebuild_done[send_idx] = true`。
- 回切方向：健康检查确认 `[local_idx, target_idx]` 恢复后先置 `p_tjetty->valid = true`；随后 `hc_set_local_idx_jettys_hc_valid()` 要求该 local_idx 的 `rebuild_done` 已被 `atomic_exchange` 消费成功，才发布 `hc_valid[local_idx] = true` 与 `port_status_bad[local_idx] = false`。
- `try_failback()` 在 store 路径的 post 之前调用：`bondp_fb_need_switch_path()` 先做无锁探测（`hc_valid[]` 全为 false 时连 `send_lock` 都不取），命中才 `bondp_fb_switch_path()` 把 `comp->valid[]` 恢复并把在 WR ring 里的 WR 改投到恢复后的路径。
- balance 模式不在故障时主动重传，下一次 post 才重新选路；**因此 hint 无需跨 post 保存复用**（与第 3 节结论 11 一致）。

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

## 5. 坐标与字段有效性表达方式

本设计只复用 `bondp_port_id_t`，不新增其它坐标类型。`bondp_port_id_t`（chip 4bit / die 4bit / port 8bit + `value`）沿用现状：与 `BONDP_USER_CTL_SET_BONDING_PORT`、`BONDP_USER_CTL_QUERY_PORT_STATUS` 使用同一套 UVS 坐标，`bondp_active_index_to_port_id()` 的输出可以原样回传，取值空间自洽，见 7.4 的坐标维说明。

"哪些字段有效"由外层一个 64bit mask 表达，而不是每侧一个位域 flag，理由：

- 与现有 provider 风格一致（`bondp_set_ctx_cfg_in.mask`、`urma_bond_jetty_ext.mask`、`bondp_rjetty_ext_*` 都是 mask 决定字段有效性）。
- src/dst 两侧六个字段可以放进同一个 64bit 字，后续加 `has_eid` / `has_vjetty_id` / `has_path_id` 等只是加 bit，结构体布局不变。
- 位域 union 是 ABI 可见布局，跨编译器/跨版本风险高于裸 `uint64_t`，也避免"每侧独立 flag"带来的两套语义。

## 6. 接口设计

### 6.1 数据结构

```c
#define BONDP_WR_HINT_VERSION_V0 0

/* 存在该宏即表示当前 liburma_ubagg 支持带 hint 的发送接口。
 * 仅用于 #ifdef / #if defined() 判断，不要用 #if BONDP_HAS_... 判断取值。 */
#define BONDP_HAS_POST_JETTY_SEND_WR_HINT BONDP_HAS_POST_JETTY_SEND_WR_HINT

typedef enum bondp_wr_hint_mask {
    BONDP_WR_HINT_MASK_SRC_CHIP_ID = 1ULL << 0,
    BONDP_WR_HINT_MASK_SRC_DIE_ID  = 1ULL << 1,
    BONDP_WR_HINT_MASK_SRC_PORT_ID = 1ULL << 2,
    BONDP_WR_HINT_MASK_DST_CHIP_ID = 1ULL << 3,
    BONDP_WR_HINT_MASK_DST_DIE_ID  = 1ULL << 4,
    BONDP_WR_HINT_MASK_DST_PORT_ID = 1ULL << 5,
} bondp_wr_hint_mask_t;

#define BONDP_WR_HINT_MASK_ALL \
    (BONDP_WR_HINT_MASK_SRC_CHIP_ID | BONDP_WR_HINT_MASK_SRC_DIE_ID | \
     BONDP_WR_HINT_MASK_SRC_PORT_ID | BONDP_WR_HINT_MASK_DST_CHIP_ID | \
     BONDP_WR_HINT_MASK_DST_DIE_ID  | BONDP_WR_HINT_MASK_DST_PORT_ID)

typedef struct bondp_wr_hint {
    uint32_t version;        /* 必须为 BONDP_WR_HINT_VERSION_V0 */
    uint32_t size;           /* 调用方头文件中的 sizeof(bondp_wr_hint_t) */
    uint64_t mask;           /* 0 表示不指定，完全走 bondp 内部调度 */
    bondp_port_id_t src_id;  /* chip/die/port 坐标；子字段有效性由 SRC_* 位决定 */
    bondp_port_id_t dst_id;  /* chip/die/port 坐标；子字段有效性由 DST_* 位决定 */
    uint64_t reserved[4];    /* 必须为 0，预留给后续扩展 */
} bondp_wr_hint_t;
```

命名说明：一份 hint 同时承载 src/dst 两侧的 chip、die、port 三个维度的坐标，所以**类型名、枚举名、宏前缀、函数名只用 `hint`，不再用 `port`**（`bondp_wr_hint_t` / `BONDP_WR_HINT_*` / `urma_post_jetty_send_wr_with_hint`）。三点补充：

- `mask` 的 bit 名保留 `PORT`：`BONDP_WR_HINT_MASK_SRC_PORT_ID` / `..._DST_PORT_ID` 指坐标里的 `port_idx` 子字段本身，不是整个 hint；`SRC_CHIP_ID` / `SRC_DIE_ID` 同构。
- 字段类型仍复用既有的 `bondp_port_id_t`（chip 4bit / die 4bit / port 8bit + `value`），这是既有公共类型，本次不改名、不新增坐标类型。
- 结构体内字段名用 `src_id` / `dst_id`，不用 `src_port_id` / `dst_port_id`，避免把"含三维信息的坐标"叫成 port。

设计取舍：

- 未使能的子字段取值一律忽略，因此"不指定 port_idx"与"port_idx = 0"不会混同。
- `chip_id` 的合法范围是 `[1, CHIP_NUM]`，未使能时遗留的 0 值天然不可能被误判为合法值。
- 两级扩展余量：bit 级余量在 `mask`（64bit 里还剩 58 个 bit），结构体级余量在 `reserved[4]`。
- `version` 只在不兼容语义变更时递增；`size` 负责前向兼容，provider 只读 `min(size, sizeof)` 的部分。

### 6.2 函数原型

```c
urma_status_t urma_post_jetty_send_wr_with_hint(urma_jetty_t *jetty, urma_jfs_wr_t *wr,
                                                urma_jfs_wr_t **bad_wr,
                                                const bondp_wr_hint_t *hint);
```

不修改 `urma_post_jetty_send_wr` 的签名（避免 ABI 破坏），也不改动 urma core 的 `urma_provider_ops_t`；新符号由 `liburma_ubagg.so` 导出，与 `urma_write_affinity` 一致。

### 6.3 使用示例

```c
bondp_wr_hint_t hint = {0};
hint.version = BONDP_WR_HINT_VERSION_V0;
hint.size    = sizeof(hint);
hint.mask    = BONDP_WR_HINT_MASK_ALL;
hint.src_id.bs.chip_id  = src_chip;   /* 来自 UVS 查询 */
hint.src_id.bs.die_id   = src_die;
hint.src_id.bs.port_idx = src_port;
hint.dst_id.bs.chip_id  = dst_chip;
hint.dst_id.bs.die_id   = dst_die;
hint.dst_id.bs.port_idx = dst_port;

urma_jfs_wr_t *bad_wr = NULL;
urma_status_t ret = urma_post_jetty_send_wr_with_hint(jetty, wr, &bad_wr, &hint);
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
| 坐标形态非法：`chip_id ∉ [1, CHIP_NUM]`、`die_id != 1`、`port_idx` 既不是 `UINT8_MAX`（primary EID）也不小于 `PORT_EID_MAX_NUM_PER_DEV`；或使能了某一侧却没给该侧的 `*_CHIP_ID` 位 | `URMA_EINVAL` |
| 本端坐标合法但换算不到活端口：`convert_bond_port_id_to_active_index()` 返回 -1（该端口没有活 p_ctx，或坐标维与 `bonding_level` 不匹配，见 7.4） | `URMA_EINVAL` |
| 本端换算出的 `local_idx` 不在本 jetty 的端口集合里（`local_idx ∉ bdp_comp->active_indices[0..active_count)`，见 12.1） | `URMA_EINVAL` |
| 对端坐标合法、但这条路径不在本 tjetty 里或当前不可用 | 不报错，按 7.2 纠正 + warn（见 9.2.3、9.3.2 第 2 步） |
| opcode 非 WRITE / READ 且 `mask != 0` | 待确认（当前可选 `URMA_EINVAL` 或 `URMA_FAIL`） |
| 非 balance 模式且 `mask != 0` | 待确认（同上） |

注：现有 `urma_status_t` 只有 `URMA_EINVAL` / `URMA_ENOMEM` / `URMA_FAIL` 等，没有 `ENOTSUP`，不建议为此改公共状态码枚举。

关于 `port_idx` 的取值范围：平台上真正"活动"的 port EID 是 `[URMA_ACTIVE_PORT_MIN, URMA_ACTIVE_PORT_MAX] = [4, 5]`（`bondp_env.c:init_path()` 只给这两个端口建 failover 路由，`SET_BONDING_PORT` 也只接受这两个值），但接口层**不硬挡**其它 port EID——只要该端口有活 p_ctx 且在本 jetty 的端口集合里就接受，与创建 jetty 时 `port_ids` 的既有校验（同样只走 `convert_bond_port_id_to_active_index()`）保持一致。差别只是：非 [4, 5] 的端口没有 failover 路由，只能走直连路径。

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

建议的 warn 日志字段：`vjetty_id`、`tjetty_id`、`opcode`、`hint(mask, src_id, dst_id)`、`resolved(local_idx→port_id, target_idx)`、`reason`。

### 7.4 与 bonding_level 的关系

balance 模式下 `bonding_level` 有两种取值，二者看到的"端口"根本不同，hint 语义必须分开定义（基线依据：`bondp_init_ctx_enabled_indices()` 按 level 只把对应档位的活 p_ctx 收进 `enabled_indices`；`bondp_init_member_eid_info_list()` 只用 `members[i].level == bonding_level` 的档位创建 p_ctx）：

| bonding_level | 活 p_ctx / 候选 local_idx | hint 可用的坐标维 | 说明 |
| --- | --- | --- | --- |
| `BONDP_BONDING_LEVEL_IODIE` | `[0, IODIE_NUM)`，即 chip 的 primary EID | 仅 chip（`port_idx` 必须为 `UINT8_MAX`） | `select_path_by_priority()` 的索引就是 `chip_id - 1`；给 port EID 会因 `p_ctxs[]` 为空被判非法 |
| `BONDP_BONDING_LEVEL_PORT` | `[IODIE_NUM, URMA_UBAGG_DEV_MAX_NUM)`，即各 die 的 port EID | chip + port（`die_id` 恒为 1） | 索引 `= IODIE_NUM + (chip_id-1) * PORT_EID_MAX_NUM_PER_DEV + port_idx`；平台活动端口是 `[URMA_ACTIVE_PORT_MIN, URMA_ACTIVE_PORT_MAX] = [4, 5]`（`bondp_env.c:init_path()` 只登记这两个端口），其它 port EID 若 UVS 报告为活也可以被指定，但没有 failover 路由 |

结论：接口文档需要显式声明"当前 level 支持哪些维"；应用从 UVS 拿到的坐标若不属于当前 level，一律按 7.1 报 `URMA_EINVAL`，不要静默忽略该维（静默忽略会让应用误以为 hint 生效）。实现上**不需要为此写额外的 level 判断**——坐标维不匹配时该档位的 `p_ctxs[]` 为空，`convert_bond_port_id_to_active_index()` 会直接返回 -1（例如 IODIE 级给 port EID、PORT 级给 primary EID），见 9.2.3。

## 8. 回切感知方案

这是设计阶段暴露出的主要难点：应用自己指定端口后，如何知道故障端口已回切以便重新使用。

结论：由 bondp 侧兜底纠正，应用侧通过 CR 感知，不新增查询接口或事件通道。

链路如下：

1. 健康检查确认某条 `[local_idx, target_idx]` 恢复后：先由 `hc_set_tjetty_list_target_valid()` 置 `p_tjetty->valid = true`；待该 local_idx 的 `rebuild_done` 被消费后才由 `hc_set_local_idx_jettys_hc_valid()` 发布 `hc_valid[local_idx] = true` 与 `port_status_bad[local_idx] = false`（见 4.3 的四层状态表）。
2. 应用下一次 post 时即使 hint 指向故障端口，bondp 也会改投到已恢复的端口（7.2 的强制纠正项），并打 warn 日志。注意这是"下一次 post"生效：`try_failback()` 只在 store 路径的 post 之前跑，且 `bondp_fb_need_switch_path()` 为 false 时完全不加锁。
3. 该 WR 的 CR 通过 imm 回传实际使用的本端 port index（`bondp_set_cr_port_idx`），应用用 `bondp_get_cr_local_port_id()` 即可读到真实的 chip/die/port。
4. 应用据此把该端口重新纳入候选集合，避免可用端口只减不增。

补充说明：

- 应用不做纠正逻辑也不影响正确性，仅是"更快重新使用恢复端口"。
- 应用侧观察到的"端口可用性"与 bondp 内部并不严格同刻：`port_status_bad[]`（应用可查）与 `comp->valid[]`（调度用）由同一次 `hc_valid[]` 发布更新，但 `p_tjetty->valid`（逐路径）更早变 true。因此存在"应用查到的仍是 BAD、而 bondp 已把该端口作为可纠正目标"的短暂窗口，这属于既有竞态窗口，本期不改（13 节记录）。
- 应用若希望在 post 之前就知道端口状态，仍可轮询 `BONDP_USER_CTL_QUERY_PORT_STATUS`（现有能力，本次不改）。
- 当前 CR 只回传本端端口，未回传对端 target 端口；若应用需要感知"实际对端端口"，本次不覆盖（见 12.5）。

## 9. 实现方案（含参考代码）

下面的代码都是"照着基线的风格写、可以直接落到对应文件"的参考实现，行号对应基线 `7ab0e5ec`。分三块讲：

- 9.1：要改哪些地方（一张表看全）
- 9.2：新接口本身怎么写（参数检查 + 坐标换算）
- 9.3：应用指定端口后，`schedule_send` 这条链要怎么配合

### 9.1 要改哪些地方

一句话：**加一个对外接口 + 加一个公共结构体 + 给三个内部函数各加一个"hint 入参" + 在 `schedule_send_balance()` 里加三步适配**。不新建调度器，不动 urma core 的 ops 表。

| 文件 | 改什么 | 位置（基线行号） |
| --- | --- | --- |
| `bond/include/urma_ubagg.h` | 新增 `bondp_wr_hint_t` / `bondp_wr_hint_mask_t` / 版本宏 / 能力宏 / `urma_post_jetty_send_wr_with_hint()` 声明 | `bondp_port_id_t`（`:168`）之后 |
| `bondp_datapath.c` | 新增 `urma_post_jetty_send_wr_with_hint()`：检查参数 + 换算坐标，然后走原有 post 流程 | 紧邻 `urma_write_affinity()`（`:865`） |
| `bondp_datapath.c` | 两个内部 post 函数和 `schedule_send_wr()` 各加一个 `hint` 入参，并把它透传下去 | `:481`、`:590`、`:351` |
| `bondp_datapath_schedule.h/.c` | `schedule_send()`、`schedule_send_balance()` 各加一个 `hint` 入参；新增 4 个小函数：`is_path_usable()`、`path_match_hint()`、`idx_to_chip()`、`narrow_by_hint()` | `:425`、`:322`，新函数放 `:182` 附近 |
| `bondp_dp_wr_hint.c/h`（可选） | 参数检查和坐标换算单独成文件；不建也可以，直接放 `bondp_datapath.c` | 新文件，`bond/CMakeLists.txt` 用 `aux_source_directory` 自动纳入 |

补充两点：

- `liburma_ubagg.so` 没有 version script，新增非 static 符号会直接导出（与 `urma_write_affinity` 同路径），所以 6.2 的"新符号由该库导出、不动 urma core ops 表"在工程上成立。
- `bondp_post_send_wr_no_store()` 只有 standalone 模式会走到（`is_single_dev_mode()`），而接口本身已经挡掉了非 balance 模式，所以这条路径上的 `hint` 永远是 `NULL`；为了两条路径原型一致，仍然给它加上这个入参。

### 9.2 新接口怎么写

#### 9.2.1 入口函数

```c
/* bondp_datapath.c，放在 urma_write_affinity() 后面 */
urma_status_t urma_post_jetty_send_wr_with_hint(urma_jetty_t *jetty, urma_jfs_wr_t *wr,
                                                urma_jfs_wr_t **bad_wr,
                                                const bondp_wr_hint_t *hint)
{
    if (jetty == NULL || wr == NULL) {
        return URMA_EINVAL;
    }
    if (hint == NULL || hint->mask == 0) {
        /* 没带 hint：行为与原接口完全一致 */
        return bondp_post_jetty_send_wr(jetty, wr, bad_wr);
    }

    bondp_comp_t *bdp_jetty = CONTAINER_OF_FIELD(jetty, bondp_comp_t, v_jetty);

    /* ① 参数检查（9.2.2） */
    urma_status_t ret = check_wr_hint_valid(bdp_jetty, wr, hint);
    if (ret != URMA_SUCCESS) {
        return ret;
    }
    /* ② 坐标换算：应用的 chip/die/port -> 内部端口编号（9.2.3） */
    bondp_wr_hint_filter_t hint_filter;
    ret = bondp_hint_to_filter(bdp_jetty, hint, &hint_filter);
    if (ret != URMA_SUCCESS) {
        return ret;
    }

    /* ③ 后面和 bondp_post_jetty_send_wr() 一模一样，只是多带一个 hint_filter */
    int wr_total = 0;
    ret = post_send_check_wr_list_valid(bdp_jetty, wr, bad_wr, &wr_total);
    if (ret != URMA_SUCCESS) {
        return ret;
    }
    if (is_single_dev_mode(bdp_jetty->bondp_ctx)) {
        return bondp_post_send_wr_no_store(bdp_jetty, wr, bad_wr, wr_total, &hint_filter);
    }
    return bondp_post_send_wr_list_and_store(bdp_jetty, wr, bad_wr, wr_total, &hint_filter);
}
```

落地时把 `{ use_cnt 自增 / deleting 检查 / PERF_PROFILING_* }` 这几行照抄 `bondp_post_jetty_send_wr()`（`:805`-`:830`）即可，参考实现里省略了它们。

#### 9.2.2 参数检查

```c
static urma_status_t check_wr_hint_valid(const bondp_comp_t *bdp_jetty, const urma_jfs_wr_t *wr,
                                         const bondp_wr_hint_t *hint)
{
    if (hint->version != BONDP_WR_HINT_VERSION_V0) {
        URMA_LOG_ERR("Unsupported wr hint version=%u\n", hint->version);
        return URMA_EINVAL;
    }
    if (hint->size < sizeof(bondp_wr_hint_t)) {
        URMA_LOG_ERR("Wr hint size=%u is too small\n", hint->size);
        return URMA_EINVAL;
    }
    if ((hint->mask & ~(uint64_t)BONDP_WR_HINT_MASK_ALL) != 0) {
        URMA_LOG_ERR("Wr hint has unknown mask=0x%llx\n", (unsigned long long)hint->mask);
        return URMA_EINVAL;
    }
    for (uint32_t i = 0; i < sizeof(hint->reserved) / sizeof(hint->reserved[0]); i++) {
        if (hint->reserved[i] != 0) {
            URMA_LOG_ERR("Wr hint reserved[%u] must be zero\n", i);
            return URMA_EINVAL;
        }
    }
    if (bdp_jetty->bondp_ctx->bonding_mode != BONDP_BONDING_MODE_BALANCE) {
        URMA_LOG_ERR("Wr hint is only supported in balance mode, mode=%d\n",
                     (int)bdp_jetty->bondp_ctx->bonding_mode);
        return URMA_EINVAL;
    }
    if (wr->opcode != URMA_OPC_WRITE && wr->opcode != URMA_OPC_READ) {
        URMA_LOG_ERR("Wr hint is only supported for WRITE/READ, opcode=%d\n", (int)wr->opcode);
        return URMA_EINVAL;
    }
    return URMA_SUCCESS;
}
```

#### 9.2.3 坐标换算：应用给的 chip/die/port → 内部端口编号

先定义换算结果。应用可能"只指到 chip"（比如只想让本端落在 1 号 chip），也可能"指到具体端口"，所以每一侧用两个精度：

```c
/* 纯内部类型，放在 bondp_datapath_schedule.h（或新建 bondp_dp_wr_hint.h），不进 urma_ubagg.h */
typedef struct bondp_wr_hint_side {
    bool used;      /* 应用使能了这一侧 */
    bool by_port;   /* true: 精确到端口；false: 只限定到 chip */
    uint32_t idx;   /* by_port == true 时有效：端口编号 */
    uint32_t chip;  /* 始终有效：[1, CHIP_NUM] */
} bondp_wr_hint_side_t;

typedef struct bondp_wr_hint_filter {
    bondp_wr_hint_side_t src;   /* 本端 */
    bondp_wr_hint_side_t dst;   /* 对端 */
} bondp_wr_hint_filter_t;
```

**本端换算**：直接复用控制面已有的 `convert_bond_port_id_to_active_index()`（`bondp_cp_jetty.c:543`），它已经校验了 `chip_id ∈ [1, CHIP_NUM]`、`die_id == 1`、以及该端口有没有活着的 `p_ctx`。这里只多加一条越界防护：

```c
/* chip_id / die_id 形态检查（只指到 chip 时也要做） */
static int check_hint_id_form(const bondp_port_id_t *id)
{
    if (id->bs.chip_id < BONDP_CHIP_ID_MIN || id->bs.chip_id > BONDP_CHIP_ID_MAX) {
        URMA_LOG_ERR("Invalid wr hint chip_id=%u\n", id->bs.chip_id);
        return -1;
    }
    if (id->bs.die_id != 1) {          /* 当前平台 IODIE_NUM_PER_CHIP == 1，die 只支持 1 */
        URMA_LOG_ERR("Invalid wr hint die_id=%u\n", id->bs.die_id);
        return -1;
    }
    return 0;
}

/* 本端 chip/die/port -> 端口编号（local_idx） */
static int hint_to_local_idx(const bondp_context_t *bdp_ctx, const bondp_port_id_t *id,
                             uint32_t *local_idx)
{
    /* port_idx 允许 UINT8_MAX（primary EID）或 [0, PORT_EID_MAX_NUM_PER_DEV-1]。
     * 注意不要直接照抄 convert_bond_port_id_to_active_index() 的 "port_idx > PORT_NUM"：
     * port_idx == PORT_NUM(9) 会被算到下一个 die 的端口上，属于既有隐患，这里显式挡掉。 */
    if (id->bs.port_idx != UINT8_MAX && id->bs.port_idx >= PORT_EID_MAX_NUM_PER_DEV) {
        URMA_LOG_ERR("Invalid wr hint port_idx=%u\n", id->bs.port_idx);
        return -1;
    }
    if (convert_bond_port_id_to_active_index(bdp_ctx, *id, local_idx) != 0) {
        return -1;   /* 该端口没有活 p_ctx，或坐标维与 bonding_level 不匹配（见 7.4） */
    }
    return 0;
}
```

**对端换算**：对端没有现成函数，但编号规则和本端完全一样（`bondp_env.c:init_path()` 与 `bondp_provider_ops.c:bondp_init_member_eid_info_list()` 用的是同一个公式），所以直接写一个对称的小函数：

```c
/* 对端 chip/die/port -> 端口编号（target_idx）
 * 编号空间：0/1 是 1 号、2 号 chip 的 primary EID；之后按 die 分组排 port EID：
 *           idx = IODIE_NUM + (chip_id - 1) * PORT_EID_MAX_NUM_PER_DEV + port_idx */
static int hint_to_target_idx(const bondp_port_id_t *id, uint32_t *target_idx)
{
    if (id->bs.chip_id < BONDP_CHIP_ID_MIN || id->bs.chip_id > BONDP_CHIP_ID_MAX) {
        URMA_LOG_ERR("Invalid wr hint chip_id=%u\n", id->bs.chip_id);
        return -1;
    }
    if (id->bs.die_id != 1) {
        URMA_LOG_ERR("Invalid wr hint die_id=%u\n", id->bs.die_id);
        return -1;
    }
    if (id->bs.port_idx == UINT8_MAX) {                        /* primary EID */
        *target_idx = id->bs.chip_id - 1;
    } else if (id->bs.port_idx < PORT_EID_MAX_NUM_PER_DEV) {    /* port EID */
        *target_idx = (uint32_t)IODIE_NUM +
                      (id->bs.chip_id - 1) * PORT_EID_MAX_NUM_PER_DEV + id->bs.port_idx;
    } else {
        URMA_LOG_ERR("Invalid wr hint port_idx=%u\n", id->bs.port_idx);
        return -1;
    }
    return 0;
}
```

**两侧合起来**，注意本端和对端"换算失败"的处理不一样（这是 7.1 与 7.2 的分界）：

```c
/* local_idx 是否在本 jetty 的端口集合里 */
static bool comp_port_enabled(const bondp_comp_t *bdp_comp, uint32_t local_idx)
{
    for (uint32_t i = 0; i < bdp_comp->active_count; i++) {
        if (bdp_comp->active_indices[i] == local_idx) {
            return true;
        }
    }
    return false;
}

static urma_status_t bondp_hint_to_filter(const bondp_comp_t *bdp_jetty,
                                          const bondp_wr_hint_t *hint,
                                          bondp_wr_hint_filter_t *filter)
{
    (void)memset(filter, 0, sizeof(*filter));

    /* ---- 本端：src_id ---- */
    uint64_t src_bits = hint->mask & (uint64_t)(BONDP_WR_HINT_MASK_SRC_CHIP_ID |
                                                BONDP_WR_HINT_MASK_SRC_DIE_ID |
                                                BONDP_WR_HINT_MASK_SRC_PORT_ID);
    if (src_bits != 0) {
        if ((src_bits & BONDP_WR_HINT_MASK_SRC_CHIP_ID) == 0) {
            URMA_LOG_ERR("Wr hint must set SRC_CHIP_ID to select a local port\n");
            return URMA_EINVAL;                  /* chip 是必填维，缺了没法定位 */
        }
        if (check_hint_id_form(&hint->src_id) != 0) {
            return URMA_EINVAL;
        }
        filter->src.used = true;
        filter->src.chip = hint->src_id.bs.chip_id;      /* 默认只限定到 chip */

        if ((hint->mask & BONDP_WR_HINT_MASK_SRC_PORT_ID) != 0) {   /* 精确到端口 */
            uint32_t local_idx = 0;
            if (hint_to_local_idx(bdp_jetty->bondp_ctx, &hint->src_id, &local_idx) != 0) {
                return URMA_EINVAL;
            }
            if (!comp_port_enabled(bdp_jetty, local_idx)) {
                /* 端口本身合法，但不在这条 jetty 的端口集合里：端口集合是应用自己配的，可自查 */
                URMA_LOG_ERR("Wr hint src port is not enabled on this jetty, local_idx=%u\n", local_idx);
                return URMA_EINVAL;
            }
            filter->src.by_port = true;
            filter->src.idx = local_idx;
        }
    }

    /* ---- 对端：dst_id ---- */
    uint64_t dst_bits = hint->mask & (uint64_t)(BONDP_WR_HINT_MASK_DST_CHIP_ID |
                                                BONDP_WR_HINT_MASK_DST_DIE_ID |
                                                BONDP_WR_HINT_MASK_DST_PORT_ID);
    if (dst_bits != 0) {
        if ((dst_bits & BONDP_WR_HINT_MASK_DST_CHIP_ID) == 0) {
            URMA_LOG_ERR("Wr hint must set DST_CHIP_ID to select a remote port\n");
            return URMA_EINVAL;
        }
        if (check_hint_id_form(&hint->dst_id) != 0) {
            return URMA_EINVAL;
        }
        filter->dst.used = true;
        filter->dst.chip = hint->dst_id.bs.chip_id;

        if ((hint->mask & BONDP_WR_HINT_MASK_DST_PORT_ID) != 0) {   /* 精确到端口 */
            if (hint_to_target_idx(&hint->dst_id, &filter->dst.idx) != 0) {
                return URMA_EINVAL;              /* 坐标形态非法，属于参数错误 */
            }
            /* 这里**不判断**这条路径在不在本 tjetty、好不好用：对端拓扑由 bondp 在 import
             * 时推导，应用无法预知，所以交给调度按 7.2 "命中就直命、命中不了纠正 + warn"。 */
            filter->dst.by_port = true;
        }
    }
    return URMA_SUCCESS;
}
```

关于"要不要缓存换算结果"：坐标→编号只用到 `chip/die/port` 和 `p_ctxs[]` 是否存活，与运行时端口好坏无关，所以**在接口入口算一次、随 post 调用传下去就够了**；"这条路径现在能不能用"才是每次 post 都要现场判断的（9.3）。

**补充：这里一共牵扯三个"端口集合"，别混起来**（会直接影响"哪些 hint 报错、哪些只 warn"）：

| 集合 | 来源 | 本次的作用 |
| --- | --- | --- |
| `bdp_comp->active_indices[]` | jetty/jfs 创建时的 `port_ids`（没给就用 `bdp_ctx->enabled_indices`，见 `init_active_indices_ex()`） | 本端 hint 的校验依据：不在里面 → `URMA_EINVAL`（7.1 / 12.1） |
| `bdp_ctx->port_cfg.enabled_indices[]` | `BONDP_USER_CTL_SET_BONDING_PORT` | 影响导入 tjetty 时路径对的贪心选择（`init_target_active_indices()`），不参与接口校验 |
| `bdp_tjetty->p_tjettys[].local_indice / remote_indice` | 导入 rjetty 时按 `connected[][]` 拓扑推导 | 对端 hint 的"命中/纠正"依据：命中不了只 warn，不报错（7.2） |

如果应用"先建 jetty、后调 `SET_BONDING_PORT`"，前两个集合可能不一致：hint 指向一个"在 `active_indices` 里、但不在 `port_cfg` 里"的端口时，本端校验会通过，但该路径对通常不存在，于是走 9.3.2 的"直命失败 → 收窄后留不下候选 → warn + 回退内部调度"。这个降级是设计上接受的，不是缺陷；实现时不要为了消除它去额外放行或额外报错。

### 9.3 应用指定端口后，schedule_send 要做的适配

#### 9.3.1 先给调度链加一个入参

接口算出的 `hint_filter` 要一路传到 balance 的选路函数。改动就是加参数 + 透传，逻辑不变：

```c
/* bondp_datapath.c */
static urma_status_t bondp_post_send_wr_no_store(bondp_comp_t *bdp_comp, urma_jfs_wr_t *wr,
                                                 urma_jfs_wr_t **bad_wr, int wr_total,
                                                 const bondp_wr_hint_filter_t *hint);
static urma_status_t bondp_post_send_wr_list_and_store(bondp_comp_t *bdp_comp, urma_jfs_wr_t *wr,
                                                       urma_jfs_wr_t **bad_wr, int wr_total,
                                                       const bondp_wr_hint_filter_t *hint);
static urma_status_t schedule_send_wr(const urma_jfs_wr_t *wr, bondp_comp_t *bdp_comp,
                                      int *send_idx, int *target_idx,
                                      const bondp_wr_hint_filter_t *hint);

/* bondp_datapath.c 里两处重试循环（:519、:612）只有这一行要改 */
    ret = schedule_send_wr(wr, bdp_comp, &send_idx, &target_idx, hint);

/* bondp_datapath_schedule.h */
int schedule_send(urma_target_jetty_t *tjetty, bondp_comp_t *bdp_comp, int *send_idx,
                  int *target_idx, bondp_chip_id_info_t *info,
                  const bondp_wr_hint_filter_t *hint);
```

`schedule_send_wr()` 需要把"新 hint 的 chip 维"和"原有 `has_drv_ext` 的 chip hint"合起来，喂给 affinity 选路（否则 PORT 级下 `info == NULL`，affinity 分支根本不会跑，见 4.2）：

```c
static urma_status_t schedule_send_wr(const urma_jfs_wr_t *wr, bondp_comp_t *bdp_comp,
                                      int *send_idx, int *target_idx,
                                      const bondp_wr_hint_filter_t *hint)
{
    bondp_chip_id_info_t info = {0};
    const bondp_chip_id_info_t *info_p = NULL;

    if (wr->flag.bs.has_drv_ext) {                               /* 原有 chip hint，逻辑不变 */
        const bondp_jfs_wr_t *bwr = CONTAINER_OF_FIELD(wr, bondp_jfs_wr_t, base);
        info.src_chip_id = bwr->src_chip_id;
        info.dst_chip_id = bwr->dst_chip_id;
    }
    if (hint != NULL) {                                          /* 新 hint 的 chip 维优先 */
        if (hint->src.used) { info.src_chip_id = hint->src.chip; }
        if (hint->dst.used) { info.dst_chip_id = hint->dst.chip; }
    }
    /* 只有两端 chip 都确定时才交给 affinity，避免半截 info 让 select_path_by_priority()
     * 用 src_chip_id = 0 去算 local_idx = -1 越界访问 valid[] */
    if (info.src_chip_id >= BONDP_CHIP_ID_MIN && info.src_chip_id <= BONDP_CHIP_ID_MAX &&
        info.dst_chip_id >= BONDP_CHIP_ID_MIN && info.dst_chip_id <= BONDP_CHIP_ID_MAX) {
        info_p = &info;
    }
    return schedule_send(wr->tjetty, bdp_comp, send_idx, target_idx, info_p, hint);
}
```

#### 9.3.2 schedule_send_balance() 里加三步

只改 `schedule_send_balance()`，原有 affinity / least-load / 随机挑选代码一行不动，只在合适的位置插三步：

```c
static int schedule_send_balance(const bondp_comp_t *bdp_comp, const bondp_target_jetty_t *bdp_tjetty,
                                 int *send_idx, int *target_idx, const bondp_chip_id_info_t *info,
                                 const bondp_wr_hint_filter_t *hint)
{
    uint32_t min_active_count = MIN(bdp_comp->active_count, bdp_tjetty->active_count);
    bondp_path_t least_load_path[URMA_UBAGG_MAX_CONNECTION] = {{0, 0}};
    uint32_t least_load_cnt = 0;
    uint32_t min[2] = {0};
    uint32_t max[2] = {0};

    if (min_active_count == 0) {
        URMA_LOG_ERR("Invalid min_active_count.\n");
        return URMA_EINVAL;
    }

    /* ===== 第 1 步：应用两端都指到了具体端口，且这条路径现在可用 -> 直接用 ===== */
    if (hint != NULL && hint->src.by_port && hint->dst.by_port &&
        is_path_usable(bdp_comp, bdp_tjetty, hint->src.idx, hint->dst.idx)) {
        *send_idx = (int)hint->src.idx;
        *target_idx = (int)hint->dst.idx;
        return 0;      /* 直接返回，不再进 select_random_path()，结果可预期 */
    }

    /* ===== 以下为原有选路，一行未改 ===== */
    if (info != NULL) {
        bondp_path_t old_path = {0};
        if (*send_idx >= 0 && *target_idx >= 0) {
            old_path.local_idx = *send_idx;
            old_path.target_idx = *target_idx;
        }
        least_load_cnt = select_affinity_path(bdp_comp, bdp_tjetty, info, least_load_path, &old_path);
        if (least_load_cnt == 0 && !bdp_comp->bondp_ctx->enable_failover) {
            return URMA_FAIL;
        }
        filter_least_load_path(least_load_path, &least_load_cnt);
    }

    if (least_load_cnt == 0) {
        if (bdp_comp->bondp_ctx->bonding_level == BONDP_BONDING_LEVEL_IODIE) {
            min[0] = min[1] = 0;
            max[0] = max[1] = IODIE_NUM;
        } else if (bdp_comp->bondp_ctx->bonding_level == BONDP_BONDING_LEVEL_PORT) {
            min[0] = min[1] = IODIE_NUM;
            max[0] = max[1] = URMA_UBAGG_DEV_MAX_NUM;
        } else {
            URMA_LOG_ERR("Unsupported bonding level=%d.\n", (int)bdp_comp->bondp_ctx->bonding_level);
            return URMA_EINVAL;
        }
        select_least_load_path(bdp_comp, bdp_tjetty, min, max, least_load_path, &least_load_cnt);
    }

    /* ===== 第 2 步：只指了一端（或指了具体端口但当前不可用）-> 按 hint 收窄候选 ===== */
    if (hint != NULL && least_load_cnt > 0) {
        narrow_by_hint(hint, least_load_path, &least_load_cnt);
    }

    if (least_load_cnt == 0) {
        return URMA_FAIL;
    }

    /* ===== 第 3 步：在剩下的候选里挑一个（原有逻辑）===== */
    uint32_t selected_pos = select_random_path(least_load_cnt);
    *send_idx = least_load_path[selected_pos].local_idx;
    *target_idx = least_load_path[selected_pos].target_idx;
    return 0;
}
```

三个配套小函数：

```c
/* 端口编号 -> chip 号（1..CHIP_NUM）。与 bondp_cp_jetty.c:bondp_active_index_to_port_id() 同义，
 * 只是这里只需要 chip 一个字段。 */
static uint32_t idx_to_chip(uint32_t idx)
{
    if (idx < (uint32_t)IODIE_NUM) {
        return idx + 1;                                     /* primary EID */
    }
    return (idx - (uint32_t)IODIE_NUM) / PORT_EID_MAX_NUM_PER_DEV + 1;   /* port EID */
}

/* 这条路径现在能不能直接用。判断项与 select_path_by_priority()/select_path_by_chip()
 * 循环里那四项完全一致，抽出来只写一份，避免两处判定漂移。 */
static bool is_path_usable(const bondp_comp_t *bdp_comp, const bondp_target_jetty_t *bdp_tjetty,
                           uint32_t local_idx, uint32_t target_idx)
{
    if (!atomic_load(&bdp_comp->valid[local_idx])) {
        return false;
    }
    if (bdp_comp->p_jetty[local_idx] == NULL) {
        return false;
    }
    const bondp_p_target_jetty_t *p_tjetty = bondp_find_p_tjetty_const(bdp_tjetty, local_idx, target_idx);
    if (!bondp_p_tjetty_available(p_tjetty)) {
        return false;                                        /* 对端端口不在本 tjetty 路径集合里，或该路径不可用 */
    }
    if (get_comp_urma_trans_mode(bdp_comp) == URMA_TM_RC && bdp_comp->comp_type == BONDP_COMP_JETTY &&
        bdp_comp->p_jetty[local_idx]->remote_jetty != p_tjetty->p_tjetty) {
        return false;                                        /* RC 模式下 jetty 绑的不是这条路径 */
    }
    return true;
}

/* 一条候选是否满足应用指定的端口 */
static bool path_match_hint(const bondp_wr_hint_filter_t *hint, const bondp_path_t *path)
{
    if (hint->src.used) {
        if (hint->src.by_port) {
            if (path->local_idx != hint->src.idx) { return false; }
        } else if (idx_to_chip(path->local_idx) != hint->src.chip) {
            return false;
        }
    }
    if (hint->dst.used) {
        if (hint->dst.by_port) {
            if (path->target_idx != hint->dst.idx) { return false; }
        } else if (idx_to_chip(path->target_idx) != hint->dst.chip) {
            return false;
        }
    }
    return true;
}

/* 按 hint 收窄候选集合。一条都留不下时保持原候选不变，只打日志：
 * 端口刚坏、正在回切、负载/优先级覆盖都属于"纠正"，不能让 post 失败（7.2）。 */
static void narrow_by_hint(const bondp_wr_hint_filter_t *hint, bondp_path_t path[], uint32_t *cnt)
{
    uint32_t keep = 0;

    for (uint32_t i = 0; i < *cnt; i++) {
        if (path_match_hint(hint, &path[i])) {
            path[keep] = path[i];        /* 原地压缩，和 filter_least_load_path() 一个写法 */
            keep++;
        }
    }
    if (keep == 0) {
        URMA_LOG_WARN("Wr hint port is unavailable now, fall back to internal schedule, "
                      "src_idx=%u(%s), dst_idx=%u(%s), candidate_cnt=%u\n",
                      hint->src.idx, hint->src.by_port ? "port" : "chip",
                      hint->dst.idx, hint->dst.by_port ? "port" : "chip", *cnt);
        return;
    }
    *cnt = keep;
}
```

`schedule_send()` 只需要把 `hint` 往 balance 分支透传，其它 mode 不用管（接口层已经挡掉了非 balance）：

```c
        case BONDP_BONDING_MODE_BALANCE:
            return schedule_send_balance(bdp_comp, bdp_tjetty, send_idx, target_idx, info, hint);
```

顺带建议做一个零行为变化的小重构：现有 `select_path_by_priority()`（`:40`，IODIE 级）和 `select_path_by_chip()`（`:75`，PORT 级）里的四项可用性判定与新加的 `is_path_usable()` 完全重复，可替换掉，避免以后两处判定漂移：

```c
/* select_path_by_priority()：循环体前后的索引计算保持不变，只把四行判定换成一行 */
    for (int i = 0; i < CHIP_ROUTE_NUM; i++) {
        local_idx = chip_pri[i].src_chip_id - BONDP_CHIP_ID_MIN;
        target_idx = chip_pri[i].dst_chip_id - BONDP_CHIP_ID_MIN;
        if (!is_path_usable(bdp_comp, bdp_tjetty, local_idx, target_idx)) {
            continue;
        }
        path->local_idx = local_idx;
        path->target_idx = target_idx;
        path->least_load = atomic_load(&bdp_comp->sqe_cnt[local_idx][target_idx]);
        return URMA_SUCCESS;
    }
```

两点注意：`is_path_usable()` 内部先查 `valid[]` 再取 `p_tjetty`，短路顺序与现有代码相同（不会用无效 index 去索引 `p_jetty[]`）；重构本身不改变任何判定结果，建议单独一个提交，方便 review。

#### 9.3.3 为什么是这三步

- **第 1 步要放在最前面、并且命中就直接 return**：应用把两端端口都算好了，bondp 再随机挑就没意义了；而且只有"确定性返回"才让应用能预期同一条 hint 落在哪个端口。放在 `info != NULL` 分支之前，也保证了即使 affinity 选路选到别处，也不会覆盖应用指定的端口。
- **第 2 步放在候选集合构建完之后**：候选可能来自 affinity 选路，也可能来自 least-load 兜底。放在这一处，只写一次就把两条来源都照顾到了；否则会出现"只给了对端端口、结果走了 least-load 就被忽略"的漏判。
- **收窄失败只 warn 不回错**：这就是 7.2 的纠正语义——端口可能刚坏（`comp->valid == false`）、可能正在回切、也可能因为负载/优先级被 bondp 覆盖。无论哪种，post 都必须继续，不能失败或者丢 WR。
- **每轮重试都要重新判断可用性**：`BONDP_POST_SEND_MAX_RETRY` 循环里 `send_idx` 是重新解析的（4.2），`valid[]` / `p_tjetty->valid` 也可能刚翻转。坐标换算结果（`hint_filter`）不含运行时状态，接口层算一次即可；"能不能用"的 `is_path_usable()` 必须在每轮调度里现场做。
- **别把 hint 塞进 `wr_entry->info`**：那个字段是 `bondp_chip_id_info_t`（只有 `src_chip_id/dst_chip_id`），chip hint、回传和回切路径都在用，见 9.4。

### 9.4 与现有 chip hint 的关系

- `urma_write_affinity` 与 `wr.flag.bs.has_drv_ext` 的 chip 级 hint 保持可用，行为不变。
- 新接口的 hint 是 chip 级 hint 的细粒度版本（可以精确到 port，也可以只到 chip）；两种 hint 同时出现时，新 hint 的 chip 维优先（见 9.3.1 的合成顺序），具体优先级建议在 12.6 中确认。
- 既有 `try_failback()` 在 store 路径 post 前执行，因此本次 post 的 hint 解析发生在 failback 完成之后，能看到最新的端口可用性。
- 由于 balance 模式不做内部重传，hint 不需要写入 `wr_entry->info` 供后续复用；`bondp_resend_jfs_wr` 仍按解析后的 `send_idx/target_idx` 工作。实现上**不要**把新的 hint 塞进 `wr_entry->info`：该字段是 `bondp_chip_id_info_t`（仅 `src_chip_id/dst_chip_id`），已有 chip hint 与回传/回切路径在使用。

### 9.5 新增日志一览

| 位置 | 级别 | 触发条件 | 关键字段 |
| --- | --- | --- | --- |
| `check_wr_hint_valid()` | ERR | version / size / mask / reserved / 非 balance / opcode 不合法 | `version`、`mask`、`opcode`、`bonding_mode` |
| `bondp_hint_to_filter()` | ERR | 缺 `*_CHIP_ID` 位、chip/die 形态非法、`port_idx` 越界、本端端口不在 jetty 端口集合 | `chip_id`、`die_id`、`port_idx`、`local_idx` |
| `narrow_by_hint()` | WARN | 候选集合被收窄到空（涵盖回切纠正、端口 BAD、路径不存在） | `src_idx` / `dst_idx`（带 `port` / `chip` 精度）、`candidate_cnt` |
| `schedule_send_balance()` 直命命中 | DEBUG（建议） | 应用指定路径可用并直接采用 | `local_idx`、`target_idx` |

说明：

- 回切纠正、端口 BAD、"路径不存在"都落到 `narrow_by_hint()` 的 WARN，天然满足 7.3 的"回切必须 warn"；负载/优先级导致的偏离也走同一条日志，频次可能很高，所以级别先按 12.4 待定（先按 DEBUG，压测后复议）。
- 建议 WARN 日志里同时带 `vjetty_id` / `tjetty_id`（hint 只定位到端口，排查时需要知道是哪条 jetty）。

### 9.6 建议的落地顺序

| 提交 | 内容 | 为什么这么拆 |
| --- | --- | --- |
| 提交 1（功能） | `urma_ubagg.h` 新增类型/宏/声明；接口入口 + 参数检查 + 坐标换算；`schedule_send_balance()` 三步与 4 个配套小函数 | 一次交付完整功能，避免出现"接口已导出但 hint 被静默忽略"的中间状态 |
| 提交 2（纯重构） | `select_path_by_priority()` / `select_path_by_chip()` 改用 `is_path_usable()` | 零行为变化，独立评审更容易 |
| 提交 3（打磨） | 日志级别按压测结果定稿、补 11 节用例、按需把换算拆到 `bondp_dp_wr_hint.c/h` | 不阻塞功能交付 |

注意：提交 1 里 `bondp_post_send_wr_no_store()` 也要一起加上 `hint` 入参（永远传 `NULL`），否则两条 post 路径调用 `schedule_send_wr()` 的原型对不上。

## 10. 兼容性与版本策略

### 10.1 能力宏机制

`urma_ubagg.h` 现有写法：

```c
/* For version compatibility */
#define BONDP_HAS_DATAPATH_QUERY BONDP_HAS_DATAPATH_QUERY
```

含义：宏**存在**即代表能力存在，本质是自助定义。应用只能这样用：

```c
#ifdef BONDP_HAS_POST_JETTY_SEND_WR_HINT
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
- version/size 非法、`mask` 含未知 bit、`reserved` 非 0 → `URMA_EINVAL`。
- 坐标类校验（7.1）：`chip_id = 0 / > CHIP_NUM`、`die_id != 1`、`port_idx >= PORT_EID_MAX_NUM_PER_DEV`（含 `port_idx = 9` 这个 `PORT_NUM` 边界，见 12.3）、使能某侧却没给该侧 `*_CHIP_ID`、本端端口不在本 jetty 端口集合、坐标维与 `bonding_level` 不匹配 → 均 `URMA_EINVAL`。
- 对端不可用不报错（7.2）：对端坐标合法但路径不在本 tjetty、或路径当前不可用 → 接口返回成功，实际走纠正 + warn（用 CR 或日志核对）。
- SEND / ATOMIC、非 balance 模式带 hint → 按 12.2 的结论返回错误。

功能：

- 路径健康时 hint 被采纳：用 CR imm（`bondp_get_cr_local_port_id`）核对实际本端端口等于 hint。
- 本端端口 BAD 时纠正：校验 CR 端口与 warn 日志。
- 回切场景：hint 指向 A，bondp 已恢复 B，确认实际落在 B（CR imm）且日志为 warn。
- 只给 dst、只给 src、只给 chip 等部分使能组合各一例。
- 同一个 WR 链（多 WR）共用同一解析路径。
- dst 维换算的自检：对每个可用的 `target_idx`，用 `bondp_active_index_to_port_id()` 反向生成 `(chip, die, port)` 再作为 dst hint 传入，确认直命命中的 `target_idx` 与原值一致（同时验证 9.2.3 的对称公式）。
- 重试语义：构造"第 1 轮命中 hint 后 `comp->valid[]` 被置 false"的场景，确认第 2 轮重新做可用性判断并按 7.2 纠正，而不是沿用上一轮的判断结果或直接失败。

性能：

- 带 hint 与不带 hint 的 post 路径做微基准对比，确认新增开销仅为若干分支与一次坐标转换，无额外分配与锁。

可执行构造步骤（把上面用例落成实际测试，按这个顺序搭环境）：

1. **取坐标**：用 `BONDP_USER_CTL_QUERY_PORT_STATUS`（输出 `bondp_query_port_status_out_t`，每项含 `chip_id/die_id/port_idx/status`）或 UVS 查询拿到本端/对端坐标；`port_idx = UINT8_MAX` 表示 primary EID。
2. **建连接**：目标 jetty/jfs 创建时的 `port_ids` 要与 `BONDP_USER_CTL_SET_BONDING_PORT` 的配置对齐，否则本端校验直接 `URMA_EINVAL`（见 7.4 / 12.1 / 9.2.3 的端口集合补充）。
3. **造 BAD 端口**：对该端口的 WR 用错 token 或错 ubva 触发 `URMA_CR_LOC_ACCESS_ERR`，或直接把对端 port EID 置 down；再用 `QUERY_PORT_STATUS` 确认该端口的 `status` 变成 `BONDP_PORT_STATUS_BAD`。
4. **核对实际落点**：`urma_poll_jfc()` 取到 CR 后用 `bondp_get_cr_local_port_id(cr)` 比对 chip/die/port；不符合预期时抓 9.5 的 WARN 日志（含 `src_idx` / `dst_idx` / `candidate_cnt`）。
5. **回切**：等一个健康检查周期（`BONDP_CTX_CFG_HEALTH_CHECK_INTERVAL`）或手动恢复端口，确认下一次 post 的实际落点变化，并核对日志级别是否符合 12.4 的结论。
6. **微基准**：同一份 WR 分别走原接口与新接口各 N 次，比较 post 延迟与 `PERF_PROFILING` 打点（`BOND_JETTY_POST_SEND`）。

## 12. 待确认问题

1. **本端 port 坐标合法、但不在本 jetty 的候选集合内（已定）**：返回 `URMA_EINVAL`。依据：该集合完全由应用自己决定——创建 jetty/jfs 时的 `port_ids`（`init_active_indices_ex()`）或 `BONDP_USER_CTL_SET_BONDING_PORT`（写 `bondp_context_t.port_cfg`），应用可感知；bondp 无法替应用猜测意图。判定范围即 `bdp_comp->active_indices[0..active_count)`。
2. **错误码选择**：非 balance 模式、SEND/ATOMIC 带 hint 时用 `URMA_EINVAL` 还是 `URMA_FAIL`。当前没有 `ENOTSUP`，倾向 `URMA_EINVAL` 并正确填写 `bad_wr`。
3. **dst 坐标映射（已关闭）**：远端 `target_idx` 与本地 `local_idx` 同处一个绝对设备索引空间——`bondp_types.h` 与 `urma_ubagg.h:300` 的注释、`bondp_env.c:init_path()`、`bondp_provider_ops.c:bondp_init_member_eid_info_list()` 三处互证，公式对称，直接用 9.2 的换算函数，"用 eid 匹配 rjetty ext"方案不再考虑。**附带发现（本设计需显式规避）**：`convert_bond_port_id_to_active_index()` 的 `port_idx > PORT_NUM` 判定有 off-by-one——`port_idx == PORT_NUM(9)` 会算出 `IODIE_NUM + 9 = 11`，即**另一个 die 的 port 0 槽**（该槽 p_ctx 存活时转换会"成功"），而不是被拒绝。hint 侧不要照抄这个边界，按 7.1 收紧为 `port_idx == UINT8_MAX` 或 `port_idx < PORT_EID_MAX_NUM_PER_DEV`；既有函数的该隐患见 12.11。
4. **日志级别**：回切纠正必须 warn；负载/优先级偏离 hint 的频次可能很高，建议降为 debug，避免刷日志。
5. **对端端口回传**：是否需要让应用知道这条 WR 实际使用的对端端口。当前 CR imm 只回传本端 port index，若需要则要额外设计（本次可不做）。
6. **与 `urma_write_affinity` 的关系**：建议保留原接口，二者独立；同一次 post 同时存在 chip 级与 port 级 hint 时，建议以 port 级 hint 为准并打 warn（待确认）。
7. **命名（已定）**：类型、枚举、宏与函数名都用 `hint`，不带 `port`（见 6.1 命名说明）——`bondp_wr_hint_t`、`bondp_wr_hint_mask_t`、`BONDP_WR_HINT_MASK_*`、`BONDP_WR_HINT_VERSION_V0`、`BONDP_HAS_POST_JETTY_SEND_WR_HINT`、`urma_post_jetty_send_wr_with_hint`；字段名 `src_id` / `dst_id`。理由：一份 hint 同时承载 chip/die/port 三维坐标，以 `port` 命名会窄化语义。
8. **结构体形态最终确认（已定）**：字段有效性统一用外层单个 64bit `mask`（理由见第 5 节），不引入每侧独立 flag、也不使用位域 union。
9. **hint 的生效时机（已定）**：坐标换算在接口入口算一次（只用 `chip/die/port` 与 `p_ctxs[]` 是否存活，与端口好坏无关），"这条路径现在能不能用"由调度在每轮重试里现场判断（`is_path_usable()`）；hint 不写入 `wr_entry->info`（见 9.2.3 / 9.3.3）。
10. **bonding_level 语义（已定，待应用确认提示方式）**：IODIE 级只接受 chip 坐标，PORT 级接受 chip+port 坐标（见 7.4）。这一条不需要额外代码：坐标维不匹配时 `convert_bond_port_id_to_active_index()` 会因 `p_ctxs[]` 为空而返回 -1，接口按 7.1 报 `URMA_EINVAL`。待应用确认的只是"报错 vs 忽略"的体验，本设计选报错——静默忽略会让应用误判 hint 已生效。
11. **是否顺手修复 `convert_bond_port_id_to_active_index()` 的 `port_idx == PORT_NUM` 别名问题（待确认）**：见 12.3 的附带发现。hint 侧先用更严格的规则规避；是否同时修既有函数（会影响 `SET_BONDING_PORT` 与创建路径的既有行为）需单独评估。

## 13. 与基线代码的对齐记录

本节记录本次 rebase 后的核对结论，便于评审时区分"设计问题"与"文档滞后"。

### 13.1 基线

| 项 | 值 |
| --- | --- |
| 上游 master | `7ab0e5ec`（`!3389 docs: fix documentation drift`） |
| 特性分支 | `br_9_20_port` = `3aa1e952`（`urma: add port schedule enable`），即本文档所在提交 |
| 该提交实际改动 | 当前为 `urma_ubagg.h` 的两处结构体声明（全仓尚无引用）与本文档；**没有任何调度或数据面改动**。评审结论：这两个结构体不再引入，需连头部声明一并回退 |
| 结论 | 本文档描述的是"待实现方案"，其中的新增结构体与调度改造都还没落码 |

### 13.2 rebase 引入的上游改动（仅 3 个提交触及 bond）

| 提交 | 内容 | 对本文档的影响 |
| --- | --- | --- |
| `a81dcd22`（合并 `8428956c`，`!3705`） | `bondp_dp_health.c:hc_process_probe_cr()` 改为 `atomic_exchange(&path->valid, ok)` | 4.3 / 8 节原来只写 `hc_valid` / `port_status_bad`，已补为四层状态发布（hc path → p_tjetty → comp.valid → hc_valid/port_status_bad） |
| `6d371dc8`（合并 `862fcd7b`，`!3688`） | `bondp_jetty_is_available()` / `bondp_get_cr_local_port_id()` 增加 NULL 与 bonding 设备校验 | 4.4 引用的 API 仍存在且未改签名，仅更健壮；无设计影响 |
| `14e3149e`（合并 `6bdb9683`，`!3677`） | rjetty ext 偏移按 `BONDP_RJETTY_EXT_MASK_LOCAL_CTX` 计算 | 12.3 的备选方案"用 eid 匹配 rjetty ext slave 条目"更没必要，直接关闭 |

`urma_ubagg.h`、`bondp_datapath.c`、`bondp_datapath_schedule.c`、`bondp_cp_jetty.c`、`bondp_cp_tjetty.c` 在本轮 rebase 范围内**未被上游改动**，因此文档引用的函数名与调用流程没有事实性过时，问题集中在"描述粒度不足"与"校验规则抄得不够严"。

### 13.3 本次修订的文档内容

1. 4.2：改为基线真实调用链，补 `select_least_load_path()`、`valid[]` 复查、`BONDP_POST_SEND_MAX_RETRY` 重试循环、`info != NULL` 前置条件与 `old_path` 语义。
2. 4.3：`valid` / `hc_valid` / `port_status_bad` / `p_tjetty->valid` 的状态分层与发布顺序。
3. 5：删除已废弃结构体的评价内容，只保留"复用 `bondp_port_id_t` + 外层 64bit mask 表达字段有效性"的设计取舍。
4. 7.1 与新 7.4：坐标校验分三段（形态非法 / 换算不到活端口 / 不在本 jetty 端口集合），并区分 IODIE / PORT 两种 bonding level 的可用维（不匹配时由既有换算函数自然拒绝，不需要额外代码）。
5. 8：回切链路补步骤顺序，并说明"应用可见状态与内部调度状态不同刻"的窗口。
6. 9（整章重写为"实现方案 + 参考代码"）：9.1 改动清单；9.2 接口实现（参数检查、`bondp_wr_hint_filter_t`、`hint_to_local_idx()` / `hint_to_target_idx()` / `bondp_hint_to_filter()`，以及三个端口集合的关系）；9.3 调度适配三步（直命 / 收窄候选 / 回退 + warn）与配套 `is_path_usable()` / `path_match_hint()` / `idx_to_chip()` / `narrow_by_hint()`，另含既有选路函数复用 `is_path_usable()` 的零行为重构建议；9.5 日志一览、9.6 落地拆分顺序。
7. 11：补充坐标边界、level 不匹配、dst 反向自检、重试幂等四类用例，以及一组可执行的测试构造步骤（取坐标 → 建连接 → 造 BAD → 核对落点 → 回切 → 微基准）。
8. 12：关闭 12.3，新增 12.9–12.11。

### 13.4 仍未解决的风险

- 7.2 的"纠正 + warn"在负载/优先级场景下的频次未知（12.4 待定级），需要压测后再决定日志级别。
- `port_status_bad[]`（应用可见）与 `p_tjetty->valid`（调度可见）之间存在短期不一致窗口（8 节补充说明）；若应用要求"查询结果与 hint 采纳严格一致"，需要单独确认。
- 12.10 里"坐标维与 level 不匹配时报错还是忽略"的体验需要应用侧确认；行为本身已由既有换算函数确定（报错），确认点只是接口文档怎么向应用承诺。
