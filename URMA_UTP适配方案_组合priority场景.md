# URMA UTP 适配方案 —— 组合 priority 场景（RTP|UTP 共存）

> 日期：2026-08-04
> 前提：udma 已支持 UTP，且一个 priority 可同时支持多种 tp 类型（如 RTP|UTP 位组合）。
> 范围：urma 各组件（urma_admin、urma_perftest、urma 库 jetty 创建校验）如何适配"组合 priority"语义，精确到代码粒度。

---

## 一、核心结论与适配边界

### 1.1 数据模型：两套 tp_type，不可混淆

代码中存在两套 tp_type 表达，语义不同，适配时必须区分：

| 类型 | 定义 | 语义 | 使用位置 |
|------|------|------|---------|
| **位图 union** `urma_tp_type_en` / `ubcore_sl_info.tp_type` | `struct { rtp:1; ctp:1; utp:1; reserved:29 } bs; uint32_t value;` | **能力位图**，一个 priority 可组合多 bit | `priority_info[].tp_type`、`dev_cap.{rm,rc,um}_tp_cap` |
| **单值枚举** `urma_tp_type_t` / `enum ubcore_tp_type` | `URMA_RTP/CTP/UTP`（0/1/2） | **jetty 选定的单一 tp** | `jetty_cfg.tp_type`、`rjetty.tp_type`、`cfg->tp_type`、`key.lk.tp_type` |

定义位置：

- 用户态位图：`00_umdk/umdk/src/urma/lib/urma/core/include/urma_types.h:168`（`union urma_tp_type_en`）
- 用户态枚举：`00_umdk/umdk/src/urma/lib/urma/core/include/urma_types.h:702`（`enum urma_tp_type`）
- 内核位图：`01_kernel/include/ub/urma/ubcore_types.h:629`（`union urma_tp_type_en`）
- 内核枚举：`01_kernel/include/ub/urma/ubcore_types.h:1554`（`enum ubcore_tp_type { UBCORE_RTP, UBCORE_CTP, UBCORE_UTP }`）

### 1.2 关键认知

用户所说"一个 priority 既能 RTP 又能 UTP"指的是**位图能力**（`priority_info[p].tp_type` 可同时 `bs.rtp=1 && bs.utp=1`）。

而 jetty 创建时传入的 `tp_type` 仍是**单值枚举**（一个 jetty 只能选一种 tp）。

**所以适配的本质不是把枚举改成位图，而是：消费 `priority_info[].tp_type`（位图）时，必须用位与（bit-inclusion）判断，不能用整体 value 全等比较。**

### 1.3 全量扫描结果：哪些点受组合影响

| 写法 | 组合场景表现 | 是否需改 |
|------|-------------|---------|
| `tp_type.value == priority_info[].tp_type.value`（整体相等） | **失效**（单 bit ≠ 组合值） | **需改** |
| `tp_type.bs.rtp == 1` / `bs.utp == 1 \|\| bs.rtp == 1`（位与） | 正确 | 无需改（模板） |
| `cfg->tp_type == UBCORE_RTP`（单值枚举比较） | 不涉及位图，正确 | 无需改 |

### 1.4 需适配的组件清单

| 组件 | 缺口 | 严重度 |
|------|------|--------|
| **urma_perftest** | `get_jetty_priority_by_tp_type` 及两处校验用整体 value 比较，组合场景下选不到 priority / 校验误报 | 高（功能不可用） |
| **urma_admin show** | `urma_tp_type_en_to_string` 只认纯单类型，组合返回 "Invalid Value" | 中（展示错误） |
| **urma 库 jetty 创建校验** | `urma_create_jetty_check_dev_cap` 不校验"所选 tp 是否被该 priority 支持" | 中（缺校验，内核兜底但报错延后/不明） |
| ipourma | 已是组合感知写法（`bs.utp==1 \|\| bs.rtp==1`） | 无需改（模板） |
| bond 库 | 不消费 `priority_info` | 无需改 |
| udma_jetty / connect_adapter | 比较单值枚举，不涉位图 | 无需改 |

