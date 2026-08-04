# URMA topo 节点数 1024 → 1280 适配方案与内存增量分析

> 日期：2026-08-04
> 范围：URMA topo（拓扑）节点数硬编码 `MAX_NODE_NUM (1024)` 改为 `1280`，覆盖用户态与内核态全量修改点、ABI 兼容性、内存增量评估。

---

## 一、背景

URMA 的 topo（网络拓扑）节点数量上限采用硬编码 `#define MAX_NODE_NUM (1024)`，分布在内核态与用户态多个独立头文件中（互不 include，各有副本）。现计划从 1024 提升至 1280，需确认全量修改点、配套常量是否需要联动、以及内存与 ABI 兼容性影响。

### 关键结论速览

- **必须修改 6 处** `#define MAX_NODE_NUM (1024)` → `(1280)`，纯常量替换，无逻辑改动。
- **配套常量不动**：`EID_LEN / PORT_NUM / IODIE_NUM / DEV_NUM / CHIP_NUM` 决定单节点结构大小，两端已对齐且与节点数解耦。
- **无关 1024 不动**：`UBCORE_MAX_EID_CNT / UBCORE_MAX_UE_CNT / UBCORE_SIP_TABLE_SIZE / UBURMA_MAX_DEVICE`（独立资源上限）；sentry 的 `MAX_NODE_NUM=32`（多 die 心跳报数，与 topo 无引用）。
- **ABI 强约束**：ubagg `GET_TOPO_INFO` 输出结构嵌入 `topo_info[MAX_NODE_NUM]` 数组，两端必须同批部署，否则该接口返回 `-ENOSPC` 不可用（有校验兜底，不溢出）。
- **内存增量**：内核常驻 +81.2 MiB（vmalloc），瞬时峰值额外 +203 MiB；用户态每进程常驻 ~41 MiB、峰值 +40.6 MiB。

---

## 二、全量修改清单（6 处）

| # | 层级 | 文件 | 行 | 当前定义 | 目标定义 |
|---|------|------|----|---------|---------|
| 1 | 内核 | `01_kernel/drivers/ub/urma/ubcore/ubcore_topo_info.h` | 18 | `#define MAX_NODE_NUM (1024)` | `#define MAX_NODE_NUM (1280)` |
| 2 | 内核 | `01_kernel/drivers/ub/urma/ubagg/ubagg_topo_info.h` | 18 | `#define MAX_NODE_NUM (1024)` | `#define MAX_NODE_NUM (1280)` |
| 3 | 用户态 | `00_umdk/umdk/src/urma/lib/uvs/core/include/uvs_api.h` | 26 | `#define MAX_NODE_NUM (1024)` | `#define MAX_NODE_NUM (1280)` |
| 4 | 用户态 | `00_umdk/umdk/src/urma/lib/urma/bond/utils/bondp_topo_info.h` | 24 | `#define MAX_NODE_NUM (1024)` | `#define MAX_NODE_NUM (1280)` |
| 5 | 用户态 | `00_umdk/umdk/src/urma/tools/urma_admin/admin_parameters.h` | 26 | `#define MAX_NODE_NUM (1024)` | `#define MAX_NODE_NUM (1280)` |
| 6 | 用户态 | `00_umdk/umdk/src/urma/tools/urma_ping/ping_run.h` | 18 | `#define MAX_NODE_NUM (1024)` | `#define MAX_NODE_NUM (1280)` |

**说明**：

- 全部为纯常量替换，无逻辑改动。
- 6 处互相不 include（各有独立副本），可一次性同批修改，无顺序依赖。
- 部署必须用户态库 + 内核模块**同批升级**（见第四节兼容性）。

### 各定义点的用途

