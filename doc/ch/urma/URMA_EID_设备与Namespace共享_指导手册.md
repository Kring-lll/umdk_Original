# URMA EID / 设备 / Namespace 共享指导手册

> 走读范围：用户态 `00_umdk/umdk/`、内核态 `01_kernel/drivers/ub/urma/`（ubcore / uburma / ubagg）
> 基于源码走读生成，所有结论带 `file:line` 可回溯
> 主题：用户态与内核态文件总结、EID 与 ubcore_device 对应的 UE 设备关系、容器 namespace 场景下共享 EID 与 device 的方案与配置

---

## 0. 一句话结论

- **`ubcore_device` 是全局资源**（挂在单一全局链表 `g_device_list`），但 **EID 是 per-netns 资源**（每个 `ubcore_eid_entry` 带 `struct net *net` 字段）。
- 内核提供两个独立的全局开关 `g_dev_ns_shared` / `g_eid_ns_shared`，分别控制 device 与 EID 是否跨 network namespace 共享。**默认是独占（exclusive）模式**，不同 container 默认看不到彼此的 device/EID。
- 要实现"不同 namespace 共享同一份 EID 和 ubcore_device"，开 `urma_admin system set dev_sharing on` + `eid_sharing on` 即可；或用 `urma_admin dev/eid/agg expose ... ns` 做精细化逐项暴露。
- **部署坑点**：`/dev/uburma/*` 与 `/dev/ubagg` 的字符设备 class **不是 netns-aware**，即便共享开关全开，容器内也默认看不到这些 `/dev` 节点，需容器 runtime 做 bind-mount。

---

## 1. 文件清单总结

### 1.1 内核态（`01_kernel/drivers/ub/urma/`）

#### ubcore/ — 核心层（device / EID / namespace / cdev / genl）

| 文件 | 职责（与本主题相关） |
|---|---|
| [ubcore_device.c](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c) / [.h](01_kernel/drivers/ub/urma/ubcore/ubcore_device.h) | **核心**。全局 `g_device_list`、`ubcore_device` 注册/注销/查找、EID 表 `eid_table`、EID 命名空间归属 `eid_entry.net`、**两个共享开关** `g_dev_ns_shared`/`g_eid_ns_shared`、device ns 迁移（`set_dev_ns`/`expose_dev_ns`/`unexpose_dev_ns`/`set_dev_eid_ns`）、pernet ops（`net_init`/`net_exit`）、netns-aware class、`ubcore_get_device_by_eid` |
| [ubcore_cdev_file.c](01_kernel/drivers/ub/urma/ubcore/ubcore_cdev_file.c) | sysfs 设备属性；**`eid_show_cb` 按 netns 过滤 EID 显示** |
| [ubcore_genl.c](01_kernel/drivers/ub/urma/ubcore/ubcore_genl.c) / [ubcore_genl_define.h](01_kernel/drivers/ub/urma/ubcore/ubcore_genl_define.h) | genl family `UBCORE_GENL` 注册，`.netnsok = true` |
| [ubcore_genl_admin.c](01_kernel/drivers/ub/urma/ubcore/ubcore_genl_admin.c) | **管理面 genl 命令**：`set_dev_ns_mode_ops`/`set_eid_ns_mode_ops`/`expose_dev_ns_ops`/`set_dev_ns_ops`/`set_dev_eid_ns_ops`/`unexpose_dev_ns_ops` 等 |
| [ubcore_main_ue_eid.c](01_kernel/drivers/ub/urma/ubcore/ubcore_main_ue_eid.c) / [.h](01_kernel/drivers/ub/urma/ubcore/ubcore_main_ue_eid.h) | **全局 trie** `g_ubcore_eid_trie`：任意 EID → 主 UE EID 映射，带引用计数 + `FIRST_ADD`/`LAST_DEL` 事件 |
| [ubcore_eid_trie.c](01_kernel/drivers/ub/urma/ubcore/ubcore_eid_trie.c) / [.h](01_kernel/drivers/ub/urma/ubcore/ubcore_eid_trie.h) | Patricia Trie 实现（被 main_ue_eid 使用） |
| [ubcore_cgroup.c](01_kernel/drivers/ub/urma/ubcore/ubcore_cgroup.c) | RDMA cgroup 资源计费（`HCA_HANDLE`/`HCA_OBJECT`），**与 netns 隔离无关** |
| [ubcore_uvs_cmd.c](01_kernel/drivers/ub/urma/ubcore/ubcore_uvs_cmd.c) | UVS（用户态守护）通过 `/dev/ubcore` 下发的命令：EID 表 add/del entry、topo、main_ue_eid 批量插入 |
| [ubcore_topo_info.c](01_kernel/drivers/ub/urma/ubcore/ubcore_topo_info.c) / [.h](01_kernel/drivers/ub/urma/ubcore/ubcore_topo_info.h) | 全局拓扑图 `g_ubcore_topo_map`：node → `agg_devs[]` → `ues[IODIE_NUM]`（含 `primary_eid`/`port_eid[]`/`cna[]`/`entity_id`），路径计算 |
| [ubcore_priv.h](01_kernel/drivers/ub/urma/ubcore/ubcore_priv.h) | 私有结构：`ubcore_ue_table`/`ubcore_ue_entry`（ue_idx→uvs 映射） |
| [ubcore_main.c](01_kernel/drivers/ub/urma/ubcore/ubcore_main.c) | 模块 init/exit，串联 class/cdev/genl/pernet/workqueue |

#### uburma/ — 用户态接口层（cdev）

| 文件 | 职责 |
|---|---|
| [uburma_main.c](01_kernel/drivers/ub/urma/uburma/uburma_main.c) | 模块 init/exit，注册为 `ubcore_client`；`g_uburma_class`（**无** ns_type/namespace 回调）；创建 `/dev/uburma/<name>` |
| [uburma_dev_ops.c](01_kernel/drivers/ub/urma/uburma/uburma_dev_ops.c) | `uburma_open`/`close`/mmap，open 不做 netns 过滤 |
| [uburma_cmd.c](01_kernel/drivers/ub/urma/uburma/uburma_cmd.c) | ioctl 入口与分发；**`uburma_fill_eid_list` 按 `current->nsproxy->net_ns` 过滤 EID**；`alloc_ucontext` 调 `ubcore_alloc_ucontext`（内部做 ns 检查） |
| [uburma_uobj.c](01_kernel/drivers/ub/urma/uburma/uburma_uobj.c) / [.h](01_kernel/drivers/ub/urma/uburma/uburma_uobj.h) | uobj 生命周期与事务 |

