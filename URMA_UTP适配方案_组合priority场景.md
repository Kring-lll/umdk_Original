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
| **位图 union** `urma_tp_type_en` | `struct { rtp:1; ctp:1; utp:1; reserved:29 } bs; uint32_t value;` | **能力位图**，一个 priority 可组合多 bit | `priority_info[].tp_type`、`dev_cap.{rm,rc,um}_tp_cap`（`urma_types.h:168/296`） |
| **单值枚举** `urma_tp_type_t` | `URMA_RTP/CTP/UTP`（0/1/2） | **远端/import 选定的单一 tp** | `rjetty.tp_type`、`tjetty.tp_type`（`urma_types.h:702/784/796`） |

### 1.2 关键认知

用户所说"一个 priority 既能 RTP 又能 UTP"指的是**位图能力**（`priority_info[p].tp_type` 可同时 `bs.rtp=1 && bs.utp=1`）。

而单值枚举 `tp_type` 存在于**远端/import/bind 结构**（`urma_rjetty_t` / `urma_target_jetty_t`），**`urma_jetty_cfg_t` 本身无 `tp_type` 字段**——所以 create_jetty 阶段拿不到 tp_type，只能用 `trans_mode` 收窄；bind_jetty 阶段才同时持有 priority 与 tp_type（详见 4.2/4.3）。

**适配本质**：消费 `priority_info[].tp_type`（位图）时必须用**位与（bit-inclusion）**判断，不能用整体 value 全等比较。

### 1.3 需适配的组件清单

| 组件 | 缺口 | 严重度 |
|------|------|--------|
| **urma_perftest** | `get_jetty_priority_by_tp_type` 及两处校验用整体 value 比较，组合场景下选不到 priority / 校验误报 | 高（功能不可用） |
| **urma_admin show** | `urma_tp_type_en_to_string` 只认纯单类型，组合返回 "Invalid Value" | 中（展示错误） |
| **urma 库 jetty 创建校验** | `urma_create_jetty_check_dev_cap` 不碰 priority（连越界都不查），组合场景下 trans_mode 与 priority 能力全冲突无 user 态拦截 | 中（4.2） |
| **urma 库 bind_jetty 校验** | bind 同时持有 priority 与 tp_type，但不校验"远端 tp 是否被本端 priority 支持" | 中（4.3） |
| ipourma | 已是组合感知写法（`bs.utp==1 \|\| bs.rtp==1`） | 无需改（模板） |
| bond 库 | 不消费 `priority_info` | 无需改 |
| udma_jetty / connect_adapter | 比较单值枚举，不涉位图 | 无需改 |

---

## 二、urma_perftest 适配

### 2.1 问题定位

**选 priority**（`perftest_resources.c:139-151`）：

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
    /* tp_type 为单 bit mask；priority 可组合多 tp，故按位与包含匹配 */
    for (int i = 0; i <= URMA_MAX_PRIORITY; i++) {
        if ((dev_attr->dev_cap.priority_info[i].tp_type.value & tp_type.value) != 0) {
            return i;
        }
    }
    LOG_ERROR("Failed to get sl resources for tp_type=0x%x", tp_type.value);
    return -1;
}
```

**抽公共 helper**（放在 `get_jetty_priority_by_tp_type` 上方）：

```c
/* priority p 是否支持单 bit tp_type */
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

### 2.3 perftest 的 UTP 路径说明

perftest 的 tp_type 并非在 create jetty 时设置（`urma_jetty_cfg_t` 无 tp_type 字段），而是在 **import 前**根据 `trans_mode` 设置 `rjetty->tp_type`（`perftest_resources.c:2010-2016`）：

```c
if (cfg->use_ctp) {
    rjetty->tp_type = URMA_CTP;
} else if (rjetty->trans_mode == URMA_TM_UM) {
    rjetty->tp_type = URMA_UTP;   /* UM 自动走 UTP */
} else {
    rjetty->tp_type = URMA_RTP;
}
```