---

## 二、urma_perftest 适配（核心，3 处）

### 2.1 问题定位

**选 priority**（`00_umdk/umdk/src/urma/tools/urma_perftest/perftest_resources.c:139-151`）：

```c
static int get_jetty_priority_by_tp_type(urma_device_attr_t *dev_attr, union urma_tp_type_en tp_type)
{
    int pri = -1;
    for (int i = 0; i <= URMA_MAX_PRIORITY; i++) {
        if (tp_type.value == dev_attr->dev_cap.priority_info[i].tp_type.value) {  // ❌ 整体相等
            pri = i;
            return pri;
        }
    }
    LOG_ERROR("Failed to get sl resources");
    return -1;
}
```

**校验用户指定 priority**（`perftest_resources.c:216-228`）：

```c
if (cfg->use_ctp && ctx->urma_ctx->ops->import_jetty_ex != NULL) {
    tp_type.bs.ctp = 1;
    if (tp_type.value != ctx->dev_attr.dev_cap.priority_info[cfg->priority].tp_type.value) {  // ❌
        LOG_ERROR("You should set the priority of type CTP\n");
        goto uninit;
    }
} else if (ctx->urma_ctx->ops->import_jetty_ex != NULL) {
    tp_type.bs.rtp = 1;
    if (tp_type.value != ctx->dev_attr.dev_cap.priority_info[cfg->priority].tp_type.value) {  // ❌
        LOG_ERROR("You should set the priority of type RTP\n");
        goto uninit;
    }
}
```

**失效场景**：当某 priority 是 `rtp|utp` 组合（value=0b101=5），用户态构造 `tp_type.bs.rtp=1`（value=1），`1 != 5` → 误报"应设置 RTP priority"，而该 priority 实际就支持 RTP。

### 2.2 修复方案：整体 value 比较 → 位与包含判断

**改造 `get_jetty_priority_by_tp_type`**（找第一个包含所求 tp bit 的 priority）：

```c
static int get_jetty_priority_by_tp_type(urma_device_attr_t *dev_attr, union urma_tp_type_en tp_type)
{
    /* tp_type is a single-bit mask (bs.rtp/ctp/utp == 1) built by the caller.
     * A priority may support multiple tp types simultaneously, so match by
     * bit-inclusion instead of full value equality. */
    for (int i = 0; i <= URMA_MAX_PRIORITY; i++) {
        if ((dev_attr->dev_cap.priority_info[i].tp_type.value & tp_type.value) != 0) {
            return i;
        }
    }
    LOG_ERROR("Failed to get sl resources for tp_type=0x%x", tp_type.value);
    return -1;
}
```

**抽公共 helper 复用**（放在 `get_jetty_priority_by_tp_type` 上方）：

```c
/* Returns true if priority p supports the tp indicated by the single-bit tp_type. */
static inline bool priority_supports_tp(urma_device_attr_t *attr, uint8_t p, union urma_tp_type_en tp_type)
{
    if (p > URMA_MAX_PRIORITY) {
        return false;
    }
    return (attr->dev_cap.priority_info[p].tp_type.value & tp_type.value) != 0;
}
```

**把 216-228 行两处校验改为**：

```c
if (cfg->use_ctp && ctx->urma_ctx->ops->import_jetty_ex != NULL) {
    tp_type.bs.ctp = 1;
    if (!priority_supports_tp(&ctx->dev_attr, cfg->priority, tp_type)) {
        LOG_ERROR("Priority %hhu does not support CTP\n", cfg->priority);
        goto uninit;
    }
} else if (ctx->urma_ctx->ops->import_jetty_ex != NULL) {
    tp_type.bs.rtp = 1;
    if (!priority_supports_tp(&ctx->dev_attr, cfg->priority, tp_type)) {
        LOG_ERROR("Priority %hhu does not support RTP\n", cfg->priority);
        goto uninit;
    }
}
```