#### ubagg/ — bonding/聚合层

| 文件 | 职责 |
|---|---|
| [ubagg_main.c](01_kernel/drivers/ub/urma/ubagg/ubagg_main.c) | 模块 init/exit；`g_ubagg_class`（**无** ns 回调）；`/dev/ubagg` 字符设备 |
| [ubagg_netlink.c](01_kernel/drivers/ub/urma/ubagg/ubagg_netlink.c) / [.h](01_kernel/drivers/ub/urma/ubagg/ubagg_netlink.h) | genl family `UBAGG_GENL`（`.netnsok = true`），topo/物理设备/v2p 查询，`genlmsg_multicast_allns` 广播 |
| [ubagg_topo_info.c](01_kernel/drivers/ub/urma/ubagg/ubagg_topo_info.c) / [.h](01_kernel/drivers/ub/urma/ubagg/ubagg_topo_info.h) | 全局 `g_topo_map`；`ubagg_get_primary_eid_by_agg_eid` |
| [ubagg_ioctl.c](01_kernel/drivers/ub/urma/ubagg/ubagg_ioctl.c) | `/dev/ubagg` ioctl；bonding device 注册时**强制 `eid_entries[0].net = &init_net`** |
| [ubagg_types.h](01_kernel/drivers/ub/urma/ubagg/ubagg_types.h) | `struct ubagg_device`（内嵌 `ubcore_device` + `bonding_eid` + slave 列表） |

#### 头文件（`01_kernel/include/ub/urma/`）

| 文件 | 关键定义 |
|---|---|
| [ubcore_types.h](01_kernel/include/ub/urma/ubcore_types.h) | **核心数据结构**：`ubcore_device`(:3445)、`ubcore_eid_entry`(:3368)、`ubcore_eid_table`(:3375)、`ubcore_logic_device`(:3429)、`ubcore_client`(:3529) |

### 1.2 用户态（`00_umdk/umdk/src/urma/`）

#### lib/urma/core — 核心用户态库

| 文件 | 职责 |
|---|---|
| [lib/urma/core/urma_main.c](00_umdk/umdk/src/urma/lib/urma/core/urma_main.c) | 库主入口；全局 device list 维护、provider 加载（dlopen `/usr/lib64/urma/*.so`）；`urma_get_device_list`/`urma_get_eid_list`/`urma_get_device_by_name`/`urma_get_device_by_eid`/`urma_create_context` |
| [lib/urma/core/urma_device.c](00_umdk/umdk/src/urma/lib/urma/core/urma_device.c) / [.h](00_umdk/umdk/src/urma/lib/urma/core/urma_device.h) | 扫描 `/sys/class/ubcore` 枚举设备、解析属性、读 EID 列表（sysfs 或 ioctl） |
| [lib/urma/core/urma_cmd.c](00_umdk/umdk/src/urma/lib/urma/core/urma_cmd.c) / [urma_cmd_tlv.c](00_umdk/umdk/src/urma/lib/urma/core/urma_cmd_tlv.c) | 封装对 `/dev/uburma/<dev>` 的 `ioctl(URMA_CMD)` |
| [lib/urma/core/urma_format_convert.c](00_umdk/umdk/src/urma/lib/urma/core/urma_format_convert.c) | `urma_str_to_eid`/`urma_u32_to_eid`：字符串/IP → `urma_eid_t` |
| [lib/urma/core/urma_private.h](00_umdk/umdk/src/urma/lib/urma/core/urma_private.h) | `urma_sysfs_dev_t`、`urma_is_bonding_dev()` |
| [lib/urma/core/include/urma_types.h](00_umdk/umdk/src/urma/lib/urma/core/include/urma_types.h) | `urma_eid_t`(:136)、`urma_device_t`(:371)、`urma_context_t`(:390)、`urma_eid_info_t`(:403) |
| [lib/urma/core/include/urma_api.h](00_umdk/umdk/src/urma/lib/urma/core/include/urma_api.h) | 公共 API 声明 |

#### lib/urma/bond — bonding/UE 聚合层

| 文件 | 职责 |
|---|---|
| [lib/urma/bond/bondp_main.c](00_umdk/umdk/src/urma/lib/urma/bond/bondp_main.c) | 以 `__attribute__((constructor))` 注册 `ub_agg` provider ops |
| [lib/urma/bond/bondp_provider_ops.c](00_umdk/umdk/src/urma/lib/urma/bond/bondp_provider_ops.c) | bonding device provider ops；通过 `GET_SLAVE_DEVICE` 拉取 UE 列表、按 primary/port EID 建多个 p_ctx |
| [lib/urma/bond/bondp_types.h](00_umdk/umdk/src/urma/lib/urma/bond/bondp_types.h) | `bondp_device_t`(:107)、`bondp_context_t`(:117，v_ctx + p_ctxs[]) |
| [lib/urma/bond/ubagg_ioctl.h](00_umdk/umdk/src/urma/lib/urma/bond/ubagg_ioctl.h) | `bondp_physical_device_t`(:36，含 `primary_eid_idx` + `port_eid_idx[PORT_NUM]`) |
| [lib/urma/bond/utils/topo_info.h](00_umdk/umdk/src/urma/lib/urma/bond/utils/topo_info.h) | 拓扑常量（`IODIE_NUM=2`、`PORT_NUM=9`）、`bondp_topo_ue_t`、eid 映射 |
| [lib/urma/bond/bondp_netlink.c](00_umdk/umdk/src/urma/lib/urma/bond/bondp_netlink.c) | genl 客户端，连 `UBAGG_GENL`、订阅 `bonding` multicast |

#### tools/urma_admin — 管理命令行工具