所以 **UM 模式已隐式走 UTP**，无需新增 `--utp` 选项。组合 priority 场景下，UM/UTP 能否选中组合 priority 由 2.2 的 `get_jetty_priority_by_tp_type`（位与判断）保证。tp_type 经 rjetty→import→tjetty→bind 链传递，create 阶段不介入。

---

## 三、urma_admin show 适配

### 3.1 问题定位

`urma_types_str.h:98-110` —— 只认纯单类型，组合返回 "Invalid Value"：

```c
static inline const char *urma_tp_type_en_to_string(union urma_tp_type_en tp_type)
{
    if (tp_type.bs.rtp == 1 && tp_type.bs.ctp == 0 && tp_type.bs.utp == 0) return "RTP";
    if (tp_type.bs.rtp == 0 && tp_type.bs.ctp == 1 && tp_type.bs.utp == 0) return "CTP";
    if (tp_type.bs.rtp == 0 && tp_type.bs.ctp == 0 && tp_type.bs.utp == 1) return "UTP";
    return "Invalid Value";   // ❌ rtp|utp 等组合落到这里
}
```

`admin_cmd_show.c:706-719` 的 `print_ubep_prioritys` 调用它逐个打印 16 个 priority 的 tp_type，组合 priority 会显示 "Invalid Value"。

### 3.2 修复方案：tp_type 分行打印（RTP / CTP / UTP 各占一行）

改为每个 tp 类型占一行（共 3 行），每行对应 16 个 priority 该 bit 是否支持，支持则打印类型名、否则留空。例如 priority 0 支持 RTP+UTP，则第 1 行该列 `RTP`、第 3 行该列 `UTP`、第 2 行（CTP）空：

```
priority  :    0    1    2    3    4    5    6    7    8    9   10   11   12   13   14   15
      sl  :    0    0    1    1    2    2    2    2    3    3    4    4    5    5    6    6
 tp_type  :  RTP  RTP       RTP  RTP  UTP  RTP  UTP       RTP       UTP  RTP       RTP  UTP
            CTP       CTP       CTP             CTP       CTP             CTP       CTP
            UTP       UTP            UTP            UTP       UTP            UTP       UTP
```

只改 `print_ubep_prioritys`（`admin_cmd_show.c:706-719`）一个函数，直接读 `priority_info[].tp_type` 位 bit；`urma_tp_type_en_to_string` 保持不动（服务于其他单值调用点）。