### 2.3 perftest 的 UTP 测试路径补充（可选，按需）

当前 perftest 只在 `use_ctp` 与默认 RTP 间二选一（`perftest_resources.c:198-210`），**没有 UTP 选路分支**。若要 perftest 支持 UTP 压测，需补：

- `perftest_parameters`：加 `--utp` 选项（类似 `--ctp`/`use_ctp`），设置 `cfg->use_utp`。
- `perftest_resources.c:198-210` 的 `if (cfg->priority == PERFTEST_INVALID_PRIORITY)` 分支补 UTP 分支：

  ```c
  } else if (cfg->use_utp) {
      tp_type.bs.utp = 1;
      cfg->priority = (uint8_t)get_jetty_priority_by_tp_type(&ctx->dev_attr, tp_type);
  }
  ```

- 创建 jetty 时 `jetty_cfg.tp_type = URMA_UTP`、`trans_mode = URMA_TM_UM`（配合 `urma_check_tp_type_valid` 的约束：UTP 必须 UM 模式）。
- mgmt jetty 仍强制 CTP（`perftest_mgmt_ub.c:76`），**不动**（mgmt 信道独立）。

> 若本期不含 perftest UTP 压测需求，2.3 可缓做，仅做 2.2 的兼容性修复即可（避免组合场景下 perftest 误报）。

---

## 三、urma_admin show 适配（展示层，1 处）

### 3.1 问题定位

`00_umdk/umdk/src/urma/lib/urma/core/include/urma_types_str.h:98-110` —— 只认纯单类型，组合返回 "Invalid Value"：

```c
static inline const char *urma_tp_type_en_to_string(union urma_tp_type_en tp_type)
{
    if (tp_type.bs.rtp == 1 && tp_type.bs.ctp == 0 && tp_type.bs.utp == 0) return "RTP";
    if (tp_type.bs.rtp == 0 && tp_type.bs.ctp == 1 && tp_type.bs.utp == 0) return "CTP";
    if (tp_type.bs.rtp == 0 && tp_type.bs.ctp == 0 && tp_type.bs.utp == 1) return "UTP";
    return "Invalid Value";   // ❌ rtp|utp 等组合落到这里
}
```

`00_umdk/umdk/src/urma/tools/urma_admin/admin_cmd_show.c:706-719` 的 `print_ubep_prioritys` 调用它逐个打印 16 个 priority 的 tp_type，组合 priority 会显示 "Invalid Value"。

### 3.2 修复方案：tp_type 分行打印（RTP / CTP / UTP 各占一行）

#### 3.2.1 目标效果

把当前单行 tp_type 表格（组合 priority 会显示 "Invalid Value"）：

```
 tp_type  :  RTP  RTP  CTP  RTP  Invalid Value  ...   （16 列，组合 priority 显示 Invalid Value）
```

改为**每个 tp 类型占一行**，共 3 行（RTP / CTP / UTP）。每行对应 16 个 priority 该 bit 的是否支持：支持则打印类型名，不支持则该列留空。例如 priority 0 支持 RTP+UTP，则第 1 行该列 `RTP`、第 2 行（CTP）该列空、第 3 行该列 `UTP`：

```
priority  :    0    1    2    3    4    5    6    7    8    9   10   11   12   13   14   15
      sl  :    0    0    1    1    2    2    2    2    3    3    4    4    5    5    6    6
 tp_type  :  RTP  RTP       RTP  RTP  UTP  RTP  UTP       RTP       UTP  RTP       RTP  UTP
            CTP       CTP       CTP             CTP       CTP             CTP       CTP
            UTP       UTP            UTP            UTP       UTP            UTP       UTP
```

每个 priority 占 5 字符宽（与 `sl` 的 `%5d` 对齐），列间无额外间距，三行数据列严格对齐。