| 文件 | 职责 |
|---|---|
| [tools/urma_admin/admin_cmd_system.c](00_umdk/umdk/src/urma/tools/urma_admin/admin_cmd_system.c) | **`system set dev_sharing/eid_sharing`、`system show`** |
| [tools/urma_admin/admin_cmd_dev.c](00_umdk/umdk/src/urma/tools/urma_admin/admin_cmd_dev.c) | `admin_nl_set_dev_ns`/`expose_dev_ns`/`unexpose_dev_ns` |
| [tools/urma_admin/admin_cmd_eid.c](00_umdk/umdk/src/urma/tools/urma_admin/admin_cmd_eid.c) | `admin_nl_set_eid_ns`（单个 EID 绑 ns） |
| [tools/urma_admin/admin_cmd_agg.c](00_umdk/umdk/src/urma/tools/urma_admin/admin_cmd_agg.c) | `agg expose <eid> <netns>`：暴露聚合设备及关联 EID |
| [tools/urma_admin/admin_parameters.c](00_umdk/umdk/src/urma/tools/urma_admin/admin_parameters.c) | `admin_get_ns_fd`：解析 `/proc/$pid/ns/net` 拿 ns_fd |
| [tools/urma_admin/admin_cmd.c](00_umdk/umdk/src/urma/tools/urma_admin/admin_cmd.c) | 顶层命令树，含 `set_ns_mode`/`set_dev_ns` legacy 入口 |

#### 文档

| 文件 | 关键内容 |
|---|---|
| [doc/ch/urma/URMA User Guide.ch.md](00_umdk/umdk/doc/ch/urma/URMA%20User%20Guide.ch.md) | UE/逻辑设备/命名空间机制(:1556-1590)、命令清单(:1572-1590)、`<netns>` 格式(:1990)、多 ns sysctl 限制(:2329) |
| [doc/ch/urma/URMA API Guide.ch.md](00_umdk/umdk/doc/ch/urma/URMA%20API%20Guide.ch.md) | 每 ns 每 dev 最多 80 EID(:1685) |

---

## 2. 整体架构与组件关系

```
        ┌──────────────────── 用户态 ────────────────────┐
        │  应用 / urma_ping / urma_perftest              │
        │      │ liburma.so (core: urma_* API)           │
        │      │ liburma_bond.so (bond: bonding 调度)    │
        │      │ urma_admin (管理命令行)                 │
        └──────┬─────────────────────────────────────────┘
               │ ① ioctl(/dev/uburma/<dev>, URMA_CMD)
               │ ② ioctl(/dev/ubagg)
               │ ③ genl: UBCORE_GENL / UBAGG_GENL
        ═══════╪══════════════════════════════════════════ 系统调用
        ┌──────┴──────────────────────────────────────────┐
        │  内核态 drivers/ub/urma/                          │
        │                                                  │
        │  ubcore (g_device_list 全局)                     │
        │   ├─ ubcore_device.eid_table[] (per-EID net 归属)│
        │   ├─ ubcore_device.ldev.net (主逻辑设备 ns)       │
        │   ├─ g_ubcore_eid_trie (全局: EID→main_ue_eid)   │
        │   ├─ g_dev_ns_shared / g_eid_ns_shared (开关)    │
        │   └─ pernet ops (net_init/net_exit)              │
        │  uburma (/dev/uburma/<name>, cdev, uobj)        │
        │  ubagg  (/dev/ubagg, bonding genl, topo)         │
        └──────────────────────────────────────────────────┘
               │ UB 硬件 (ubus/udma/ubfi/ummu...)
               ▼
```

**三个字符设备 + 两个 genl family**：

| 通道 | 节点 / family | class netns-aware? | 用途 |
|---|---|---|---|
| cdev | `/dev/uburma/<name>` | **否** | URMA 业务 ioctl（create_ctx/jetty/seg/EID 列表等） |
| cdev | `/dev/ubagg` | **否** | bonding topo/物理设备查询 |
| cdev | `/dev/ubcore` | 是（但节点在 init_net） | UVS 守护进程下发管理命令 |
| genl | `UBCORE_GENL` | `.netnsok=true`（跨 ns 可见） | admin 命令（ns 模式/expose 等） |
| genl | `UBAGG_GENL` | `.netnsok=true` | topo 查询、failback 通知 |

---

## 3. EID 与 ubcore_device 对应的 UE 设备关系

### 3.1 核心数据结构