```c
static void print_ubep_prioritys(const admin_show_ubep_t *ubep)
{
    /* tp_type 为位图，priority 可同时支持 RTP|CTP|UTP；每类 tp 占一行，列宽 5 对齐 SL 行 */
    static const struct {
        const char *name;
        uint32_t    mask;  /* urma_tp_type_en.bs 单 bit */
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

mask 依据 `urma_tp_type_en` 位序（`urma_types.h:168`，`bs.rtp`=bit0/`bs.ctp`=bit1/`bs.utp`=bit2 → `0x1/0x2/0x4`）。用 `value & mask` 比 `bs.xxx == 1` 稳妥（避免位域实现定义行为），且与组合语义兼容。

---

## 四、urma 库 jetty 创建/bind 校验适配

### 4.1 问题定位

`urma_create_jetty`（`urma_cp_api.c:1757`）调用的 `urma_create_jetty_check_dev_cap`（`urma_cp_api.c:1613-1644`）只校验深度/sge/jetty_grp 数量，**不碰 `jfs_cfg->priority`**——连越界都不查，组合场景下越界值会原样传到内核 `priority_info[cfg->priority].SL` 致数组越界；`trans_mode` 与 priority 能力全冲突（如 UM jfs 配纯 RTP priority）也无 user 态拦截，内核兜底报错延后。

### 4.2 修复方案：在 dev_cap 校验中加 priority 能力校验

#### 4.2.1 拦截原则

create 时手上有 `jfs_cfg.priority`、`jfs_cfg.trans_mode`、`cap->priority_info[]`。约束核心：**UM 不走 RTP**（UM 可绑 UTP/CTP，不可绑 RTP）。按 trans_mode 收窄允许集，与 priority 能力位图取交集，无交集则拦：

| trans_mode | 允许集 mask | 拦截条件（满足即拦） |
|------------|------------|---------------------|
| `URMA_TM_UM` | `UTP|CTP` | `(sup.value & mask) == 0` |
| `URMA_TM_RM` / `URMA_TM_RC` | `RTP|CTP` | `(sup.value & mask) == 0` |

mask 由 `urma_tp_type_to_mask` 构造（`UTP→0x4 / CTP→0x2 / RTP→0x1`，见 4.3.2）。依据：UM 不可绑 RTP（本需求给定）；`urma_check_tp_type_valid`（`urma_cp_api.c:1550`）显式拦 `非UM+UTP` 与 `UM+RTP`。

组合 priority（如 `rtp|utp`）配 UM 因 `sup.utp==1` 放行、配 RM 因 `sup.rtp==1` 放行，符合组合语义；单类型 priority 行为不变，无回归。

#### 4.2.2 代码改动

复用 4.3.2 的 `urma_tp_type_to_mask`（同文件，create 路径先于 bind 用）。在 `urma_create_jetty_check_dev_cap`（`urma_cp_api.c:1613`）末尾 `return 0` 前补（`cap`、`jfs_cfg` 已在函数内取好，`urma_cp_api.c:1615-1616`）：

```c
if (jfs_cfg->priority > URMA_MAX_PRIORITY) {
    URMA_LOG_ERR("jfs priority %u out of range [0, %u].\n",
        jfs_cfg->priority, URMA_MAX_PRIORITY);
    return -1;
}
uint32_t allow = (jfs_cfg->trans_mode == URMA_TM_UM)
    ? (urma_tp_type_to_mask(URMA_UTP) | urma_tp_type_to_mask(URMA_CTP))   /* UM: UTP|CTP */
    : (urma_tp_type_to_mask(URMA_RTP) | urma_tp_type_to_mask(URMA_CTP));  /* RM/RC: RTP|CTP */
union urma_tp_type_en sup = cap->priority_info[jfs_cfg->priority].tp_type;
if ((sup.value & allow) == 0) {
    URMA_LOG_ERR("priority %u (trans_mode=%d) supports none of allowed tp (sup=0x%x, allow=0x%x).\n",
        jfs_cfg->priority, (int)jfs_cfg->trans_mode, sup.value, allow);
    return -1;
}
```

字段依据：`jfs_cfg.priority` 为 `uint8_t`（`urma_types.h:559`），`URMA_MAX_PRIORITY=15`（`urma_opcode.h:96`），`priority_info[].tp_type` 为位图（`urma_types.h:168/296`）。

### 4.3 bind_jetty 阶段：精确 tp_type ↔ priority 校验

bind_jetty 同时持有本端 `jetty->jetty_cfg.jfs_cfg.priority` 与远端 `tjetty->tp_type`（`urma_types.h:796`），是唯一能做精确 tp 匹配的落点（create 无 tp_type、import 无 priority）。

#### 4.3.1 拦截原则

三个 bind 接口（`urma_bind_jetty` `urma_cp_api.c:2205`、`urma_bind_jetty_ex:2239`、`urma_bind_jetty_async:2387`）前置强制两端 `trans_mode == URMA_TM_RC`，故 trans_mode 恒为 RC、本端允许集 `{RTP, CTP}`；且 `tjetty->tp_type` 来自 import，已过 `urma_check_tp_type_valid`（`urma_cp_api.c:2024`），RC 下只能是 RTP/CTP。故 bind 不必再查 trans_mode↔tp 互斥，只做位图包含校验：

| 项 | 拦截条件 | 说明 |
|----|---------|------|
| tp_type 越界 | `tjetty->tp_type ∉ {RTP, CTP, UTP}` | 防御性，理论上 import 已保证 |
| priority 越界 | `jetty->jetty_cfg.jfs_cfg.priority > URMA_MAX_PRIORITY` | 同 4.2.1 |
| **tp 不被支持** | `priority_info[priority].tp_type` 位图不含 `tjetty->tp_type` 对应 bit | **核心**：例 priority 仅 utp、远端 tp_type=RTP → 拦 |

组合 priority（含 rtp+utp）配远端 RTP → `sup.rtp==1` 放行；配远端 CTP → `sup.ctp==1` 放行。单类型 priority 行为不变，无回归。

#### 4.3.2 代码改动

抽公共 helper（放在 `urma_cp_api.c` 文件作用域，**4.2.2 与 4.3.2 共用**枚举→mask 映射，消除重复）：

```c
/* 单值枚举 tp_type -> 位图 mask，4.2.2（create 构造允许集）与 4.3.2（bind 取远端单值）共用 */
static inline uint32_t urma_tp_type_to_mask(urma_tp_type_t tp_type)
{
    switch (tp_type) {
        case URMA_RTP: return 0x1;  /* bs.rtp */
        case URMA_CTP: return 0x2;  /* bs.ctp */
        case URMA_UTP: return 0x4;  /* bs.utp */
        default: return 0;
    }
}

