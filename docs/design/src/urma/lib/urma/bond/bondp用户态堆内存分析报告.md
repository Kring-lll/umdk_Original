# bondp 用户态堆内存消耗分析报告

> 范围：仅分析用户态堆内存（calloc/malloc/memalign），不含 ioctl 陷入内核态的内核侧分配（如 `urma_cmd_create_context`/`urma_cmd_import_jetty` 内核侧对象、udma queue `qbuf` 走 `udma_u_alloc_kernel_buf` 的 mmap）。
> 对象：bondp 控制面两个关键 API —— `bondp_create_context`（context 建立）、`bondp_import_jetty`（建链）及其全部内层调用。
> 典型场景：2 IODie × 2 端口（本端/对端各 4 个 member device，4 条 connected 路径）。
> 编号说明：①~⑮ 为 `bondp_create_context` 路径的申请点；A~C 为 `bondp_import_jetty` 路径的申请点（与 create_context 路径相互独立）。

---

## 目录

- [一、参数配置条件总览](#一参数配置条件总览)
- [二、bondp_create_context 堆内存分析](#二bondp_create_context-堆内存分析)
  - [2.1 调用树与申请点](#21-调用树与申请点)
  - [2.2 结构体大小估算](#22-结构体大小估算)
  - [2.3 默认场景（hc 关闭）占比](#23-默认场景hc-关闭占比)
  - [2.4 hc 开启场景精细化占比](#24-hc-开启场景精细化占比)
- [三、bondp_import_jetty 堆内存分析](#三bondp_import_jetty-堆内存分析)
  - [3.1 调用树与申请点](#31-调用树与申请点)
  - [3.2 结构体大小估算](#32-结构体大小估算)
  - [3.3 典型场景占比](#33-典型场景占比)
- [四、核心问题定位](#四核心问题定位)
- [五、内存优化方案设计](#五内存优化方案设计)
- [六、落地优先级建议](#六落地优先级建议)
- [七、总结](#七总结)

---

## 一、参数配置条件总览

bondp 堆内存消耗高度依赖环境变量与拓扑参数。下表汇总各申请点的触发条件与可调参数。

### 1.1 环境变量开关

| 环境变量 | 默认值 | 影响的申请点 | 说明 |
|----------|--------|--------------|------|
| `BOND_ENABLE_HEALTH_CHECK` | `false` | ⑧⑨⑩⑪⑫⑬、jetty/sq wrid | 关闭时 hc 子树完全不分配（省 ~4.4 MB/context）；开启时每 context 引入完整 hc 子树 |
| `BOND_ENABLE_FAILOVER` | 见代码 | ⑭⑮（failback task 表） | failback 依赖 failover；failover 关闭时 fb_ctx 仍建表但任务量小 |
| `BOND_ENABLE_FAILBACK` | 见代码 | ⑭⑮ | 关闭则 `bondp_fb_init` 不执行（省 ~8 KB/context） |
| `BOND_HEALTH_CHECK_ACTIVE_INTERVAL` | 见代码 | ⑧ 探测周期 | 仅影响探测频率，不影响单次分配大小 |

### 1.2 拓扑/容量常量（决定固定分配大小）

| 常量 | 值 | 定义位置 | 影响申请点 | 含义 |
|------|----|----------|-----------|------|
| `IODIE_NUM` | 2 | `utils/topo_info.h` | 真实路径数 | IODie 数 |
| `URMA_ACTIVE_PORT_PER_DIE` | 2 | `bond/include/urma_ubagg.h` | 真实路径数 | 每 IODie 激活端口数 → member 总数 4 |
| `URMA_UBAGG_DEV_MAX_NUM` | 20 | `bond/include/urma_ubagg.h` | ①A/⑧ node 矩阵维度 | 固定分配上限，2×2 场景利用率 1% |
| `BONDP_MAX_NUM_JETTYS` | 10240 | `bondp_types.h` | ② | vjetty id hash bucket 数 |
| `BONDP_MAX_NUM_RSEGS` | 10240 | `bondp_types.h` | ③ | remote v2p hash bucket 数 |
| `BONDP_FB_TASK_TABLE_SIZE` | 1024 | `bondp_failback.c` | ⑮ | failback task hash bucket 数 |
| `MAX_NODE_NUM` | 1024 | `utils/topo_info.h` | ⑧ nodes 数组 | hc node 预分配维度，典型用 2，利用率 0.2% |
| `HC_PROBE_DEPTH` | 1024 | `bondp_health.c` | jetty sq wrid | 探测 SQ 深度 → wrid 8 KB/组 |
| jfc_cfg.depth（hc 探测） | 4096 | `bondp_health.c` `hc_init_probe_resource` | ⑩ jfc CQ wrid | 探测 CQ 深度 → wrid 32 KB/组 |
| `HC_PROBE_SEG_LEN` | `getpagesize()`=4096 | `bondp_health.c` | ⑫ probe buf | 探测段长度 |

### 1.3 两个 API 的关系

| API | 触发时机 | 频次 | 内存生命周期 |
|-----|----------|------|--------------|
| `bondp_create_context` | 应用打开 bonding 设备建 context | 每 bonding 设备 1 次（context 级） | 长生命周期，随 context 删除释放 |
| `bondp_import_jetty` | 应用每次建链（import 远端 jetty） | 每条连接 1 次（连接级） | 中生命周期，随 unimport 释放 |

> 二者内存互不重叠：create_context 分配 context 级基础设施（hash 表/hc/fb），import_jetty 分配连接级对象（target_jetty/pjetty）。

---

## 二、bondp_create_context 堆内存分析

### 2.1 调用树与申请点

```
bondp_create_context                                          [bondp_provider_ops.c:660]
├─ calloc(bondp_context_t)                       ★① 主对象          [L667]
├─ bondp_create_vcontext                                             [L401]
│    ├─ bdp_p_vjetty_id_table_create                                [L403]
│    │    └─ bondp_hash_table_create → ub_hmap_init
│    │         └─ calloc(ub_hmap_head × (mask+1))       ★② bucket   [ub_hmap.h:197]
│    ├─ bdp_r_v2p_token_id_table_create                             [L408]
│    │    └─ 同上
│    │         └─ calloc(ub_hmap_head × (mask+1))       ★③ bucket   [ub_hmap.h:197]
│    ├─ urma_cmd_create_context → 纯 ioctl，用户态零堆分配           [urma_cmd.c]
│    └─ epoll_create(1)  → fd，非堆内存                              [L428]
├─ get_topo_info_from_ko  (仅首个 context，全局共享)                 [L366]
│    ├─ calloc(struct ubagg_topo_info_out)  ★④ 临时，用完 free       [L371]
│    ├─ urma_cmd_user_ctl → 纯 ioctl
│    └─ create_topo_map                                               [topo_info.c:140]
│         ├─ calloc(topo_map_t + node_num×topo_node)  ★⑤ 全局持久     [L154]
│         └─ eid_mapping_hash_table_create
│              └─ calloc(bucket)            ★⑥ 全局持久              [L175]
├─ bondp_create_pcontext                                             [L556]
│    └─ 对每个 member dev（bonding_level 匹配的）：
│        urma_create_context(m->dev, m->eid_index)                   [urma_main.c:502]
│          └─ urma_open_cdev → fd，非堆
│          └─ dev->ops->create_context = udma_u_create_context       [udma_u_ops.c:267]
│               └─ calloc(struct udma_u_context)   ★⑦ 每个 pctx 1个   [L280]
│                  (后续 urma_cmd_create_context 为 ioctl，不计)
├─ bondp_init_ctx_enabled_indices  无堆分配                           [L630]
├─ bondp_hc_init_from_env  (仅 enable_health_check=true 时)          [L652]
│    └─ bondp_hc_init                                                 [bondp_health.c:702]
│         ├─ calloc(bondp_hc_ctx_t)           ★⑧ 含 nodes[1024] ~4.1MB [L714]
│         ├─ hc_init_nodes → 操作内嵌 nodes，无额外堆分配             [L471]
│         └─ hc_init_probe_resources → 每个 p_ctx 创建探测通道       [L668]
│              └─ hc_init_probe_resource                              [L552]
│                   ├─ urma_create_jfce → udma calloc(jfce)  ★⑨       [L555]
│                   ├─ urma_create_jfc  → udma calloc(jfc)   ★⑩       [L565]
│                   ├─ urma_create_jfr  → udma calloc(jfr)   ★⑪       [L577]
│                   ├─ urma_register_seg → calloc(seg)       ★⑬       [L598]
│                   ├─ hc_create_probe_jetty → calloc(jetty)         [L604]
│                   └─ memalign(HC_PROBE_SEG_LEN)            ★⑫ buf   [L583]
└─ bondp_fb_init  (无条件，但依赖 fb_ctx 非空)                        [L636]
     └─ fb_task_table_create                                          [bondp_failback.c:116]
          ├─ calloc(bondp_fb_ctx_t)                           ★⑭       [L118]
          └─ ub_hmap_init(task_map, 1024)
               └─ calloc(ub_hmap_head × (mask+1))            ★⑮      [ub_hmap.h:197]
```

**申请点汇总：**

| 编号 | 申请位置 | 对象 | 触发条件 | 频次 |
|------|----------|------|----------|------|
| ① | `bondp_create_context` | `bondp_context_t` | 无条件 | 每次 1 次 |
| ② | `bdp_p_vjetty_id_table_create` | hash bucket（10240 项） | 无条件 | 每次 1 次 |
| ③ | `bdp_r_v2p_token_id_table_create` | hash bucket（10240 项） | 无条件 | 每次 1 次 |
| ④ | `get_topo_info_from_ko` | `ubagg_topo_info_out`（临时） | 首个 context 且 topo 未缓存 | 用完释放 |
| ⑤ | `create_topo_map` | `topo_map_t` + nodes | 首个 context | 全局 1 次 |
| ⑥ | `eid_mapping_hash_table_create` | hash bucket | 首个 context | 全局 1 次 |
| ⑦ | `udma_u_create_context` | `udma_u_context` | 每个 member（bonding_level 匹配） | 每 pctx 1 次 |
| ⑧ | `bondp_hc_init` | `bondp_hc_ctx_t`（含 nodes[1024]） | `BOND_ENABLE_HEALTH_CHECK=true` | 每次 1 次 |
| ⑨⑩⑪⑬ | `hc_init_probe_resource` | jfce/jfc/jfr/seg/jetty | hc 开启 | 每 pctx 1 组 |
| ⑩ jfc CQ wrid | `udma_u_alloc_queue_buf` | wrid[4096] | hc 开启 | 每 pctx 1 个 |
| jetty sq wrid | `udma_u_alloc_queue_buf` | wrid[1024] | hc 开启 | 每 pctx 1 个 |
| ⑫ | `hc_init_probe_resource` | 探测 buf (memalign) | hc 开启 | 每 pctx 1 个 |
| ⑭ | `fb_task_table_create` | `bondp_fb_ctx_t` | 无条件 | 每次 1 次 |
| ⑮ | `fb_task_table_create` | hash bucket（1024 项） | 无条件 | 每次 1 次 |

> 说明：`urma_cmd_create_context`（vcontext/pcontext 内核 context）、`urma_cmd_user_ctl`（topo 查询）均为 ioctl 封装，用户态零堆分配。`epoll_create`/`urma_open_cdev`/`epoll_ctl` 返回 fd，非堆。jfc/jetty 的 `qbuf`（CQ/SQ 命令缓冲）走 `udma_u_alloc_kernel_buf`（ioctl/mmap），不计用户态堆，只有 `wrid` 是 calloc。`sw_db` 走 db_page 池化摊销，单次近似为 0。

### 2.2 结构体大小估算

**① `bondp_context_t`**（[bondp_types.h:117](src/urma/lib/urma/bond/bondp_types.h#L117)）
`v_ctx`(urma_context_t ~88B) + `p_ctxs[20]`(160B) + 2×`bondp_hash_table_t`(内嵌，bucket 另算，~48B×2) + `enabled_indices[20]`(80B) + 其余 ~40B ≈ **~440 B**
（注：内嵌 hash_table 的 bucket 是指针，单独 calloc，见 ②③）

**②③ hash bucket**（[ub_hmap.h:197](src/urma/common/include/ub_hmap.h#L197)）
`calc_mask(10240)` → mask=16383 → bucket 数=16384 → `sizeof(ub_hmap_head)=8B` → **128 KB/个**，两个 = **256 KB**

**⑤ `topo_map_t`**（[topo_info.c:58](src/urma/lib/urma/bond/utils/topo_info.c#L58)）
`sizeof(topo_map_t) + node_num×sizeof(bondp_topo_node_t)`。node_num 典型 2，`bondp_topo_node_t`~400B → **~800 B + bucket**（全局共享）

**⑦ `udma_u_context`**（[udma_u_common.h:90](src/urma/hw/udma/udma_u_common.h#L90)）
`urma_ctx`(88B) + db_addr/page_size/db_list[UDMA_DB_TYPE_NUM] + 2×mutex + `jetty_table[UDMA_JETTY_TABLE_NUM]`/`jfr_table[...]` + 其余 ≈ **~400 B**

**⑧ `bondp_hc_ctx_t`**（[bondp_health.c:72](src/urma/lib/urma/bond/bondp_health.c#L72)）★ 内存大头
`probes[20]`(20×~64B=1280B) + `nodes[1024]`×`bondp_hc_node_t`

`bondp_hc_node_t`（[bondp_health.c:55](src/urma/lib/urma/bond/bondp_health.c#L55)）：

| 字段 | 计算 | 字节 |
|------|------|------|
| `valid[20][20]` (atomic_bool) | 400 | 400 |
| `hc_tjetty[20][20]` (指针) | 400×8 | 3200 |
| `probe_checked[20][20]` (bool) | 400 | 400 |
| `lock`+`tjetty_list` | — | ~80 |
| **每 node 合计** | | **~4080** |

`bondp_hc_ctx_t` 合计 ≈ 1280 + 1024×4080 ≈ **4.18 MB**

**⑭ `bondp_fb_ctx_t`** + **⑮ fb bucket**
fb_ctx ~32B + bucket(`calc_mask(1024)`→mask=1023→1024×8) = **~8 KB**

### 2.3 默认场景（hc 关闭）占比

pctx 数 = 4（2 IODie × 2 端口），topo 为全局共享（仅首个 context 计入）。

**单次 create_context 堆内存（非首个，topo 已缓存）：**

| 申请点 | 计算 | 字节 |
|--------|------|------|
| ① bondp_context_t | — | 440 |
| ② vjetty id bucket | 128 KB | 131072 |
| ③ remote v2p bucket | 128 KB | 131072 |
| ⑦ udma_u_context ×4 | 4×400 | 1600 |
| ⑭ fb_ctx | — | 32 |
| ⑮ fb task bucket | 1024×8 | 8192 |
| **合计** | | **~272 KB** |

占比：

| 申请点 | 字节 | 占比 |
|--------|------|------|
| ②③ hash bucket ×2 | 262144 | **96.3%** |
| ⑮ fb bucket | 8192 | 3.0% |
| ⑦ pctx ×4 | 1600 | 0.6% |
| ① 主对象 | 440 | 0.2% |
| ⑭ fb_ctx | 32 | 0.0% |

**首个 context** 额外计入全局 topo（⑤⑥）~1 KB，总计 ~273 KB，占比极小。

### 2.4 hc 开启场景精细化占比

hc 开启时，在 2.3 基础上额外引入 hc 子树：`bondp_hc_ctx_t` 主对象（含预分配 `nodes[1024]`）+ 每 p_ctx 探测资源（共 4 组）。

**每 p_ctx 探测资源（`hc_init_probe_resource`）逐项：**

| 申请点 | 对象 | 计算依据 | 单 pctx 字节 |
|--------|------|----------|--------------|
| ⑨ jfce | `urma_jfce_t` | ~24B（含 fd/ctx） | 24 |
| ⑩ jfc 对象 | `udma_u_jfc` | 含 `urma_jfc_t`+`udma_u_jetty_queue`+sw_db 指针 | ~160 |
| ⑩ jfc CQ wrid | `wrid[baseblk_cnt]` | depth=4096, cqe_size=16 → baseblk_cnt=4096 → 4096×8 | **32768** |
| ⑩ jfc sw_db | `udma_u_db_page` 摊销 | db_page 共享分摊 | ~小 |
| ⑪ jfr 对象 | `udma_u_jfr` | 含 `urma_jfr_t`+`udma_u_jetty_queue`+idx_que | ~200 |
| ⑪ jfr idx_que | `udma_u_alloc_jfr_idx_que` | depth=1 → 极小 | ~小 |
| ⑬ seg | `udma_u_segment` | `urma_register_seg`（本端注册） | 104 |
| jetty 对象 | `udma_u_jetty` | 含 `urma_jetty_t`+`udma_u_jetty_queue` | ~200 |
| jetty sq wrid | `wrid[baseblk_cnt]` | depth=HC_PROBE_DEPTH=1024 → 1024×8 | **8192** |
| ⑫ probe buf | `memalign(page, 4096)` | HC_PROBE_SEG_LEN=getpagesize()=4096 | 4096 |
| **单 pctx 小计** | | | **~45.8 KB** |

**hc 开启场景总堆内存：**

| 申请点 | 计算 | 字节 |
|--------|------|------|
| ⑧ bondp_hc_ctx_t（nodes[1024]+probes[20]） | 1024×4080+1280 | **4,178,560** |
| ⑩ jfc CQ wrid ×4 | 4×32768 | 131072 |
| jetty sq wrid ×4 | 4×8192 | 32768 |
| ⑫ probe buf ×4 | 4×4096 | 16384 |
| ⑨⑩⑪⑬ jetty/jfc/jfr/jfce/seg 对象 ×4 | 4×(~160+200+200+24+104) | ~2752 |
| **hc 子树小计** | | **~4.36 MB** |
| ②③ hash bucket ×2（来自 2.3） | — | 262144 |
| ⑮ fb bucket | — | 8192 |
| ⑦ pctx ×4 | — | 1600 |
| ①⑭ 其余 | — | ~500 |
| **总计** | | **~4.64 MB** |

**占比（hc 开启，总计 ~4.86 MB）：**

| 申请点 | 字节 | 占比 |
|--------|------|------|
| ⑧ bondp_hc_ctx_t（nodes[1024] 预分配） | 4,178,560 | **87.0%** |
| ②③ hash bucket ×2 | 262144 | 5.5% |
| ⑩ jfc CQ wrid ×4 | 131072 | 2.7% |
| jetty sq wrid ×4 | 32768 | 0.7% |
| ⑫ probe buf ×4 | 16384 | 0.3% |
| ⑮ fb bucket | 8192 | 0.17% |
| ⑦ pctx ×4 | 1600 | 0.03% |
| ⑨⑩⑪⑬ 对象 ×4 | 2752 | 0.06% |
| ①⑭ 其余 | 500 | 0.01% |

**hc 子树内部占比（仅看 hc 引入部分 ~4.36 MB）：**

| 申请点 | 字节 | hc 子树内占比 |
|--------|------|---------------|
| ⑧ bondp_hc_ctx_t | 4,178,560 | **95.8%** |
| ⑩ jfc CQ wrid ×4 | 131072 | 3.0% |
| jetty sq wrid ×4 | 32768 | 0.8% |
| ⑫ probe buf ×4 | 16384 | 0.4% |
| 对象 ×4 | 2752 | 0.06% |

---

## 三、bondp_import_jetty 堆内存分析

### 3.1 调用树与申请点

```
bondp_import_jetty                                      [bondp_api.c:2154]
├─ calloc(bondp_target_jetty_t)              ★A 主对象              [L2165]
├─ bondp_fill_bond_id_info_from_rjetty_ext    (仅 has_user_info 分支)
│    └─ 纯解析，无堆分配                                              [L1883]
├─ bondp_import_vjetty                       (else 分支，无 has_user_info) [L2042]
│    └─ urma_cmd_import_jetty → 纯 ioctl 封装，用户态零堆分配        [urma_cmd.c:2160]
├─ bondp_rebuild_connected_by_topo            栈上数组，无堆分配        [L1759]
├─ init_target_active_indices                无堆分配                    [L538]
├─ bondp_import_pjetty                       ★B 关键                    [L2078]
│    └─ 对每个 connected [local_idx][target_idx] 组合：
│        urma_import_jetty(p_ctxs[local_idx], ...)
│          → urma_import_jetty_compat                         [urma_cp_api.c:1976]
│            → ops->import_jetty_ex = udma_u_import_jetty_ex   [udma_u_jetty.c:665]
│              └─ calloc(struct udma_u_target_jetty)   ★C 每路径1个 [L685]
│                 (后续 urma_cmd_import_jetty_ex 为 ioctl，不计)
├─ bondp_hc_import_tseg                      ★D (hc_ctx 存在且健康检查开启时) [bondp_health.c:846]
│    └─ 对每个 p_tjetty[i][j] 非空且 seg.len>0：
│        urma_import_seg(p_ctxs[i], ...)
│          → ops->import_seg = udma_u_import_seg               [udma_u_segment.c:193]
│            └─ calloc(struct udma_u_segment)         ★E 每探测段1个 [L207]
│               (纯用户态实现，无 ioctl)
└─ bondp_hc_register_tjetty                  操作已有 node + 内嵌链表节点，无堆分配 [bondp_health.c:963]
```

**申请点汇总：**

| 编号 | 申请位置 | 对象类型 | 触发条件 | 频次 |
|------|----------|----------|----------|------|
| A | `bondp_import_jetty` 顶部 | `bondp_target_jetty_t` | 无条件 | 每次 import 1 次 |
| C | `udma_u_import_jetty_ex` | `udma_u_target_jetty` | 每个 connected pjetty 路径 | 每 P 路径 1 次 |
| E | `udma_u_import_seg` | `udma_u_segment` | hc 开启且对端有健康探测段 | 每 H 段 1 次 |

> 说明：`bondp_import_vjetty`（虚拟 jetty）走 `urma_cmd_import_jetty`，仅栈上构造 ioctl 参数，用户态零堆分配；`bondp_rebuild_connected_by_topo` 使用栈上 `bool connected[N][N]`；`init_target_active_indices`、`bondp_hc_register_tjetty` 均无堆分配。

### 3.2 结构体大小估算

**A `bondp_target_jetty_t`**（[bondp_types.h:248](src/urma/lib/urma/bond/bondp_types.h#L248)）

| 字段 | 计算 | 字节 |
|------|------|------|
| `p_tjetty[20][20]` 指针矩阵 | 20×20×8 | 3200 |
| `p_check_tseg[20][20]` 指针矩阵 | 20×20×8 | 3200 |
| `v_tjetty` (urma_target_jetty_t) | ~96 | 96 |
| `local_active_indices[20]`+`active_indices[20]` | 2×20×4 | 160 |
| `valid[20]`+`hc_entry`+ 其余 | — | ~150 |
| **合计** | | **~6.6 KB** |

**C `udma_u_target_jetty`**（[udma_u_common.h:249](src/urma/hw/udma/udma_u_common.h#L249)）
`urma_tjetty`(~96B) + `le_eid`(16B) + `token_value`(4B) + bool ≈ **120 B**

**E `udma_u_segment`**（[udma_u_common.h:240](src/urma/hw/udma/udma_u_common.h#L240)）
`urma_tseg`(`urma_target_seg_t` ~72B) + `token_value`(8B) + `len`/`va`(2×8B) + `tid`(4B) + bool ≈ **104 B**

### 3.3 典型场景占比

2×2 场景：本端/对端各 4 个 active port，connected 路径数 P=4，健康探测段数 H=4。

**单次 import 堆内存：**

```
Total ≈ 6600 + 120×P + 104×H = 6600 + 480 + 416 = 7496 字节
```

| 申请点 | 字节 | 占比（hc 开启） | 占比（hc 关闭） |
|--------|------|-----------------|------------------|
| A `bondp_target_jetty_t` | 6600 | **88.1%** | **93.2%** |
| C pjetty ×4 | 480 | 6.4% | 6.8% |
| E hc tseg ×4 | 416 | 5.5% | — |
| **合计** | 7496 | 100% | 7080 (100%) |

**300 并发建链放大：**

| 申请点 | 单次 | ×300 | 其中浪费 |
|--------|------|------|----------|
| A 主对象 | 6.6 KB | **1.93 MB** | ~1.83 MB（固定矩阵浪费） |
| C pjetty | 0.48 KB | 0.14 MB | — |
| E hc tseg | 0.42 KB | 0.12 MB | — |
| **合计** | ~7.3 KB | **~2.19 MB** | — |

---

## 四、核心问题定位

### 4.1 bondp_create_context 默认场景：hash bucket 是主体

hc 关闭时，**两个 hash bucket（②③）占 96.3%**，每个 128 KB。根源：

- `BONDP_MAX_NUM_JETTYS = 10240` / `BONDP_MAX_NUM_RSEGS = 10240` 容量过大
- `ub_hmap_init` 按容量 `calloc` bucket 数组，`calc_mask(10240)→16384` 个桶，每桶 8B
- 2×2 典型场景实际 jetty/rseg 数远少于 10240，**bucket 数组利用率极低**
- 每个固定 128 KB，两个 256 KB，是默认场景 RSS 的主体

### 4.2 bondp_create_context hc 开启场景：hc_ctx 的 nodes[1024] 是巨无霸

hc 开启时，**`bondp_hc_ctx_t` 占 hc 子树 95.8%（4.18 MB），占总堆 87.0%**。根源：

- `bondp_hc_node_t nodes[MAX_NODE_NUM=1024]` **预分配 1024 个 node**
- 每个 `bondp_hc_node_t` 含 `valid[20][20]`+`hc_tjetty[20][20]`+`probe_checked[20][20]` = ~4080 B
- `hc_init_nodes` 实际只用 `node_num` 个（典型 2），**利用率 2/1024 = 0.2%**
- 浪费 1022×4080 ≈ 4.17 MB/context

### 4.3 bondp_import_jetty：target_jetty 矩阵是主体

2×2 场景下，**`bondp_target_jetty_t`（A）占 88%~93%**。根源：

- 两个指针矩阵按 `URMA_UBAGG_DEV_MAX_NUM=20` **固定分配**：
  ```c
  urma_target_jetty_t  *p_tjetty[20][20];       // 3200B
  urma_target_seg_t   *p_check_tseg[20][20];     // 3200B
  ```
- 2×2 场景实际只用 4 个槽位，**利用率 4/400 = 1%**
- 有效载荷仅 4×8=32B，实际固定占用 6400B，**6368B 纯浪费**（占主对象 96.4%）
- 300 并发下该浪费放大至 ~1.83 MB

### 4.4 共性问题：固定分配浪费

两个 API 都存在「按上限常量固定分配、实际利用率极低」的共性问题：

| API | 浪费点 | 预分配维度 | 实际使用 | 利用率 |
|-----|--------|-----------|----------|--------|
| create_context | hash bucket ②③ | 10240 容量 | 几十个 | <1% |
| create_context | hc nodes ⑧ | MAX_NODE_NUM=1024 | 2 | 0.2% |
| create_context | hc node 矩阵 | 20×20 | 4 | 1% |
| import_jetty | target_jetty 矩阵 A | 20×20 | 4 | 1% |

### 4.5 次要申请点

- ⑩ jfc CQ wrid（depth=4096 → 32KB/组）是 hc 子树内第二大项，属按深度配置的可调项
- C pjetty 与 E hc tseg 按**实际路径数线性增长**，路径数少时占比小，语义合理，非优化重点
- ② vjetty 用户态零分配，无需关注

---

## 五、内存优化方案设计

### 方案一：bondp_target_jetty_t 矩阵瘦身（针对 import_jetty，收益最大）

**目标**：把 `[20][20]` 固定矩阵改为按真实拓扑维度 `[IODIE_NUM][IODIE_NUM]`（即 `[2][2]`）按需分配，消除固定浪费。

**设计 A（编译期定维，最简）**：

```c
typedef struct bondp_target_jetty {
    urma_target_jetty_t v_tjetty;
    // ...
    urma_target_jetty_t *p_tjetty[IODIE_NUM][IODIE_NUM];       // 2×2×8 = 32B
    urma_target_seg_t   *p_check_tseg[IODIE_NUM][IODIE_NUM];   // 2×2×8 = 32B
    // ... 其余字段同步把 [URMA_UBAGG_DEV_MAX_NUM] 降为 [IODIE_NUM]
} bondp_target_jetty_t;
```

**设计 B（动态分配，支持可变拓扑）**：

```c
typedef struct bondp_target_jetty {
    urma_target_jetty_t **p_tjetty;      // 按实际 enabled_count 动态 calloc
    urma_target_seg_t   **p_check_tseg;
    uint32_t local_dev_num;
    uint32_t target_dev_num;
} bondp_target_jetty_t;
```

在 `init_target_active_indices` 确定维度后 calloc，import 失败路径 free。

**收益**：

| 项目 | 优化前 | 优化后 | 降幅 |
|------|--------|--------|------|
| `p_tjetty`+`p_check_tseg` | 6400B | 64B | **-99%** |
| 主对象总大小 | ~6.6 KB | ~0.25 KB | **-96%** |
| 单次 import 堆内存 | ~7.3 KB | ~1.0 KB | **-86%** |
| 300 并发主对象占用 | 1.93 MB | ~0.07 MB | **-96%** |

**风险**：① 索引语义变化，需审计所有 `local_idx/target_idx` 取值上界（`bondp_import_pjetty`/`bondp_hc_import_tseg`/`schedule_send`/failback）；② `bondp_target_jetty_t` 是内部私有结构，无对外 ABI 暴露，安全；③ 满配场景需用方案 B。

### 方案二：bondp_hc_ctx_t nodes 按需分配（针对 create_context hc 场景）

**目标**：把 `nodes[MAX_NODE_NUM=1024]` 预分配改为按 `node_num` 动态分配，消除 4.17 MB 浪费。

**设计**：

```c
struct bondp_hc_ctx {
    bondp_hc_cfg_t cfg;
    bondp_probe_res_t *probes;        // 动态 calloc(node_num 或 URMA_UBAGG_DEV_MAX_NUM)
    uint32_t node_num;
    bondp_hc_node_t *nodes;           // 动态 calloc(node_num × sizeof(node))
    // ...
};
```

`hc_init_nodes` 在确认 `node_num` 后 calloc，`hc_destroy_nodes` free。

**收益**：

| 项目 | 优化前 | 优化后（node_num=2） | 降幅 |
|------|--------|---------------------|------|
| hc_ctx nodes | 4.18 MB | ~8 KB | **-99.8%** |
| hc 子树 | 4.36 MB | ~0.19 MB | **-95.7%** |
| create_context（hc 开启） | 4.86 MB | ~0.69 MB | **-85.8%** |

**风险**：① `bondp_hc_node_t` 内的 `[20][20]` 矩阵仍固定（需配合方案三）；② 动态分配后 node 访问从数组下标变指针偏移，需审计所有 `hc_ctx->nodes[i]` 访问点；③ node_num 在运行期可能变化（topo 更新），需支持 realloc 或重建。

### 方案三：bondp_hc_node_t 内部矩阵瘦身（针对 create_context hc 场景）

**目标**：把 `bondp_hc_node_t` 内 `valid[20][20]`/`hc_tjetty[20][20]`/`probe_checked[20][20]` 改为 `[IODIE_NUM][IODIE_NUM]`，消除每 node 6240B 浪费。

**收益**：每 node 从 4080B → ~104B（降 97%），与方案二叠加后 hc_ctx 从 4.18MB → ~0.2MB。

**风险**：与方案一同需审计 idx 上界。

### 方案四：hash bucket 动态扩容（针对 create_context 默认场景）

**目标**：把 ②③ 的 10240 容量预分配改为按需小初始 + 动态扩容，消除 256 KB 浪费。

**设计**：`ub_hmap_init` 初始 bucket 数取较小值（如 64），插入量超阈值时 rehash 扩容。或直接把 `BONDP_MAX_NUM_JETTYS/BONDP_MAX_NUM_RSEGS` 下调为合理值（如 512）。

**收益**：默认场景 ②③ 从 256 KB → ~1 KB（降 99%），create_context 从 272 KB → ~17 KB。

**风险**：① `ub_hmap` 当前不支持动态扩容，需改造底层；② 下调常量可能影响高并发场景 hash 性能；③ bucket 缩小后 hash 冲突增多，lookup 性能下降。

### 方案五：bondp_target_jetty_t 对象池（针对 import_jetty 并发）

**目标**：300 并发建链下消除 malloc arena 锁竞争与重复分配。

**设计**：per-context freelist 缓存已释放的 `bondp_target_jetty_t`，import 时从 freelist 取，unimport 时归还。

**收益**：消除并发 malloc 锁竞争（时延收益）+ 复用内存降低 RSS 峰值。与方案一正交，可叠加。

**风险**：freelist 需线程安全，归还对象需 reset 全部字段。

### 方案六：hc tseg 懒加载（针对 import_jetty hc 场景，次要）

**目标**：`bondp_hc_import_tseg` 的 E 在健康检查未真正启用探测时避免预分配。

**收益**：2×2 场景省 ~0.42KB/import，占比小，非重点。

---

## 六、落地优先级建议

| 优先级 | 方案 | 针对场景 | 收益 | 风险 | 建议动作 |
|--------|------|----------|------|------|----------|
| P0 | 方案一（target_jetty 矩阵瘦身） | import_jetty | 主对象 -96%，单次 -86%，300并发 -1.83MB | 中（需审计 idx 上界） | 先做 idx 上界审计，确认安全后落地 |
| P0 | 方案二（hc nodes 按需分配） | create_context hc 开启 | hc_ctx -99.8%，单次 -4.17MB | 中（node 访问审计） | 与方案三一起做 |
| P0 | 方案三（hc node 矩阵瘦身） | create_context hc 开启 | 每 node -97% | 中（idx 上界） | 配合方案二 |
| P1 | 方案四（hash bucket 动态扩容） | create_context 默认 | ②③ -99%，单次 -255KB | 中高（改 ub_hmap） | 评估 ub_hmap 改造成本 |
| P1 | 方案五（对象池） | import_jetty 并发 | 并发时延 + RSS 峰值 | 中（同步与 reset） | 方案一落地后叠加 |
| P2 | 方案六（hc 懒加载） | import_jetty hc | ~6% | 低 | 视健康检查实际使用评估 |

---

## 七、总结

### 7.1 两个 API 内存总览

| API | 场景 | 单次堆内存 | 主体 | 主体占比 | 浪费量 |
|-----|------|-----------|------|----------|--------|
| `bondp_create_context` | hc 关闭（默认） | ~272 KB | hash bucket ②③ | 96.3% | ~255 KB |
| `bondp_create_context` | hc 开启 | ~4.86 MB | hc_ctx nodes ⑧ | 87.0% | ~4.17 MB |
| `bondp_import_jetty` | 2×2 单次 | ~7.3 KB | target_jetty 矩阵 A | 88%~93% | ~6.4 KB |
| `bondp_import_jetty` | 2×2 ×300 并发 | ~2.19 MB | target_jetty 矩阵 A | 88%~93% | ~1.83 MB |

### 7.2 核心结论

1. **bondp_create_context 默认场景**（hc 关闭）：单次 ~272 KB，两个 hash bucket 占 96.3%（256 KB）。根因是 `BONDP_MAX_NUM_JETTYS/BONDP_MAX_NUM_RSEGS=10240` 导致 bucket 数组过大。

2. **bondp_create_context hc 开启场景**：单次暴涨至 ~4.86 MB，`bondp_hc_ctx_t` 的 `nodes[1024]` 占 87.0%（4.18 MB）。根因是 `MAX_NODE_NUM=1024` 预分配 + 每 node 含 `[20][20]` 固定矩阵。

3. **bondp_import_jetty**：2×2 单次 ~7.3 KB，`bondp_target_jetty_t` 的 `[20][20]` 矩阵占 88%~93%；300 并发放大至 ~2.19 MB，浪费 ~1.83 MB。

4. **共性根因**：两个 API 都存在「按上限常量（10240/1024/20）固定分配、实际利用率 <1%」的浪费，是 RSS 增长的主因。

5. **最高性价比优化**：
   - import_jetty → 方案一（矩阵瘦身）：单次 -86%，300 并发 -1.83 MB
   - create_context hc → 方案二+三（nodes 按需 + node 矩阵瘦身）：单次 -4.17 MB
   - create_context 默认 → 方案四（bucket 动态扩容）：单次 -255 KB

6. **参数配置建议**：生产环境若不使用健康检查，保持 `BOND_ENABLE_HEALTH_CHECK=false`（默认）可避免 4.4 MB/context 的 hc 开销；若使用，建议配合方案二/三优化后再开启。