**`ubcore_device`**（[ubcore_types.h:3445-3492](01_kernel/include/ub/urma/ubcore_types.h#L3445)）——一个 UB 设备，全局唯一，挂 `g_device_list`。关键字段：

```c
struct ubcore_device {
    struct list_head list_node;        // 挂入全局 g_device_list
    char dev_name[UBCORE_MAX_DEV_NAME];
    struct net_device *netdev;
    enum ubcore_transport_type transport_type;
    bool dynamic_eid;
    struct ubcore_eid_table eid_table;     // ← EID 表（多个 EID）
    struct ubcore_logic_device ldev;       // ← 主逻辑设备（含 net 归属）
    struct list_head ldev_list;            // ← 其它 netns 的逻辑设备副本
    void **ue2uvs_table;                   // ← ue_idx → uvs_instance 映射
    ...
};
```

> **`ubcore_device` 本身没有 `struct net *` 字段**。ns 归属放在 `ldev.net`（主）和 `eid_entries[i].net`（per-EID）。

**`ubcore_eid_entry`**（[ubcore_types.h:3368-3373](01_kernel/include/ub/urma/ubcore_types.h#L3368)）：

```c
struct ubcore_eid_entry {
    union ubcore_eid eid;     // 16 字节 EID
    uint32_t eid_index;
    struct net *net;          // ← 此 EID 所属 network namespace（per-netns！）
    bool valid;
};

struct ubcore_eid_table {
    uint32_t eid_cnt;
    struct ubcore_eid_entry *eid_entries;   // 数组
    spinlock_t lock;
};
```

**`ubcore_logic_device`**（[ubcore_types.h:3429-3437](01_kernel/include/ub/urma/ubcore_types.h#L3429)）——device 在某 netns 下的逻辑暴露，带 `possible_net_t net`。

### 3.2 UE 概念与 EID 层次

**UE = Unified-bus Entity**（文档 `URMA User Guide.ch.md:1556`）：UB 设备中一份具备隔离性的设备资源集合，是可寻址实体。一个物理设备可含多个 UE 和多个 Port。

EID 的层次（从底到顶）：

```
                ┌─────────────────────────────────┐
                │  agg / bonding EID              │  ← 聚合设备对外 EID（ubagg_device.bonding_eid）
                │  (ubcore_topo_agg_dev.agg_eid)  │     固定绑 init_net，不受容器隔离
                ├──────────────┬──────────────────┤
                │   UE[0]      │      UE[1]       │  ← IODIE_NUM = 2 个 UE / 聚合设备
                │ primary_eid  │  primary_eid    │  ← 每 UE 一个主 EID（受 eid_sharing 控制）
                │ port_eid[9]  │  port_eid[9]     │  ← 每 UE 最多 9 个端口 EID（PORT_NUM=9）
                │ cna[9]       │  cna[9]          │
                └──────────────┴──────────────────┘
                    │ main_ue_eid（全局 trie，不属于任何 device/netns）
                    │ g_ubcore_eid_trie: 任意 EID → 主 UE EID
```

- **port EID**：每 UE 最多 9 个，对应物理端口（`ubcore_topo_ue.port_eid[PORT_NUM]`）。
- **primary EID**：每 UE 一个主 EID（`ubcore_topo_ue.primary_eid`）。
- **agg/bonding EID**：聚合设备对外 EID（`ubcore_topo_agg_dev.agg_eid` = `ubagg_device.bonding_eid`）。从 bonding EID 取 UE 的 primary_eid 用 `ubagg_get_primary_eid_by_agg_eid(agg_eid, &primary_eid, ue_id)`。
- **main UE EID**：独立维度，全局 trie `g_ubcore_eid_trie`（[ubcore_main_ue_eid.c:37](01_kernel/drivers/ub/urma/ubcore/ubcore_main_ue_eid.c#L37)）把任意 EID 映射到主 UE EID，带引用计数 + `FIRST_ADD`/`LAST_DEL` 事件。**不属于任何 device，不绑 netns**。

### 3.3 device : EID : UE 对应关系

| 关系 | 结论 | 证据 |
|---|---|---|
| device ↔ EID | **一对多**：一个 `ubcore_device` 的 `eid_table.eid_entries[]` 是数组，`eid_cnt` 个 EID | [ubcore_types.h:3375](01_kernel/include/ub/urma/ubcore_types.h#L3375) |
| device ↔ UE | **一个物理 UE = 一个底层 ubcore_device**（网卡驱动注册，`transport_type=UB`）；多个 UE 上聚合为一个 bonding `ubcore_device`（`bonding_dev` 前缀） | [ubcore_topo_info.h](01_kernel/drivers/ub/urma/ubcore/ubcore_topo_info.h) |
| bonding device ↔ UE | **一个 bonding_dev 挂 2 个 UE**（`IODIE_NUM=2`），每 UE 有 1 primary + 最多 9 port EID | 用户态 [topo_info.h:27](00_umdk/umdk/src/urma/lib/urma/bond/utils/topo_info.h#L27) |
| device ↔ uvs | `ue2uvs_table`：ue_idx → uvs_instance 映射 | [ubcore_types.h:3487](01_kernel/include/ub/urma/ubcore_types.h#L3487) |

### 3.4 EID → device 解析路径

报文到达/查找 device 用 `ubcore_get_device_by_eid(eid, type)`（[ubcore_device.c:2649](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2649)）：遍历 `g_device_list` + 每个 device 的 `eid_entries`，`memcmp(eid)` + `transport_type` 匹配。**不检查 netns**——所有 ns 的代码都能按 EID 找到同一个 device 对象。namespace 过滤发生在后续的访问校验环节（§4.2）。

---

## 4. 容器 Namespace 共享机制（核心）

### 4.1 两个全局共享开关

定义在 [ubcore_device.c:79-92](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L79)：

```c
static bool g_dev_ns_shared;   // device 是否跨 ns 共享（默认 false = exclusive）
static bool g_eid_ns_shared;   // EID 是否跨 ns 共享（默认 false = exclusive）

bool ubcore_dev_ns_shared(void) { return READ_ONCE(g_dev_ns_shared); }
bool ubcore_eid_ns_shared(void) { return READ_ONCE(g_eid_ns_shared); }
```

两个开关**独立**：可单独开 device 共享而保持 EID 隔离，反之亦然。设置函数 `ubcore_set_dev_ns_mode()`(:2175)、`ubcore_set_eid_ns_mode()`(:2237)。

### 4.2 可见性/访问控制逻辑

**device 可见性**——`ubcore_dev_accessible(dev, net)`（[ubcore_device.c:1686](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L1686)）：

```c
if (ubcore_dev_ns_shared() || net_eq(net, read_pnet(&dev->ldev.net))   // 主 ldev 所属 ns
    || <net 在 dev->ldev_list 副本里>)                                  // 被 expose 的 ns
    return true;
return false;
```

**EID 可见性**——sysfs `eid_show_cb()`（[ubcore_cdev_file.c:833](01_kernel/drivers/ub/urma/ubcore/ubcore_cdev_file.c#L833)）与 ioctl `uburma_fill_eid_list()`（[uburma_cmd.c:4066](01_kernel/drivers/ub/urma/uburma/uburma_cmd.c#L4066)）：

```c
if (!e->valid || (!ubcore_eid_ns_shared() && !net_eq(e->net, current->nsproxy->net_ns)))
    // 显示空 / 不返回（隐藏）
else
    // 显示真实 EID
```

即：共享关 → EID 只对 `e->net` 所属 ns 可见；开 → 全局可见。

**访问网关**——用户态建 context 前校验（[ubcore_device.c:1725](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L1725)）：

```c
if (!ubcore_dev_accessible(dev, current->nsproxy->net_ns) ||
    !ubcore_eid_accessible(dev, eid_index))
    return -EPERM;   // 拒绝
```

> **注意**：内核数据面（报文收发）路径**不查 ns**（`current->nsproxy` 在内核线程上下文不稳定），ns 校验只在用户态入口（`alloc_ucontext`/`eid_valid`/`eid_show`/`fill_eid_list`）做。

### 4.3 两类字符设备 class 的 netns 差异（部署关键）

| class | netns-aware? | `/dev`/`/sys` 节点归属 | 容器内默认可见 |
|---|---|---|---|
| `g_ubcore_class`（ubcore） | **是**（有 `.ns_type`/`.namespace` 回调，[ubcore_device.c:178-207](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L178)） | `/sys/class/ubcore/<name>` 按 `ldev.net` 归属到对应 ns 的 sysfs | **自动可见** |
| `g_uburma_class`（uburma） | **否**（无 ns 回调，[uburma_main.c:81-84](01_kernel/drivers/ub/urma/uburma/uburma_main.c#L81)） | `/dev/uburma/<name>` 只在 init_net devtmpfs | **默认不可见** |
| `g_ubagg_class`（ubagg） | **否**（[ubagg_main.c:87-90](01_kernel/drivers/ub/urma/ubagg/ubagg_main.c#L87)） | `/dev/ubagg` 只在 init_net devtmpfs | **默认不可见** |

**结论**：即便开了 `dev_sharing`/`eid_sharing`，`/dev/uburma/*` 和 `/dev/ubagg` 节点**不会自动出现在容器 netns 里**。容器内进程要访问，必须由容器 runtime/部署工具做 bind-mount 把这些节点挂进来（§5.3）。

### 4.4 genl family 跨 ns 可见

- `ubcore_genl_family`（[ubcore_genl.c:242](01_kernel/drivers/ub/urma/ubcore/ubcore_genl.c#L242)）：`.netnsok = true`
- `ubagg` family（[ubagg_netlink.c:92](01_kernel/drivers/ub/urma/ubagg/ubagg_netlink.c#L92)）：`.netnsok = true`
- `ubagg_nl_broadcast` 用 `genlmsg_multicast_allns` 跨所有 ns 广播

`.netnsok = true` 表示 family 注册到 init_net 全局 genl 表、所有 netns 用户都能发命令——这是控制面通道跨 ns 可见的原因，但**不等于**设备按 ns 隔离被取消，隔离仍由 §4.2 的 accessible 校验决定。

### 4.5 共享/独占模式对比

| 维度 | exclusive（默认） | shared |
|---|---|---|
| `g_dev_ns_shared` / `g_eid_ns_shared` | false | true |
| device 可见范围 | 仅 `ldev.net` 或被 expose 的 ns | 所有 ns |
| EID 可见范围 | 仅 `e->net` 所属 ns | 所有 ns |
| netns 创建时 | 无自动操作 | 为每个 UB device 建 ldev 副本（[ubcore_net_init:2522](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2522)） |
| netns 销毁时 | device 迁回 init_net + EID 重置（[ubcore_net_exit:2480](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2480)） | 删 ldev 副本 + 按 eid_ns_shared 决定 reset/invalidate EID |
| 精细化 expose（`dev/eid expose ... ns`） | **支持** | **不支持**（[ubcore_device.c:2265](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2265) 拒绝） |
| 模式切换约束 | shared→exclusive 需无 netns 存在且无 device 已暴露 | exclusive→shared 需无 device 已建 ldev（否则为每个 device 在所有 ns 建 ldev 副本） |

### 4.6 逻辑设备机制（文档定义）

来自 [URMA User Guide.ch.md:1556-1590](00_umdk/umdk/doc/ch/urma/URMA%20User%20Guide.ch.md)：

- **物理设备仅属一个命名空间**；可在不同命名空间创建**同名逻辑设备**（独立访问接口，仅本 ns 可见）。
- **EID 可配 ns 属性**并绑定到对应 ns 的逻辑设备，**EID 仅在其所属 ns 可见**；删逻辑设备时其上 EID 自动迁回物理设备。
- 两种工作模式：
  - **自动模式**：ns 创建/销毁时自动为所有 URMA 物理设备创建/销毁逻辑设备，所有容器可直接识别使用。← 对应 `dev_sharing=on`
  - **手动模式**：用户主动控制逻辑设备创建/删除/归属 ns。← 对应 `dev_sharing=off` + 手动 expose

### 4.7 特殊：bonding EID 固定全局

[ubagg_ioctl.c:1304](01_kernel/drivers/ub/urma/ubagg/ubagg_ioctl.c#L1304)：bonding device 注册时强制 `eid_entries[0].net = &init_net`。**bonding/聚合口 EID 始终全局可见，不参与容器 EID 隔离**；容器场景下受 `eid_sharing` 控制的是底层 primary/port EID。

### 4.8 cgroup 与 netns 无关

[ubcore_cgroup.c](01_kernel/drivers/ub/urma/ubcore/ubcore_cgroup.c) 仅做 RDMA cgroup 的 `HCA_HANDLE`/`HCA_OBJECT` 资源计费（`#ifdef CONFIG_CGROUP_RDMA`），无任何 netns/容器隔离逻辑——它是进程级配额，不是容器网络隔离。

### 4.9 用户态库不感知 netns

liburma（core + bond）全部源码**无 `setns`/`unshare`/`/proc/*/ns/net` 操作**（唯一命中的 "unshared jfr" 是日志字符串）。设备发现走 `/sys/class/ubcore`，EID 读 sysfs 或 `/dev/uburma/<dev>` 的 `URMA_CMD_GET_EID_LIST` ioctl，bonding 走 `UBAGG_GENL` netlink——**这些通道都不绑进程 netns**。隔离完全由内核侧按 ns_fd 给逻辑设备/EID 打标签实现，用户态库透明。

---

## 5. 配置与部署

### 5.1 命令清单（`urma_admin`）

| 命令 | 作用 | 用户态 genl 命令 | 内核 ops / 实现 |
|---|---|---|---|
| `urma_admin system show` | 查询 `DEV_NS_SHARED`/`EID_NS_SHARED` 状态 | `UBCORE_CMD_SHOW_SYSTEM` | [ubcore_genl_admin.c:427](01_kernel/drivers/ub/urma/ubcore/ubcore_genl_admin.c#L427) |
| `urma_admin system set dev_sharing {on\|off}` | device 全局共享开关 | `URMA_CORE_SET_DEV_NS_MODE` + `UBCORE_ATTR_DEV_NS_MODE` | `ubcore_set_dev_ns_mode_ops` [:394](01_kernel/drivers/ub/urma/ubcore/ubcore_genl_admin.c#L394) → `ubcore_set_dev_ns_mode` [:2175](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2175) |
| `urma_admin system set eid_sharing {on\|off}` | EID 全局共享开关 | `URMA_CORE_SET_EID_NS_MODE` + `UBCORE_ATTR_EID_NS_MODE` | `ubcore_set_eid_ns_mode_ops` [:405](01_kernel/drivers/ub/urma/ubcore/ubcore_genl_admin.c#L405) → `ubcore_set_eid_ns_mode` [:2237](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2237) |
| `urma_admin dev set <dev> ns <netns>` | 设置物理设备 ns 归属 | `URMA_CORE_SET_DEV_NS` | `ubcore_set_dev_ns_ops` [:453](01_kernel/drivers/ub/urma/ubcore/ubcore_genl_admin.c#L453) → `ubcore_set_dev_ns` [:2128](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2128) |
| `urma_admin dev expose <dev> <netns>` | 暴露设备到某 ns | `URMA_CORE_EXPOSE_DEV_NS` | `ubcore_expose_dev_ns_ops` [:464](01_kernel/drivers/ub/urma/ubcore/ubcore_genl_admin.c#L464) → `ubcore_expose_dev_ns` [:2258](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2258) |
| `urma_admin dev unexpose <dev> <netns>` | 撤销暴露 | `URMA_CORE_UNEXPOSE_DEV_NS` | `ubcore_unexpose_dev_ns_ops` [:475](01_kernel/drivers/ub/urma/ubcore/ubcore_genl_admin.c#L475) |
| `urma_admin eid set <dev> <eid_idx> ns <netns>` | 设置单个 EID 的 ns | `URMA_CORE_SET_DEV_EID_NS` | `ubcore_set_dev_eid_ns_ops` [:486](01_kernel/drivers/ub/urma/ubcore/ubcore_genl_admin.c#L486) → `ubcore_set_dev_eid_ns` [:2409](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2409) |
| `urma_admin agg expose <eid> <netns>` | 暴露聚合设备及关联 EID | （组合上述） | [admin_cmd_agg.c:241](00_umdk/umdk/src/urma/tools/urma_admin/admin_cmd_agg.c#L241) |
| `urma_admin set_ns_mode -M 0\|1` (legacy) | 同 dev_sharing | — | 同上 |
| `urma_admin set_dev_ns --dev <d> --ns <p>` (legacy) | 同 dev set ns | — | [admin_cmd.c:273](00_umdk/umdk/src/urma/tools/urma_admin/admin_cmd.c#L273) |

`<netns>` 参数格式统一为 `/proc/$pid/ns/net`（[URMA User Guide.ch.md:1990](00_umdk/umdk/doc/ch/urma/URMA%20User%20Guide.ch.md)）。用户态 `admin_get_ns_fd`（[admin_parameters.c:406](00_umdk/umdk/src/urma/tools/urma_admin/admin_parameters.c#L406)）解析该路径拿 ns_fd，**工具自身不 setns**，只把 ns_fd 经 netlink 传给内核完成归属设置。

### 5.2 场景 A：全局资源共享（多容器共享同一份 device/EID）

**适用**：多容器跑同一应用的多个实例，共享底层 UB 设备与 EID 池。

```bash
# 在宿主机或特权容器执行（需能访问 urma_admin 且 device 尚未暴露）
urma_admin system set dev_sharing on    # device 全局共享
urma_admin system set eid_sharing on     # EID 全局共享
urma_admin system show                   # 确认 DEV_NS_SHARED=on / EID_NS_SHARED=on
```

开启后：
- `ubcore_dev_accessible`/`ubcore_eid_accessible` 对所有 ns 返回 true，所有容器进程都能访问该 device 与 EID。
- netns 创建时自动为每个 UB device 建 ldev 副本（[ubcore_net_init:2522](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2522)），让 sysfs 节点在新 ns 也可见。

**约束**：
- exclusive→shared 需无 device 已建逻辑设备（`ldev_list` 非空则拒绝，[ubcore_device.c:2196-2208](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2196)）；否则会为每个 device 在所有已存在 ns 建 ldev 副本（`ubcore_copy_logic_devices`）。
- shared→exclusive 需无自定义 netns 存在（[:2187](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2187)）。
- 共享模式下**不能再用** `dev/eid expose`（[:2265](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2265) 拒绝）。

### 5.3 容器内字符设备可见性（部署必读）

即便 §5.2 全局共享已开，`/dev/uburma/*` 与 `/dev/ubagg` 节点**不会自动出现在容器 netns**（§4.3）。容器内进程要使用 URMA，需由容器 runtime 做 bind-mount：

```bash
# 方式一：docker run 时挂载（只读共享）
docker run --device /dev/uburma/ubdma0:/dev/uburma/ubdma0 \
           --device /dev/ubagg:/dev/ubagg \
           ... your-image

# 方式二：bind-mount（特权或合适 CAP）
mount --bind /dev/uburma/ubdma0  /path/to/container/dev/uburma/ubdma0
mount --bind /dev/ubagg          /path/to/container/dev/ubagg

# 方式三：容器内 mknod（需 CAP_MKNOD + 同主从设备号）
#   先在宿主查设备号：ls -l /dev/uburma/ubdma0  →  主:从
#   容器内：mknod /dev/uburma/ubdma0 c <主> <从> && chmod ...
```

`/sys/class/ubcore/*` 因 ubcore class 是 netns-aware，共享模式下会自动出现在容器 sysfs，无需手动处理。

### 5.4 场景 B：精细化暴露（不全局共享，按需暴露给特定容器）

**适用**：不同容器用不同 device/EID，需隔离但偶尔共享。

```bash
# 默认 exclusive 模式下
# 把 device dev0 暴露到容器 ns（ns 由容器 pid定位）
urma_admin dev expose dev0 /proc/$(docker inspect -f '{{.State.Pid}}' ctr1)/ns/net

# 把 dev0 的 EID idx=2 绑定到容器 ns
urma_admin eid set dev0 2 ns /proc/$(docker inspect -f '{{.State.Pid}}' ctr1)/ns/net

# 暴露聚合设备及关联 EID
urma_admin agg expose <bonding_eid> /proc/$pid/ns/net

# 撤销
urma_admin dev unexpose dev0 /proc/$pid/ns/net
```

精细暴露与全局共享互斥（§4.5）。netns 销毁时被暴露的 device 迁回 init_net、EID 重置（[ubcore_net_exit:2480](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2480)）。

### 5.5 前置依赖与权限

```bash
# 内核模块
lsmod | grep -E "ubcore|ubagg|uburma|udma"   # 需全部加载
# genl family 注册检查
cat /proc/net/genl | grep -iE "ubcore|ubagg"

# 权限
chmod -R 777 /usr/lib64/urma
chmod 777 /dev/ummu/tid
chmod 755 /usr/lib64/liburma*
```

### 5.6 限额与已知限制

| 项 | 值/说明 | 出处 |
|---|---|---|
| 每 ns 每 dev EID 上限 | 80 | [URMA API Guide.ch.md:1685](00_umdk/umdk/doc/ch/urma/URMA%20API%20Guide.ch.md) |
| 多 ns sysctl | 不支持 | [URMA User Guide.ch.md:2329](00_umdk/umdk/doc/ch/urma/URMA%20User%20Guide.ch.md) |
| 容器内 sysfs 设备去重 | 已知未完成逻辑（`Todo: check ... in container`，直接 return true） | [urma_device.c:469](00_umdk/umdk/src/urma/lib/urma/core/urma_device.c#L469) |
| bonding EID | 固定 init_net，不参与容器隔离 | [ubagg_ioctl.c:1304](01_kernel/drivers/ub/urma/ubagg/ubagg_ioctl.c#L1304) |

---

## 6. 运维排障

### 6.1 常见故障与定位

| 现象 | 可能原因 | 定位 | 处置 |
|---|---|---|---|
| 容器内 `urma_get_device_list` 返回空 | device 在该 ns 不可见（exclusive 且未 expose），或 sysfs 未挂载 | 容器内 `ls /sys/class/ubcore/`；宿主 `urma_admin system show` | 开 `dev_sharing on` 或 `dev expose` |
| 容器内 open `/dev/uburma/x` 失败 (ENOENT) | `/dev` 节点未 bind-mount 进容器（§4.3） | `ls -l /dev/uburma/` | bind-mount 或 `--device` 挂载 |
| 容器内 `alloc_ucontext` 返回 -EPERM | `ubcore_dev_accessible`/`eid_accessible` 未通过 | `urma_admin system show` 看开关；确认 EID 所属 ns | 开 `eid_sharing` 或 `eid set ... ns` |
| 切换 shared 时 `-EPERM "existing ns"` | shared→exclusive 时已有自定义 netns | `ip netns list` | 先删所有自定义 ns 再切 |
| 切换 shared 时 `-EPERM "has logic devices"` | exclusive→shared 时有 device 已暴露 ldev | `urma_admin dev show` | 先 unexpose 所有再切 |
| 容器启动失败（pernet init 报错） | `ubcore_net_init` 失败（不应阻断 netns 创建，但会告警） | `dmesg \| grep ubcore` | 检查 device 状态，init 失败被设计为不阻断容器启动 |
| 多容器跑 URMA 时资源超限 | cgroup 配额（`HCA_HANDLE`/`HCA_OBJECT`） | `cat /sys/fs/cgroup/rdma/.../...` | 调整 rdma cgroup 限额 |

### 6.2 关键观测点

```bash
# 查询当前共享模式
urma_admin system show
# 输出：
#   DEV_NS_SHARED: on/off
#   EID_NS_SHARED: on/off

# 内核日志
dmesg | grep -iE "ubcore|ns mode|expose|logic device" | tail

# 查看 device 列表与 EID 归属（宿主）
ls /sys/class/ubcore/
cat /sys/class/ubcore/<dev>/eids/eid*    # exclusive 下非本 ns 的 EID 显示为空

# 查看自定义 netns
ip netns list

# genl family 注册
cat /proc/net/genl | grep -iE "ubcore|ubagg"
```

### 6.3 端到端验证清单

1. ✅ 宿主 `lsmod` 有 `ubcore ubagg uburma udma`，`/proc/net/genl` 有 `ubcore`/`ubagg` family。
2. ✅ `urma_admin system show` 确认共享开关符合预期。
3. ✅ 容器内 `ls /sys/class/ubcore/` 能看到 device（共享模式或已 expose）。
4. ✅ 容器内 `/dev/uburma/*` 和 `/dev/ubagg` 节点存在（bind-mount 成功）。
5. ✅ 容器内 `urma_ping <eid>` 能收发（数据面打通）。

---

## 7. 设计要点总结

1. **device 全局、EID per-netns**：`ubcore_device` 无 net 字段、挂全局 `g_device_list`；EID 表项 `ubcore_eid_entry.net` 各自带 ns 归属。device ↔ EID 一对多，device ↔ UE 一对一/多对一聚合。
2. **两层共享开关**：`g_dev_ns_shared`/`g_eid_ns_shared` 独立控制，默认 exclusive。切换有严格约束（无 ns/无 ldev 副本）。
3. **访问控制网关在用户态入口**：`ubcore_alloc_ucontext` 做 `dev_accessible`+`eid_accessible` 校验；数据面不查 ns。
4. **三类 cdev class 的 netns 差异是部署核心坑**：ubcore class netns-aware（sysfs 自动可见），uburma/ubagg class 非 netns-aware（`/dev` 节点必须 bind-mount）。
5. **genl 跨 ns 可见**：`UBCORE_GENL`/`UBAGG_GENL` 均 `.netnsok=true`，控制面通道天然跨 ns，但设备隔离仍由 accessible 校验决定。
6. **bonding EID 全局**：固定 `init_net`，不受容器隔离，容器场景下底层 primary/port EID 才受 `eid_sharing` 控制。
7. **main_ue_eid 全局 trie**：独立维度，把任意 EID 映射到主 UE EID，不属任何 device/netns。
8. **用户态库透明**：liburma 不感知 netns，看到的是当前进程 ns 能看到的 sysfs/dev 节点；隔离全由内核按 ns_fd 标签实现。

---

## 8. 附录：关键源码索引速查

### 数据结构
| 主题 | 位置 |
|---|---|
| `ubcore_device` | [ubcore_types.h:3445](01_kernel/include/ub/urma/ubcore_types.h#L3445) |
| `ubcore_eid_entry` / `ubcore_eid_table` | [ubcore_types.h:3368](01_kernel/include/ub/urma/ubcore_types.h#L3368) / [:3375](01_kernel/include/ub/urma/ubcore_types.h#L3375) |
| `ubcore_logic_device` | [ubcore_types.h:3429](01_kernel/include/ub/urma/ubcore_types.h#L3429) |
| `ubcore_client` | [ubcore_types.h:3529](01_kernel/include/ub/urma/ubcore_types.h#L3529) |
| `urma_eid_t` / `urma_device_t` | 用户态 [urma_types.h:136](00_umdk/umdk/src/urma/lib/urma/core/include/urma_types.h#L136) / [:371](00_umdk/umdk/src/urma/lib/urma/core/include/urma_types.h#L371) |

### namespace 开关与访问控制
| 主题 | 位置 |
|---|---|
| `g_dev_ns_shared`/`g_eid_ns_shared` 定义 | [ubcore_device.c:79-92](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L79) |
| `ubcore_set_dev_ns_mode` | [ubcore_device.c:2175](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2175) |
| `ubcore_set_eid_ns_mode` | [ubcore_device.c:2237](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2237) |
| `ubcore_dev_accessible` | [ubcore_device.c:1686](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L1686) |
| `ubcore_alloc_ucontext`（访问网关） | [ubcore_device.c:1725](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L1725) |
| `ubcore_expose_dev_ns` | [ubcore_device.c:2258](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2258) |
| `ubcore_set_dev_eid_ns` | [ubcore_device.c:2409](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2409) |
| `ubcore_net_init`/`net_exit` | [ubcore_device.c:2522](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2522) / [:2480](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2480) |
| netns-aware class 回调 | [ubcore_device.c:178-207](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L178) |

### genl 管理面
| 主题 | 位置 |
|---|---|
| `set_dev_ns_mode_ops`/`set_eid_ns_mode_ops` | [ubcore_genl_admin.c:394](01_kernel/drivers/ub/urma/ubcore/ubcore_genl_admin.c#L394) / [:405](01_kernel/drivers/ub/urma/ubcore/ubcore_genl_admin.c#L405) |
| `expose_dev_ns_ops`/`set_dev_ns_ops`/`set_dev_eid_ns_ops` | [ubcore_genl_admin.c:464](01_kernel/drivers/ub/urma/ubcore/ubcore_genl_admin.c#L464) / [:453](01_kernel/drivers/ub/urma/ubcore/ubcore_genl_admin.c#L453) / [:486](01_kernel/drivers/ub/urma/ubcore/ubcore_genl_admin.c#L486) |
| `UBCORE_GENL` family `.netnsok=true` | [ubcore_genl.c:242](01_kernel/drivers/ub/urma/ubcore/ubcore_genl.c#L242) |
| `UBAGG_GENL` family `.netnsok=true` | [ubagg_netlink.c:92](01_kernel/drivers/ub/urma/ubagg/ubagg_netlink.c#L92) |
| bonding EID 固定 init_net | [ubagg_ioctl.c:1304](01_kernel/drivers/ub/urma/ubagg/ubagg_ioctl.c#L1304) |

### EID 过滤（可见性）
| 主题 | 位置 |
|---|---|
| sysfs `eid_show_cb` | [ubcore_cdev_file.c:833](01_kernel/drivers/ub/urma/ubcore/ubcore_cdev_file.c#L833) |
| ioctl `uburma_fill_eid_list` | [uburma_cmd.c:4066](01_kernel/drivers/ub/urma/uburma/uburma_cmd.c#L4066)（过滤 :4083-4084） |

### main_ue_eid / UE 拓扑
| 主题 | 位置 |
|---|---|
| 全局 trie `g_ubcore_eid_trie` | [ubcore_main_ue_eid.c:37](01_kernel/drivers/ub/urma/ubcore/ubcore_main_ue_eid.c#L37) |
| `ubcore_get_device_by_eid` | [ubcore_device.c:2649](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2649) |
| `ubcore_topo_ue` / `ubcore_topo_agg_dev` | [ubcore_topo_info.h](01_kernel/drivers/ub/urma/ubcore/ubcore_topo_info.h) |

### 用户态工具
| 主题 | 位置 |
|---|---|
| `system set dev_sharing/eid_sharing` | [admin_cmd_system.c:177](00_umdk/umdk/src/urma/tools/urma_admin/admin_cmd_system.c#L177) |
| `agg expose` | [admin_cmd_agg.c:241](00_umdk/umdk/src/urma/tools/urma_admin/admin_cmd_agg.c#L241) |
| `admin_get_ns_fd` | [admin_parameters.c:406](00_umdk/umdk/src/urma/tools/urma_admin/admin_parameters.c#L406) |
| `urma_get_device_list`/`get_eid_list` | [urma_main.c:314](00_umdk/umdk/src/urma/lib/urma/core/urma_main.c#L314) / [:356](00_umdk/umdk/src/urma/lib/urma/core/urma_main.c#L356) |
| `urma_get_device_by_eid` | [urma_main.c:449](00_umdk/umdk/src/urma/lib/urma/core/urma_main.c#L449) |

---

*本手册基于源码走读生成，所有结论可回溯至上述 file:line。核心结论：device 全局、EID per-netns；默认 exclusive，通过 `dev_sharing`/`eid_sharing` 开全局共享或 `expose` 精细化暴露；uburma/ubagg 的 `/dev` 节点在容器内需 bind-mount。源码版本演进时，请以实际代码为准并同步本手册。*