| # | 用途 |
|---|------|
| 1 | ubcore 全局 `ubcore_topo_map.topo_infos[]`、`set_topo` 校验（`node_num > MAX_NODE_NUM`）、`update_topo_map` 满校验（`node_num >= MAX_NODE_NUM` → `-ENOSPC`） |
| 2 | ubagg 全局 `ubagg_topo_map.topo_infos[]`、ioctl `SET_TOPO_INFO` / `GET_TOPO_INFO` 校验、`update` 满校验 |
| 3 | uvs 库 `urma_topo_map.topo_infos[]`、`uvs_set_topo_info` / `uvs_set_share_topo_info` 入参校验 |
| 4 | bond 库 topo_map、健康检查 `bondp_hc_ctx.nodes[MAX_NODE_NUM]`、`health_check_batch_node_num` 上限校验 |
| 5 | admin 工具 `tool_topo_map_t.topo_infos[]`、`admin_cmd_get_topo_info` 循环上限与校验 |
| 6 | ping 工具 `urma_ping_ubcore_topo_map_t.topo_infos[]`、`ping_get_topo_info` 循环上限与校验 |

---

## 三、配套常量与无关项说明

### 不需要改动的配套常量（决定单节点结构大小）

这些常量决定**单个 topo 节点**的内存布局，用户态与内核态已对齐，且与节点数 `MAX_NODE_NUM` 解耦，保持不变：

| 常量 | 值 | 含义 |
|------|----|------|
| `EID_LEN` | 16 | EID 字节长度 |
| `PORT_NUM` | 9 | 每节点端口数 |
| `IODIE_NUM` | 2 | 每 agg_dev 的 UE 数（`IODIE_NUM_PER_CHIP(1) × CHIP_NUM(2)`） |
| `DEV_NUM` | 256 | 每节点 agg_dev 数 |
| `CHIP_NUM` | 2 | chip 数 |

### 不需要改动的无关 1024（独立资源上限 / 独立语义）

| 常量 | 值 | 位置 | 不改原因 |
|------|----|------|---------|
| `UBCORE_MAX_EID_CNT` | 1024 | `ubcore_types.h:55` | EID 资源表上限，与 topo 节点数无绑定 |
| `UBCORE_MAX_UE_CNT` | 1024 | `ubcore_types.h:41` | UE 数量上限，独立 |
| `UBCORE_SIP_TABLE_SIZE` / `UBCORE_MAX_SIP` | 1024 | `ubcore_types.h:82-83` | SIP 表大小，独立 |
| `UBURMA_MAX_DEVICE` | 1024 | `uburma_main.c:74` | uburma 设备上限（minor bitmap），独立 |
| `URMA_MAX_EID_CNT` | 1024 | `urma_types.h:41` | 用户态 EID 上限，注释 refer to `UBCORE_MAX_SIP` |
| sentry `MAX_NODE_NUM` | 32 | `sentry_remote_reporter.h:23` | 多 die 心跳报数（与 `MAX_DIE_NUM=2` 配对），与 topo 完全无引用 |

> ⚠️ **易混淆点**：sentry 目录下也有 `MAX_NODE_NUM`，但值为 32 且语义独立，**切勿一并修改**。

---

## 四、ABI 兼容性分析

改 1280 的核心风险是**跨内核边界的数据结构大小**。topo 在用户态↔内核态间有三条通道，形态不同：

### 通道 A：ubagg ioctl — `SET_TOPO_INFO`（指针+数量）+ `GET_TOPO_INFO`（嵌入式数组）

| 子接口 | 形态 | ABI 依赖 |
|--------|------|---------|
| `SET_TOPO_INFO` | `struct { void *topo; uint32_t topo_num; } in;` | 弱依赖。内核 `copy_from_user(* node_num)` 按 `node_num` 拷贝，校验 `topo_num > MAX_NODE_NUM`（内核值守门）。两端不一致也能安全工作，但用户侧若仍 1024 则无法下发 1280。 |
| `GET_TOPO_INFO` | `struct ubagg_topo_info_out { topo_info[MAX_NODE_NUM]; uint32_t node_num; }` **直接嵌入数组** | **强依赖**。内核 `vzalloc(sizeof(内核结构))`、校验 `user_ctl->out.len < sizeof(内核结构)`、`copy_to_user(... sizeof(内核结构))`。 |

**GET_TOPO_INFO 不一致后果**：

- 内核 1280 / 用户 1024：用户缓冲区只有 1024 大小 → 内核 `out.len` 校验失败返回 `-ENOSPC`，**功能不可用**（有校验兜底，不会溢出）。
- 内核 1024 / 用户 1280：内核只写 1024 大小 → **安全但拿不到 1280 上限**。