#### 3.2.2 修改点

只需改 `print_ubep_prioritys` 这一个函数（`00_umdk/umdk/src/urma/tools/urma_admin/admin_cmd_show.c:706-719`）。`urma_tp_type_en_to_string` **保持不动**（它仍服务于其他只取单值、不关心组合的调用点，如 tpid 展示路径），本方案直接读 `priority_info[].tp_type` 位 bit，不再依赖该函数做组合展示。

#### 3.2.3 修改后代码

```c
static void print_ubep_prioritys(const admin_show_ubep_t *ubep)
{
    /* tp_type is a bitmap: a priority may support RTP|CTP|UTP simultaneously.
     * Print one row per tp type so a multi-bit priority is fully visible.
     * Each priority occupies a 5-char column to align with the SL row. */
    static const struct {
        const char *name;
        uint32_t    mask;  /* single-bit value in urma_tp_type_en.bs */
    } tp_rows[] = {
        { "RTP", 0x1 },  /* bs.rtp  */
        { "CTP", 0x2 },  /* bs.ctp  */
        { "UTP", 0x4 },  /* bs.utp  */
    };

    printf("priority  :    0    1    2    3    4    5    6    7    8    9   10   11   12   13   14   15\n");
    printf("      sl  :");
    for (int i = 0; i < URMA_MAX_PRIORITY_CNT; ++i) {
        printf("%5d", ubep->dev_attr.dev_cap.priority_info[i].SL);
    }
    printf("\n");

    printf(" tp_type  :");
    for (int i = 0; i < URMA_MAX_PRIORITY_CNT; ++i) {
        if (ubep->dev_attr.dev_cap.priority_info[i].tp_type.value & tp_rows[0].mask) {
            printf("%5s", tp_rows[0].name);   /* RTP row */
        } else {
            printf("%5s", "");
        }
    }
    printf("\n");

    for (int row = 1; row < (int)(sizeof(tp_rows) / sizeof(tp_rows[0])); ++row) {
        printf("            ");  /* indent: align under " tp_type  " (12 chars) */
        for (int i = 0; i < URMA_MAX_PRIORITY_CNT; ++i) {
            if (ubep->dev_attr.dev_cap.priority_info[i].tp_type.value & tp_rows[row].mask) {
                printf("%5s", tp_rows[row].name);
            } else {
                printf("%5s", "");
            }
        }
        printf("\n");
    }
}
```

#### 3.2.4 对齐细节说明

表格左侧标签宽度必须一致才能列对齐：

| 标签 | 字符数 | 说明 |
|------|--------|------|
| `priority  :` | 11 字符 + 数据 | 首列宽 5，与数字对齐 |
| `      sl  :` | 11 字符 | 6 空格 + `sl  :` |
| ` tp_type  :` | 11 字符 | 1 空格 + `tp_type  :` |
| 续行缩进 `"            "` | 11 字符空格 | 与 ` tp_type  :` 的 `:` 后数据列对齐 |

数据列每列宽 5（`%5s` / `%5d`），首行 `priority` 行的 `    0` 也是宽 5（4 空格 + 数字），全局一致。

#### 3.2.5 位掩码取值依据

`urma_tp_type_en` 位序（`00_umdk/umdk/src/urma/lib/urma/core/include/urma_types.h:168`）：

```c
struct { uint32_t rtp:1; uint32_t ctp:1; uint32_t utp:1; uint32_t reserved:29; } bs;
uint32_t value;
```

- `bs.rtp` 是 bit 0 → mask `0x1`
- `bs.ctp` 是 bit 1 → mask `0x2`
- `bs.utp` 是 bit 2 → mask `0x4`

直接用 `value & mask` 比 `bs.xxx == 1` 更稳妥（避免位域实现定义行为），且与组合语义天然兼容。若担心硬编码 mask 与位序耦合，可改用具名常量：