/* bind 阶段：远端 tp_type 必须被本端 priority 能力位图包含（import 已保证 RC 下 tp_type∈{RTP,CTP}） */
static int urma_check_bind_tp_in_priority(urma_jetty_t *jetty, urma_target_jetty_t *tjetty)
{
    urma_device_cap_t *cap = &jetty->urma_ctx->dev->sysfs_dev->dev_attr.dev_cap;
    uint8_t priority = jetty->jetty_cfg.jfs_cfg.priority;

    if (priority > URMA_MAX_PRIORITY) {
        URMA_LOG_ERR("local jetty=%u priority %u out of range [0, %u].\n",
            jetty->jetty_id.id, priority, URMA_MAX_PRIORITY);
        return -1;
    }
    uint32_t mask = urma_tp_type_to_mask(tjetty->tp_type);
    if (mask == 0) {
        URMA_LOG_ERR("remote jetty tp_type=%d invalid.\n", (int)tjetty->tp_type);
        return -1;
    }
    union urma_tp_type_en sup = cap->priority_info[priority].tp_type;
    if ((sup.value & mask) == 0) {
        URMA_LOG_ERR("local jetty=%u priority %u does not support remote tp_type=%d (sup=0x%x).\n",
            jetty->jetty_id.id, priority, (int)tjetty->tp_type, sup.value);
        return -1;
    }
    return 0;
}
```

在三个 bind 接口的 `order_type` 校验之后、`ops->bind_*` 之前各加一处：

```c
if (urma_check_bind_tp_in_priority(jetty, tjetty) != 0) {
    return URMA_ENOPERM;
}
```

> 三处接口结构一致，可共用同一 helper；`urma_bind_jetty_ex` / `_async` 同样在 order_type 校验后插入。

---

## 五、内核态核查结论（本期不改内核）

> 本期不含内核改动。内核生产侧（`udma_main.c` `udma_set_sl` / `udma_construct_qos_param` 强制 rtp/ctp 互斥、不写 utp）不在本期范围，组合 priority 的实际生成待后续内核侧支持。本节仅核查内核消费侧，确认本期 user 态适配不会因内核消费点而失效。

### 5.1 消费侧核查

内核消费 `priority_info[].tp_type` 的点经全量扫描，**无 `tp_type.value ==` 整体相等比较**（零命中），均为位域判断或仅取 SL，组合场景天然安全：

| 点 | 代码 | 组合场景判定 |
|----|------|-------------|
| `udma_jetty.c:146` / `udma_jfs.c:319` | `ctx->sl = dev->priority_info[cfg->priority].SL` | 只取 SL，不读 tp_type，组合无影响 |
| `ubcore_cdev_file.c:1259` `tp_type_show_cb` | `snprintf("%u", ...tp_type.value)` | 打印整体 value，组合（如 0x5）显示"5"，不报错不阻断，仅 sysfs 可读性差 |
| `ub_mad.c:444` `ubmad_jetty_set_priority` | `if (...tp_type.bs.rtp == 1)` 选 priority | 位域判断，组合（rtp\|utp）下 `bs.rtp==1` 命中，行为正确 |
| `ipourma_res.c:678` | `...tp_type.bs.ctp == 1` | 位域，组合感知 |
| `ipourma_res.c:688-689` | `bs.utp==1 \|\| bs.rtp==1` | 位域，已是组合模板 |

### 5.2 不涉位图的内核路径（无需改，列证）

以下内核路径比较的是**单值枚举 `tp_type`**（远端/import 选定值），不消费 `priority_info[].tp_type` 位图，组合场景无影响：

| 代码点 | 写法 | 结论 |
|--------|------|------|
| `udma_jetty.c:2078-2111` `udma_check_tp_type_available` | `cfg->tp_type == UBCORE_XXP`（import 路径） | 单值枚举，无需改 |
| `ubcore_connect_adapter.c:960/1753-1755` 等 | `cfg->flag.bs.rtp/ctp/utp`（get_tp_cfg 位图，非 priority_info） | 另一套位图，语义独立，无需改 |
| `ubcore_tp.c:31` `ubcore_check_tp_type_valid` | trans_mode↔tp 单值互斥 | 语义正确，无需改 |

---

## 六、实施顺序与验证

### 6.1 改动文件清单

| 文件 | 改动 | 行数 |
|------|------|------|
| `admin_cmd_show.c` | `print_ubep_prioritys` 改分行打印（RTP/CTP/UTP 三行），直接读位图 | ~40 行 |
| `perftest_resources.c` | `get_jetty_priority_by_tp_type` + 两处校验改位与，加 helper | ~25 行 |
| `urma_cp_api.c` | `urma_create_jetty_check_dev_cap` 加 priority 越界 + trans_mode 能力面校验（4.2） | ~15 行 |
| `urma_cp_api.c` | 三处 bind_jetty 接口加 tp_type↔priority 精确匹配 + 公共 helper（4.3） | ~30 行 |

> 注：`urma_tp_type_en_to_string` 不再修改（保持单类型语义），组合展示由 `print_ubep_prioritys` 内部处理位图。内核本期不改（见第五章）。

### 6.2 验证

1. **admin 展示**：构造 `priority_info[p].tp_type.bs = {rtp=1, utp=1}` → `urma_admin show --whole` 该 priority 列第 1 行 `RTP`、第 3 行 `UTP`、第 2 行空；不再出现 `Invalid Value`。
2. **perftest 选 priority**：同上组合 priority，`urma_perftest`（默认 RTP）应选中该 priority，不再误报 "should set priority of type RTP"。
3. **perftest 校验**：`-O <组合priority>` + 默认 RTP 通过；`-O <组合priority>` + `--ctp` 通过；`-O <仅UTP priority>` + 默认 RTP（RC 路径）在 4.2 create 阶段早失败。
4. **jetty 校验（4.2）**：`UM` jfs + `sup` 仅 rtp → 报错；`UM` + 组合 priority（含 utp）→ 放行；`RC` + 纯 utp priority → 报错。
5. **bind 校验（4.3）**：priority 仅 utp + 远端 `tp_type=RTP` → 报错；priority 含 rtp+utp + 远端 RTP → 放行；priority 仅 rtp + 远端 CTP → 报错。三个 bind 接口行为一致。

### 6.3 回归

- 单类型 priority（纯 RTP/CTP/UTP）：admin 展示该列仅对应行有类型名；位与判断在单 bit 下等价于原全等比较，perftest/jetty 校验行为不变。
- 空能力 priority（value=0）：admin 三行该列均空；4.2/4.3 对任意 trans_mode/tp_type 均拦。