→ **两端 `MAX_NODE_NUM` 必须一致，否则 GET_TOPO_INFO 不可用。**

### 通道 B：ubcore UVS 命令（TLV）— `SET_TOPO` only

`ubcore_cmd_set_topo { void *topo_info; uint32_t topo_num; } in;`

- 内核 `vzalloc(sizeof(struct ubcore_topo_map))`（含完整嵌入式数组）+ `copy_from_user(* node_num)`。
- 校验 `node_num > MAX_NODE_NUM`（内核值守门）。
- **无 GET 接口、无嵌入式输出数组**。
- 安全性与 SET_TOPO 相同：内核侧 MAX_NODE_NUM 守门，两端不一致时功能受限但不越界。

### 通道 C：ubagg netlink — `GET_TOPO`（逐节点，单节点结构）

ping / admin 工具使用，一次查一个节点：

```
arg->in.node_idx = i;
内核校验: arg->in.node_idx < topo_map->node_num   (动态值)
copy_to_user(... sizeof(*arg))                      (单节点大小)
```

- 单节点结构 `tool_topo_info_t` / `ubagg_topo_node` 字段对齐（已确认），且依赖的是 `IODIE_NUM / PORT_NUM / DEV_NUM`，**与 MAX_NODE_NUM 无关**。
- **ABI 完全不依赖 MAX_NODE_NUM**，自动兼容。
- MAX_NODE_NUM 在此只决定**本地数组大小和循环上限**。

### 兼容性结论

1. **ABI 必须两端同步**：通道 A 的 `GET_TOPO_INFO` 嵌入式数组大小 = `sizeof(topo_node) × MAX_NODE_NUM`，两端不一致 → `-ENOSPC` 不可用（有校验，不溢出）。
2. **配套常量不动**：单节点结构大小由 `IODIE_NUM / PORT_NUM / DEV_NUM` 决定，与 MAX_NODE_NUM 解耦，改 1280 不破坏单节点 ABI。
3. **顺序无依赖**：6 处都是独立 `#define`，无相互 include，可同批改。但**部署必须同批**（用户态库 + 内核模块一起升级），否则 `GET_TOPO_INFO` 在混部期间不可用。
4. **netlink 通道零风险**：通道 C 单节点结构不含 MAX_NODE_NUM，自动兼容。

---

## 五、内存增量分析

### 5.1 基础结构大小

| 结构 | 大小 | 构成 |
|------|------|------|
| `urma/ubcore/ubagg_topo_node` | ~166,228 B ≈ **162.3 KiB** | `4×4` + `bool links[18][18]`(324) + `agg_devs[256]`(每 agg_dev 648 B) |
| `topo_map`（含嵌入式数组） | @1024 ≈ **162.3 MiB** / @1280 ≈ **202.9 MiB** | 整数组内嵌 `topo_infos[MAX_NODE_NUM]` + `node_num` |
| `bondp_hc_node_t` | ~2,884 B ≈ **2.8 KiB**（估算） | `valid/hc_tjetty/no_cqe_round/probe_checked` 各 `[20][20]` + `pthread_rwlock_t` |
| `topo_map_t`（bond 压缩版） | @1024 ≈ **0.3 MiB** | 只存 `node_links[18][18]`，不存 agg_devs |

> 单节点构成明细：
> - `topo_ue` = `chip_id/die_id/entity_id`(12) + `primary_eid`(16) + `port_eid[9][16]`(144) + `cna[9][16]`(144) = 316 B
> - `topo_agg_dev` = `agg_eid`(16) + `ues[2]`(632) = 648 B
> - `topo_node` = `4×uint32`(16) + `links[18][18]`(324) + `agg_devs[256]`(166,144) = 166,228 B

### 5.2 内核态增量（常驻，全局单实例）

每个子模块各持一个全局 `static topo_map` 指针（`vzalloc` 分配，vmalloc 区）：

| 实例 | @1024 | @1280 | 增量 |
|------|-------|-------|------|
| ubcore `g_ubcore_topo_map` | 162.3 MiB | 202.9 MiB | +40.6 MiB |
| ubagg `g_topo_map` | 162.3 MiB | 202.9 MiB | +40.6 MiB |
| **内核常驻合计** | 324.6 MiB | 405.8 MiB | **+81.2 MiB** |