```c
{ "RTP", /* bs.rtp */ 1u << 0 },
{ "CTP", /* bs.ctp */ 1u << 1 },
{ "UTP", /* bs.utp */ 1u << 2 },
```

#### 3.2.6 不改 `urma_tp_type_en_to_string` 的理由

该函数保持单类型语义（服务于其他只取单值、不关心组合的调用点，如 tpid 展示路径），**职责更清晰**，且避免 `static __thread` 缓冲带来的副作用（不可跨调用嵌套、不可存返回值跨多个 printf 复用）。组合展示的需求由 `print_ubep_prioritys` 内部直接处理位图，单一职责。

---

## 四、urma 库 jetty 创建校验适配（补强，可选但推荐）

### 4.1 问题定位

`00_umdk/umdk/src/urma/lib/urma/core/urma_cp_api.c:1757`：`urma_create_jetty` 调用 `urma_create_jetty_check_dev_cap`，但后者只校验深度/sge（`urma_cp_api.c:1613-1651`），**不校验 `jetty_cfg->tp_type` 是否被 `priority_info[jetty_cfg->priority].tp_type` 支持**。

当前依赖内核 `udma_jetty.c` 兜底（创建时 `ctx->sl = dev->priority_info[cfg->priority].SL`）。组合场景下用户更容易传错（priority 支持 rtp|utp，却传了 ctp），应早失败、报错清晰。

### 4.2 修复方案：在 dev_cap 校验中加 priority-tp 匹配

在 `urma_create_jetty_check_dev_cap` 末尾 `return 0` 前补：

```c
/* A priority may support a combination of tp types; the requested single tp_type
 * must be among the bits advertised by priority_info[priority]. */
if (jetty_cfg->jfs_cfg.priority <= URMA_MAX_PRIORITY) {
    union urma_tp_type_en req = { .value = 0 };
    /* jetty_cfg->tp_type is the single-value enum (URMA_RTP/CTP/UTP); map to bit. */
    switch (jetty_cfg->tp_type) {
        case URMA_RTP: req.bs.rtp = 1; break;
        case URMA_CTP: req.bs.ctp = 1; break;
        case URMA_UTP: req.bs.utp = 1; break;
        default:
            URMA_LOG_ERR("invalid tp_type=%d\n", (int)jetty_cfg->tp_type);
            return -1;
    }
    union urma_tp_type_en sup = cap->priority_info[jetty_cfg->jfs_cfg.priority].tp_type;
    if ((sup.value & req.value) == 0) {
        URMA_LOG_ERR("priority %u does not support tp_type %d (supported=0x%x)\n",
            jetty_cfg->jfs_cfg.priority, (int)jetty_cfg->tp_type, sup.value);
        return -1;
    }
}
```

**实施前需核对**：

- `cap` 来源：`urma_create_jetty_check_dev_cap` 取 `ctx->dev->sysfs_dev->dev_attr.dev_cap`，`priority_info` 在该结构内（`urma_types.h:296`），路径正确。
- `jetty_cfg->tp_type` 的确切字段名与类型（枚举 `urma_tp_type_t`）。
- `jfs_cfg.priority` 的字段路径（以 `urma_types.h:558/653` 为准对齐，jfs_cfg 在 jetty_cfg 内）。

### 4.3 是否必做

此适配**可选**：若本期只关注展示与 perftest，可不动 jetty 校验，交给内核兜底。但组合能力上线后，早失败能显著降低排障成本，**建议一并做**。

---

## 五、其余组件：确认无需改