**瞬时峰值**（非常驻，用完释放）：

- ubagg `GET_TOPO_INFO` 路径 `vzalloc(sizeof(ubagg_topo_info_out))` 临时分配一份完整结构 → @1280 峰值 ~203 MiB，用完 `vfree`。
- ubcore `SET_TOPO` 路径 `vzalloc(sizeof(ubcore_topo_map))` 同理临时分配 → 峰值 ~203 MiB。
- 峰值瞬时约 **+203 MiB**，常驻不保留。

### 5.3 用户态增量（按进程实例计）

| 进程 / 库 | 实例类型 | @1024 | @1280 | 增量 |
|-----------|---------|-------|-------|------|
| uvs 库（daemon） | `urma_topo_map` 常驻 + ioctl 临时缓冲 | 162.3 MiB | 202.9 MiB | +40.6 MiB |
| bond 库 `info_out`（per 进程） | `calloc` 完整数组，**峰值**（释放） | 162.3 MiB 峰值 | 202.9 MiB 峰值 | +40.6 MiB 峰值 |
| bond 库压缩版 `topo_map_t` | 常驻（只存 links） | ~0.3 MiB | ~0.4 MiB | +0.1 MiB |
| bond 健康检查 `hc_ctx.nodes[]` | 常驻（嵌入式） | ~2.8 MiB | ~3.5 MiB | +0.7 MiB |
| admin 工具 `tool_topo_map_t` | 本地数组 | 162.3 MiB | 202.9 MiB | +40.6 MiB |
| ping 工具 `urma_ping_ubcore_topo_map_t` | 本地数组 | 162.3 MiB | 202.9 MiB | +40.6 MiB |

**用户态典型常驻增量**（一个 uvs daemon + 一个使用 bond 的业务进程）：

≈ +40.6（uvs topo_map）+ 0.7（bond hc_ctx）+ 0.1（bond 压缩 topo）≈ **~41.4 MiB**

峰值（set/get topo 时临时缓冲）额外 **+40.6 MiB**。

### 5.4 风险点

1. **内核 vmalloc 区压力**：常驻 +81 MiB + 瞬时峰值 +203 MiB，需确认目标平台 vmalloc 预留（`vmalloc` 总区通常 256 MiB ~ 数 GiB，可配）。`vzalloc` 分配 203 MiB 连续虚拟空间若紧张可能失败。
2. **用户态 RSS**：topo_map 作为嵌入式数组直接计入进程 RSS；admin / ping 工具一次性 ~203 MiB 堆分配，注意 `ulimit` / 容器内存限额。
3. **栈分配隐患**：`urma_topo_map` / `tool_topo_map_t` / `urma_ping_ubcore_topo_map_t` 若被声明为**栈上局部变量**会直接爆栈（203 MiB ≫ 8 MiB 栈）。需核查 admin / ping 工具是否栈分配（当前 bond 库走 `calloc`、内核走 `vzalloc`，但工具侧需确认）。

---

## 六、后续建议（可选）

1. **直接落地 6 处修改**：一次性替换 `1024` → `1280`。
2. **核查栈分配风险**：确认 admin / ping 工具是否把 `tool_topo_map_t` / `urma_ping_ubcore_topo_map_t` 声明在栈上，避免爆栈。
3. **内存重构（单独立项）**：把嵌入式 `topo_infos[MAX_NODE_NUM]` 改为指针 + 按需动态分配，可从根本上消除 ~203 MiB 常驻 / 峰值内存，但属 ABI 重构，需评估工作量与回归范围。

---

## 附：修改后的部署校验

部署后可通过以下方式验证两端一致性：

- `urma_admin show topo` / `urma_ping` 拉取 topo，确认能正常返回 1280 上限且不报 `-ENOSPC`。
- `dmesg` 检查内核 `ubagg user ctl has no enough space` / `topo map is full` 等错误日志是否消失。
- 若出现 `-ENOSPC`，说明用户态与内核态 `MAX_NODE_NUM` 未同步，需检查是否有遗漏的 1024。