| 组件 | 代码点 | 结论 |
|------|--------|------|
| ipourma | `01_kernel/drivers/ub/urma/ulp/ipourma/ipourma_res.c:688-689` `bs.utp==1 \|\| bs.rtp==1` | 已是组合感知，**模板**，无需改 |
| udma_jetty | `01_kernel/drivers/ub/urma/hw/udma/udma_jetty.c:2083-2097` `cfg->tp_type == UBCORE_XXP` | 比较单值枚举（jetty 选定 tp），不涉位图，无需改 |
| connect_adapter | `01_kernel/drivers/ub/urma/ubcore/ubcore_connect_adapter.c` 多处 `cfg->tp_type == UBCORE_XXP` | 同上，单值枚举，无需改 |
| `urma_check_tp_type_valid` | `00_umdk/.../urma_cp_api.c:1550` | 校验 trans_mode↔tp 互斥（UTP↔UM），语义正确，无需改 |
| 内核 `ubcore_check_tp_type_valid` | `01_kernel/drivers/ub/urma/ubcore/ubcore_tp.c:31` | 同上，无需改 |
| bond 库 | 不消费 priority_info | 无需改 |
| perftest mgmt jetty | `00_umdk/.../perftest_mgmt_ub.c:76` 强制 CTP | mgmt 信道独立，无需改 |

---

## 六、实施顺序与验证

### 6.1 改动文件清单

| 文件 | 改动 | 行数 |
|------|------|------|
| `00_umdk/umdk/src/urma/tools/urma_admin/admin_cmd_show.c` | `print_ubep_prioritys` 改分行打印（RTP/CTP/UTP 三行），直接读位图 | ~40 行 |
| `00_umdk/umdk/src/urma/tools/urma_perftest/perftest_resources.c` | `get_jetty_priority_by_tp_type` + 两处校验改位与，加 helper | ~25 行 |
| `00_umdk/umdk/src/urma/lib/urma/core/urma_cp_api.c` | `urma_create_jetty_check_dev_cap` 加 priority-tp 校验（可选） | ~20 行 |

> 注：`urma_types_str.h` 的 `urma_tp_type_en_to_string` **不再修改**，保持单类型语义，组合展示由 `print_ubep_prioritys` 内部直接处理位图。

### 6.2 验证

1. **admin 展示（分行）**：构造 `priority_info[p].tp_type.bs = {rtp=1, utp=1}` → `urma_admin show --whole` 该 priority 列第 1 行 `RTP`、第 3 行 `UTP`、第 2 行（CTP）空；不再出现 `Invalid Value`。三行数据列与上方 `sl` 行、`priority` 行严格按 5 字符宽对齐。
2. **perftest 选 priority**：同上组合 priority，`urma_perftest`（默认 RTP）应能选中该 priority，不再误报 "should set priority of type RTP"。
3. **perftest 校验**：`-O <组合priority>` + 默认 RTP 应通过；`-O <组合priority>` + `--ctp` 应通过；`-O <仅UTP priority>` + `--ctp` 应早失败、报错清晰。
4. **jetty 校验（若做 4.2）**：priority 支持 rtp|utp，传 `tp_type=CTP` → 用户态即报 "priority X does not support tp_type CTP"，不再延后到内核。

### 6.3 回归

- 单类型 priority（纯 RTP / 纯 CTP / 纯 UTP）行为：admin 展示该 priority 列仅对应行有类型名，其余两行空，符合预期（与原单行展示语义等价但分行）。
- 空能力 priority（reserved-only，value=0）：三行该列均空。
- 位与包含判断在单 bit 下等价于原全等比较，perftest / jetty 校验对单类型 priority 行为不变。
- `urma_tp_type_en_to_string` 保持不变，其下游消费点（如 tpid 展示路径）不受影响。

---

## 附：适配原理速记

1. `priority_info[].tp_type` 是**位图能力**（可组合），`jetty_cfg->tp_type` 是**单值枚举**（jetty 选一种）。
2. 消费位图能力时一律用**位与包含**：`(cap.value & req.value) != 0`，禁止 `value == value`。
3. ipourma 已是正确写法，可作为模板参考。
4. 组合不影响单值枚举的比较路径（udma_jetty、connect_adapter 等无需改）。
