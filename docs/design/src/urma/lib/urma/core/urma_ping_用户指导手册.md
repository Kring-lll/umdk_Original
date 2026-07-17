# urma_ping ↔ ubmgr_ping 用户指导手册

> 走读范围：用户态 `00_umdk/umdk/src/urma/tools/urma_ping/`，内核态 `01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c`
> 文档版本：v1.0（基于源码 2026-02-03 / 2026-06-18 版本走读生成）
> 适用组件：UMDK（灵衢内存语义开发包）/ 内核 UB-URMA（ubcore + ubagg + ubmgr）

---

## 0. 一句话结论（先读这段）

`urma_ping`（用户态工具）和 `ubmgr_ping`（内核 ubcore 内置客户端）**之间没有直接 netlink 通道**。它们的"对接"发生在 **URMA 数据面**，而不是控制面：

- **控制面（netlink）**：用户态 `urma_ping` 通过 generic netlink family `UBAGG_GENL` 与内核 **`ubagg` 模块**（不是 ubmgr！）通信，唯一用途是**查询聚合拓扑（topo）**，把用户输入的 bonding EID 解析成 primary EID + chip_id。这是一条"地址传递"风格的单次请求-响应 RPC。
- **数据面（URMA 原语）**：用户态 `urma_ping` 作为 **client**，调用 `liburma` 的 URMA API（`urma_create_jetty` / `urma_post_jetty_send_wr` / `urma_wait_jfc` 等）发起一次 `SEND_IMM`；内核 `ubmgr_ping` 作为 **server/echo 端**，注册了固定 jetty（`PING_WK_JETTY_ID = 5`），收到请求后原样回发 `SEND_IMM`，把请求里携带的 `imm_data`（序列号 seq）回传。client 用 seq 匹配、计 RTT、出统计。

**所以"对接契约"有三处**：① netlink topo 协议（用户态 ↔ ubagg）；② jetty_id = 5（用户态 ↔ 内核 server）；③ `SEND_IMM + imm_data=seq` 的报文语义（双方一致）。这三点在两侧源码里完全对齐（见 §3）。

---

## 1. 功能定位与组件关系

### 1.1 urma_ping 是什么

`urma_ping` 是 UMDK `urma` 工具集中的一个**端到端连通性与时延测量工具**，类比传统 `ping`，但跑在 URMA（统一远程内存访问）语义上而非 ICMP 上。它用来：

- 验证两个 UB 节点之间 URMA 数据面是否打通（EID 可达、jetty 可建链、报文可收发）。
- 测量端到端 RTT（min/avg/max/mdev）与丢包率。

源码目录：`00_umdk/umdk/src/urma/tools/urma_ping/`，共 11 个文件，约 1451 行。

| 文件 | 行数 | 职责 |
|---|---|---|
| [ping.c](00_umdk/umdk/src/urma/tools/urma_ping/ping.c) | 53 | `main` 入口、默认参数、调用 parse→check→start_ping→print_stat |
| [ping_parameters.c](00_umdk/umdk/src/urma/tools/urma_ping/ping_parameters.c) / [.h](00_umdk/umdk/src/urma/tools/urma_ping/ping_parameters.h) | 204/42 | getopt_long 参数解析、EID 字符串解析、usage/help/version |
| [ping_run.c](00_umdk/umdk/src/urma/tools/urma_ping/ping_run.c) / [.h](00_umdk/umdk/src/urma/tools/urma_ping/ping_run.h) | 732/55 | **核心**：URMA 资源建链、EID 解析、ping 主循环、收发 |
| [ping_netlink.c](00_umdk/umdk/src/urma/tools/urma_ping/ping_netlink.c) / [.h](00_umdk/umdk/src/urma/tools/urma_ping/ping_netlink.h) | 177/18 | generic netlink 封装，仅实现 `ping_get_topo_info()` |
| [ping_stat.c](00_umdk/umdk/src/urma/tools/urma_ping/ping_stat.c) / [.h](00_umdk/umdk/src/urma/tools/urma_ping/ping_stat.h) | 84/20 | RTT 统计（Welford 方差）、`get_time_in_ms` |
| [ping_log.c](00_umdk/umdk/src/urma/tools/urma_ping/ping_log.c) / [.h](00_umdk/umdk/src/urma/tools/urma_ping/ping_log.h) | 33/33 | 分级日志（quiet/normal/verbose/vverbose） |
| [CMakeLists.txt](00_umdk/umdk/src/urma/tools/urma_ping/CMakeLists.txt) | 33 | 构建目标 `urma_ping`，链接 `urma` + `nl-genl-3/nl-3` + `m` |

### 1.2 ubmgr_ping 是什么

`ubmgr_ping` 是内核 `ubcore` 驱动里的一个**内置 UB client**，名字叫 `"ubmgr_ping"`（[ubmgr_ping.c:744](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L744)）。它**不是**一个独立的 `.ko`，而是静态编进 `ubcore.ko`（见 §6）。它充当 urma_ping 的**内核侧 echo server**：在主 UE EID 就绪时自动创建一个 jetty（id=5），循环接收 `SEND_IMM` 并立即回发，不参与 netlink。

源码：`01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c`（863 行）+ `ubmgr_ping.h`（17 行，仅 `ubmgr_ping_init/uninit` 原型）。入口在 [ubmgr.c:21](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr.c#L21) 的 `ubmgr_init()` 里调用 `ubmgr_ping_init()`。

### 1.3 三方组件关系图

```
            ┌────────────────── 用户态 ──────────────────┐
            │                                            │
            │   urma_ping (client)                      │
            │   ├─ ping_parameters.c  (getopt 解析)      │
            │   ├─ ping_netlink.c    (genl topo 查询) ───┼──┐
            │   ├─ ping_run.c        (urma API 收发) ────┼──┼──┐
            │   └─ ping_stat.c      (RTT 统计)          │  │  │
            └────────────────────────────────────────────┘  │  │
                         │ liburma.so                       │  │
                         │ (urma_* 用户态库)                 │  │
            ═════════════╪══════════════════════════════════╪══╪═══ 系统调用 / 驱动接口
            ┌────────────┴─────────────────────────────────┴──┴──┐
            │  内核态 (drivers/ub/urma/)                              │
            │                                                        │
            │  ┌─────────────┐  genl family    ┌──────────────────┐  │
            │  │ ubagg 模块  │ ◀═══(NLA_U64)══ │ (topo 控制面)    │  │  ← netlink 通道
            │  │ UBAGG_GENL  │     GET_TOPO    │ 仅查 topo，不ping│  │    对接点 ①
            │  └─────────────┘                └──────────────────┘  │
            │                                                        │
            │  ┌─────────────────────────────────┐  ubcore 原语     │
            │  │ ubcore (含 ubmgr_ping)          │ ◀═══════════════┘  │
            │  │  ubmgr_ping = "ubmgr_ping" client │                  │
            │  │  server jetty id = 5 (PING_WK_JETTY_ID)│  ← 数据面  │
            │  │  echo: SEND_IMM in → SEND_IMM out (imm=seq)│ 对接点②③│
            │  └─────────────────────────────────┘                  │
            └────────────────────────────────────────────────────────┘
                              │ UB 硬件 (ubus/udma/ubfi/ummu...)
                              ▼
                          远端节点的 urma_ping / ubmgr_ping
```

**关键澄清**：很多人会想当然以为"用户态 urma_ping 通过 netlink 命令触发内核 ubmgr_ping 去 ping"。**不是这样**。netlink 只查 topo（对接 ubagg）；真正的 ping 报文走 URMA 数据面（对接 ubcore/ubmgr_ping 的 jetty）。这两条通道独立存在，详见 §3。

---

## 2. 顶层主流程

### 2.1 用户态 main → start_ping 调用链

[ping.c:22](00_umdk/umdk/src/urma/tools/urma_ping/ping.c#L22) `main()`：

```
main()
 ├─ 构造 ping_cfg_t，填默认值（count=UINT32_MAX, interval=1, size=4, timeout=1, deadline=0, verbose_level=NORMAL）
 ├─ parse_args()      [ping_parameters.c:115]  getopt_long 解析，dst_eid 从 argv[optind] 取
 ├─ check_args()      [ping_parameters.c:186]  校验 count>0、0<size<=4096
 ├─ start_ping()      [ping_run.c:671]          主体
 │    ├─ init_urma_resource()   [ping_run.c:291]
 │    │    ├─ urma_register_log_func / urma_init
 │    │    ├─ get_src_and_dst_eid()  ← 这里调 ping_get_topo_info() 走 netlink 查 topo
 │    │    ├─ urma_get_device_by_eid / urma_query_device
 │    │    ├─ urma_create_context
 │    │    ├─ memalign(size) + urma_register_seg   (本地内存段)
 │    │    ├─ urma_create_jfc(send) / urma_create_jfce / urma_create_jfc(recv) / urma_rearm_jfc
 │    │    ├─ urma_create_jfr           (接收端，RM 模式)
 │    │    ├─ urma_create_jetty         (本地 jetty，share_jfr=1)
 │    │    └─ urma_import_jetty(tjetty) (远端 jetty id=5, eid=dst_eid)
 │    ├─ fill_recv_wr()            预投递 1 个接收 WR
 │    ├─ init_stat() + signal(SIGINT)
 │    └─ for i in [0, count):
 │         ├─ send_ping_msg()   [ping_run.c:528]  post SEND_IMM(imm_data=seq)
 │         ├─ recv_ping_msg()   [ping_run.c:613]  wait_for_recv_jfc → poll → 比对 imm_data==seq
 │         ├─ sleep(interval)
 │         └─ 检查 deadline
 ├─ print_stat()      [ping_stat.c:44]   输出 transmitted/received/loss/rtt
 └─ return
```

### 2.2 内核 ubmgr_ping 生命周期

`ubcore.ko` 加载时（`ubcore_init` → `ubmgr_init` → [ubmgr_ping_init()](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L836)）：

```
ubmgr_ping_init()
 ├─ ubcore_register_main_ue_eid_event_cb(ping_on_main_ue_eid_event)
 └─ ubcore_register_client(&g_ping_client)   // client_name="ubmgr_ping", .add/.remove 回调
```

之后由 **ubcore 事件驱动**，而非用户态命令驱动：

- 当某 `ubcore_device` 上线（`add`），`ping_on_add_device` 分配 `ubmgr_ping_ctx` + 收发 buf + workqueue，存到 `dev` 的 client ctx。
- 当该设备的 **main UE EID 首次添加**（`UBCORE_MAIN_UE_EID_FIRST_ADD` 事件），`ping_try_init_ctx` → `ping_ctx_init_jetty`：注册 seg、建 send/recv jfc、建 jfr、**建 jetty（id=5）**、prefill 1024 个 recv WR。此刻 server 开始接收。
- 当 main UE EID 最后一个删除（`LAST_DEL`），`ping_try_uninit_ctx` 拆 jetty/jfr/jfc/seg（保留 ctx 和 wq，等下次 FIRST_ADD 重建）。
- 当 `device` 移除，`ping_on_remove_device` 清空 tjetty hash、拆 jetty、销毁 wq、释放 ctx。

> 设计要点：ubmgr_ping 是**被动 echo**，自己不主动发起 ping。client 端是用户态 urma_ping。这解释了为什么内核侧没有"发起 ping"的入口函数，只有"收→回"的处理路径。

---

## 3. 细化通信方案设计（重点）

通信分**两条独立通道**，下面分别细化。

### 3.1 控制面：netlink topo 查询通道（用户态 ↔ ubagg）

#### 3.1.1 family 与协议常量

| 项 | 用户态（ping_netlink.c） | 内核态（ubagg_netlink.h/.c） | 一致性 |
|---|---|---|---|
| family 名 | `#define UBAGG_GENL_FAMILY_NAME "UBAGG_GENL"` [:22](00_umdk/umdk/src/urma/tools/urma_ping/ping_netlink.c#L22) | `#define UBAGG_GENL_FAMILY_NAME "UBAGG_GENL"` [ubagg_netlink.h:14](01_kernel/drivers/ub/urma/ubagg/ubagg_netlink.h#L14) | ✅ 一致 |
| family version | `GENL_FAMILY_VERSION 1` [:23](00_umdk/umdk/src/urma/tools/urma_ping/ping_netlink.c#L23) | `UBAGG_GENL_FAMILY_VERSION 1` [ubagg_netlink.h:15](01_kernel/drivers/ub/urma/ubagg/ubagg_netlink.h#L15) | ✅ |
| cmd 枚举 | `UBAGG_NL_CMD_UNSPEC, UBAGG_NL_GET_TOPO` [:29-32](00_umdk/umdk/src/urma/tools/urma_ping/ping_netlink.c#L29) | `UBAGG_NL_CMD_GET_TOPO = 1` [ubagg_netlink.h:28](01_kernel/drivers/ub/urma/ubagg/ubagg_netlink.h#L28) | ✅（用户态只用 GET_TOPO） |
| attr 枚举 | `UBAGG_HDR_ARGS_ADDR = 4` [:25-27](00_umdk/umdk/src/urma/tools/urma_ping/ping_netlink.c#L25) | `UBAGG_HDR_ARGS_ADDR = 4` [ubagg_netlink.h:19](01_kernel/drivers/ub/urma/ubagg/ubagg_netlink.h#L19)，policy `NLA_U64` [ubagg_netlink.c:32](01_kernel/drivers/ub/urma/ubagg/ubagg_netlink.c#L32) | ✅ |

> 用户态把 `UBAGG_HDR_ARGS_ADDR` 的"值"取成 `NLA_U64`，但语义上它是一个**用户态结构体缓冲区的指针**（`(uint64_t)(uintptr_t)arg`，[ping_netlink.c:148](00_umdk/umdk/src/urma/tools/urma_ping/ping_netlink.c#L148)）。内核 `ubagg_nl_get_topo_ops` 收到后，通过 `ubagg_nl_parse_attr` 取出这个 u64 当作用户态地址，**直接把 topo 信息回填到该地址**（copy_to_user 风格的同步回写）。这是一种非常规但高效的"地址传递 RPC"。

#### 3.1.2 消息结构契约（双方共享的内存布局）

用户态 [ping_netlink.c:34-42](00_umdk/umdk/src/urma/tools/urma_ping/ping_netlink.c#L34)：

```c
typedef struct ping_core_cmd_topo_info {
    struct { int node_idx; } in;            // 入参：要查第几个 node
    struct {
        uint32_t node_num;                 // 出参：总 node 数
        struct urma_ping_ubcore_topo_node topo_info; // 出参：该 node 的拓扑
    } out;
} ping_core_cmd_topo_info_t;
```

内核侧对应结构 `struct ubagg_nl_topo_info`（[ubagg_netlink.c:40-47](01_kernel/drivers/ub/urma/ubagg/ubagg_netlink.c#L40)）内存布局**完全一致**（`in{node_idx} + out{node_num, topo_info}`）。这就是双方对接的"隐式 ABI"——**没有头文件共享，靠内存布局对齐**，改动任一侧结构必须同步另一侧。

`urma_ping_ubcore_topo_node`（用户态，[ping_run.h:39-46](00_umdk/umdk/src/urma/tools/urma_ping/ping_run.h#L39)）字段：

```c
struct urma_ping_ubcore_topo_node {
    uint32_t type; super_node_id; node_id; is_current;
    bool links[IODIE_NUM*PORT_NUM][IODIE_NUM*PORT_NUM];   // 2*9=18 → 18x18
    struct urma_ping_ubcore_topo_agg_dev agg_devs[DEV_NUM]; // 256
};
```

> 注意常量：`IODIE_NUM=2`、`PORT_NUM=9`、`DEV_NUM=256`、`MAX_NODE_NUM=1024`（[ping_run.h:16-23](00_umdk/umdk/src/urma/tools/urma_ping/ping_run.h#L16)）。这些**必须与内核 ubagg 的 `ubagg_topo_node` 一致**，否则回填越界。这是接缝处的脆弱点，运维升级需关注。

#### 3.1.3 netlink 收发流程

用户态 `ping_get_topo_info()`（[ping_netlink.c:124](00_umdk/umdk/src/urma/tools/urma_ping/ping_netlink.c#L124)）：

1. `ping_nl_init()`：`nl_socket_alloc` → `genl_connect` → `genl_ctrl_resolve(sock, "UBAGG_GENL")` 拿到 family id。
2. 循环 `for i in [0, MAX_NODE_NUM=1024)`：
   - `calloc` 一个 `ping_core_cmd_topo_info_t`，`arg->in.node_idx = i`。
   - `ping_nl_alloc_msg(UBAGG_NL_GET_TOPO, 0)`：`nlmsg_alloc` + `genlmsg_put(family_id, version=1, cmd=GET_TOPO)`。
   - `nla_put_u64(msg, UBAGG_HDR_ARGS_ADDR, (uint64_t)arg)`：把 arg 缓冲区地址作为 attr 塞进去。
   - `ping_nl_send_recv_msg_default(msg)`：`nl_send_auto` + `nl_recvmsgs_default`（阻塞等内核回包）。
   - 内核处理完后 `arg->out.node_num` / `arg->out.topo_info` 已被回填，拷贝到 `topo_map`。
   - 用首次返回的 `node_num` 收敛循环上界（避免查满 1024 次）。

#### 3.1.4 topo 查询用来干什么（ping_run.c 里的 EID 解析）

urama_ping 允许用户传 **bonding EID**（聚合口 EID）而非 primary EID。bonding 是 ubagg 提供的多路径聚合，一个 agg_eid 对应底层多个 UE 的 primary_eid。解析链（[ping_run.c:123-199](00_umdk/umdk/src/urma/tools/urma_ping/ping_run.c#L123)）：

- `get_primary_eid_and_chip_id_by_bonding()`：在 topo 里找 `agg_eid == dst_eid`，取其 `ues[0].primary_eid` 与 `chip_id`。
- `primary_eid_to_main_primary_eid()`：在同 entity、同 chip_id 的多个 primary_eid 里选**最大值**作为"主 primary_eid"（约定主用口）。
- `get_source_eid_by_bonding_eid_and_chip_id()`：对源端同样解析，按 chip_id 选对应的 primary_eid。

> 结论：netlink topo 通道的**唯一作用是 EID 选路**——把用户给的 bonding EID 映射成 URMA 数据面真正要用的 primary EID（带 chip_id 维度选路径）。没有这条通道，用户就必须自己手填 primary EID。这也就是为什么 `-I`（指定源 EID）可选：不指定时靠 topo + bonding device 自动选。

### 3.2 数据面：URMA 原语 echo 通道（用户态 liburma ↔ 内核 ubcore/ubmgr_ping）

#### 3.2.1 对接契约三要素

| 契约 | 用户态值 | 内核态值 | 出处 |
|---|---|---|---|
| **远端 jetty id** | `PING_WK_JETTY_ID = 5` | `PING_WK_JETTY_ID = 5` | [ping_run.c:29](00_umdk/umdk/src/urma/tools/urma_ping/ping_run.c#L29) / [ubmgr_ping.c:24](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L24) |
| **opcode** | `URMA_OPC_SEND_IMM` | `UBCORE_OPC_SEND_IMM` | [ping_run.c:539](00_umdk/umdk/src/urma/tools/urma_ping/ping_run.c#L539) / [ubmgr_ping.c:314](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L314) |
| **seq 载体** | `wr.send.imm_data = seq_info->seq` | 回发 `wr.send.imm_data = cr->imm_data`（原样） | [ping_run.c:547](00_umdk/umdk/src/urma/tools/urma_ping/ping_run.c#L547) / [ubmgr_ping.c:320](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L320) |
| 收发深度 | SEND/RECV depth = 1024 | SEND/RECV depth = 1024 | [:30-31](00_umdk/umdk/src/urma/tools/urma_ping/ping_run.c#L30) / [:25-26](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L25) |
| 传输模式 | `URMA_TM_RM`（jetty/jfr） | `UBCORE_TP_RM` | [ping_run.c:418,433](00_umdk/umdk/src/urma/tools/urma_ping/ping_run.c#L418) / [ubmgr_ping.c:569,585](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L569) |
| tp_type | `URMA_CTP` | tjetty cfg `UBCORE_CTP` | [ping_run.c:454](00_umdk/umdk/src/urma/tools/urma_ping/ping_run.c#L454) / [ubmgr_ping.c:72](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L72) |
| 报文上限 | size ≤ 4096（`MAX_PING_SIZE`） | `PING_MAX_MSG_SIZE = 4096` | [ping_parameters.c:23,198](00_umdk/umdk/src/urma/tools/urma_ping/ping_parameters.c#L23) / [ubmgr_ping.c:27](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L27) |

两侧常量完全对齐是设计有意为之——client 的 `urma_import_jetty(rjetty_id=5)` 必须命中内核 server 创建的 jetty(id=5)，否则建链失败。

#### 3.2.2 一次 ping 的完整时序

**用户态 client 侧**（[ping_run.c:528-662](00_umdk/umdk/src/urma/tools/urma_ping/ping_run.c#L528)）：

```
1. send_ping_msg()
   ├─ update_stat_on_send()              // transmitted++
   ├─ seq_info->start_time = get_time_in_ms()  ← 计时起点
   ├─ sge = {addr=buf, len=size, tseg=seg}
   ├─ wr = {opcode=SEND_IMM, tjetty=res->tjetty, send.src.sge=&sge, imm_data=seq}
   └─ urma_post_jetty_send_wr(jetty, &wr)  ← 报文发出，走 URMA→ubcore→硬件→远端

2. recv_ping_msg()  循环
   ├─ wait_timeout_ms = get_wait_timeout_ms()  // timeout=0 则永久等
   ├─ wait_for_recv_jfc(res, timeout, &cr)
   │    ├─ urma_wait_jfc(recv_jfce, 1, timeout, &event_jfc)  ← 阻塞等事件
   │    ├─ urma_poll_jfc(recv_jfc, 1, &cr)                   ← 拉 CQE
   │    ├─ urma_ack_jfc(...) + urma_rearm_jfc(recv_jfc,false) ← 应答+重武装
   │    └─ 返回 cr（含 imm_data / status / completion_len）
   ├─ fill_recv_wr()                     ← 补一个新 recv WR（保持接收水位）
   ├─ if cr.imm_data != seq: 报 "Unmatched reply"，continue
   ├─ seq_info->end_time = get_time_in_ms()
   ├─ rtt = end_time - start_time
   ├─ if cr.status==SUCCESS: update_stat_on_recv(rtt) + 打印 "N bytes from EID: seq=X time=Y ms"
   └─ 超时（ret==0）: 打印 "Request timeout for seq=X"，rtt 仍记录但不计入 received
```

**内核 server 侧**（[ubmgr_ping.c:273-361](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L273)）：

```
硬件收到 SEND_IMM → 产生 CQE 到 recv_jfc → 触发回调 ping_recv_jfc_comp()
  └─ ping_wq_queue_work(jfc, ping_recv_work_handler)   // 入 workqueue
       └─ ping_recv_work_handler()
            └─ do { ubcore_poll_jfc(jfc,1,&cr); ping_wq_on_recved(ctx,&cr); } while(cr_cnt>0)
                 └─ ping_wq_on_recved(ctx, cr)
                      ├─ ping_tjetty_find_or_create(ctx, &cr->remote_id.eid, cr->remote_id.id)
                      │    // 按 对端 eid+jetty_id hash，找/建 target_jetty（发给谁）
                      ├─ 构造 resp_ctx{entry, sge_addr=cr->user_ctx}
                      ├─ sge = {addr=cr->user_ctx, len=min(comp_len,4096), tseg=ctx->seg}
                      ├─ wr = {opcode=SEND_IMM, tjetty=entry->tjetty, imm_data=cr->imm_data}
                      │    // ↑ imm_data 原样回传（seq 透传）
                      ├─ ubcore_post_jetty_send_wr(ctx->jetty, &wr)   ← echo 回发
                      └─ (发送完成回调 ping_send_work_handler→ping_wq_on_sended)
                            └─ ping_tjetty_put(entry) + ping_refill_recv_wr(sge_addr) + kfree(resp_ctx)
```

#### 3.2.3 RTT 计时与统计

- 计时用 `CLOCK_MONOTONIC`（[ping_stat.c:37](00_umdk/umdk/src/urma/tools/urma_ping/ping_stat.c#L37)），毫秒精度（`tv_sec*1000 + tv_nsec/1e6`）。
- RTT = `recv 时刻 - send 时刻`，单位 ms，**仅计入 `cr.status==SUCCESS` 的**。
- 统计字段：`transmitted / received / rtt_min / rtt_max / rtt_avg / rtt_m2 / start_time / end_time`。
- 方差用 **Welford 在线算法**（[ping_stat.c:79-83](00_umdk/umdk/src/urma/tools/urma_ping/ping_stat.c#L79)）：`rtt_m2 += (rtt-old_avg)*(rtt-new_avg)`，`mdev = sqrt(rtt_m2/received)`。避免大 count 下数值溢出。
- 丢包率 = `(transmitted - received)*100 / transmitted`。
- `SIGINT`（Ctrl+C）触发 `signal_handler` → `print_stat()` 后退出（[ping_run.c:664](00_umdk/umdk/src/urma/tools/urma_ping/ping_run.c#L664)）。

### 3.3 两条通道的协同（端到端时序）

```
用户执行:  urma_ping <bonding_eid>
  │
  ├─[1] 解析参数 (ping_parameters.c)
  │
  ├─[2] init_urma_resource
  │    ├─ get_src_and_dst_eid ─► ping_get_topo_info ─► [netlink] ubagg GET_TOPO
  │    │                                                       │
  │    │                                                       └─ ubagg 回填 topo，用户态据此
  │    │                                                          把 bonding EID 解析成 primary EID
  │    │                                                          + chip_id（控制面完成使命）
  │    ├─ urma_create_context / register_seg / create_jfc / create_jfr / create_jetty
  │    └─ urma_import_jetty(tjetty_id=5, eid=dst primary_eid)  ◄── 对接契约②③
  │
  ├─[3] for each seq in [1..count]:
  │       send SEND_IMM(imm=seq)  ──► URMA 数据面 ──► 内核 ubmgr_ping jetty(5)
  │                                                          │ echo: SEND_IMM(imm=seq)
  │       ◄────────────────── recv jfc CQE (imm=seq) ───────┘
  │       比对 imm==seq → 计 RTT → 更新统计
  │
  └─[4] print_stat() → 退出
```

---

## 4. 用户态实现走读（urma_ping）

### 4.1 命令行参数全集

参数解析用 **`getopt_long`**（[ping_parameters.c:117-125](00_umdk/umdk/src/urma/tools/urma_ping/ping_parameters.c#L117)），短选项串 `":c:I:i:s:qw:W:ShVv"`，长选项仅 `--verbose/--help/--version`。

| 选项 | 参数 | 含义 | 默认值 | 约束 | 出处 |
|---|---|---|---|---|---|
| `<destination>` | 位置参数 | 目的 EID：primary EID 或 bonding EID | 必填 | IPv6 / IPv4 / 整数，见 §4.2 | [ping_parameters.c:178](00_umdk/umdk/src/urma/tools/urma_ping/ping_parameters.c#L178) |
| `-c` | count | 发包数量 | `UINT32_MAX`（无限） | >0 | [:127](00_umdk/umdk/src/urma/tools/urma_ping/ping_parameters.c#L127) |
| `-I` | eid | 本地源 primary EID | 不指定=自动选 | 同 dst 格式 | [:130](00_umdk/umdk/src/urma/tools/urma_ping/ping_parameters.c#L130) |
| `-i` | interval | 包间隔（秒） | 1 | u32 | [:134](00_umdk/umdk/src/urma/tools/urma_ping/ping_parameters.c#L134) |
| `-s` | size | 单包 payload 字节 | 4 | 0 < size ≤ 4096 | [:138](00_umdk/umdk/src/urma/tools/urma_ping/ping_parameters.c#L138),[:198](00_umdk/umdk/src/urma/tools/urma_ping/ping_parameters.c#L198) |
| `-w` | deadline | 总执行时限（秒），0=不限 | 0 | u32 | [:141](00_umdk/umdk/src/urma/tools/urma_ping/ping_parameters.c#L141) |
| `-W` | timeout | 单包等待超时（秒），0=永久等 | 1 | u32 | [:144](00_umdk/umdk/src/urma/tools/urma_ping/ping_parameters.c#L144) |
| `-q` | — | 安静模式（只出统计） | — | — | [:146](00_umdk/umdk/src/urma/tools/urma_ping/ping_parameters.c#L146) |
| `-v` | — | 详细等级 +1（可叠加 -vv） | NORMAL(1) | — | [:149](00_umdk/umdk/src/urma/tools/urma_ping/ping_parameters.c#L149) |
| `--verbose` | — | 同 -v | — | — | [:118](00_umdk/umdk/src/urma/tools/urma_ping/ping_parameters.c#L118) |
| `-h` / `--help` | — | 帮助并退出 | — | — | [:152](00_umdk/umdk/src/urma/tools/urma_ping/ping_parameters.c#L152) |
| `-V` / `--version` | — | 版本并退出 | — | — | [:155](00_umdk/umdk/src/urma/tools/urma_ping/ping_parameters.c#L155) |
| `-S` | — | optstring 里有 `S` 但 **无 case 分支** → 走 default 报错 | — | 未实现 | [:125](00_umdk/umdk/src/urma/tools/urma_ping/ping_parameters.c#L125) |

> 日志等级枚举（[ping_log.h:17-22](00_umdk/umdk/src/urma/tools/urma_ping/ping_log.h#L17)）：`QUIET=0 / NORMAL=1 / VERBOSE=2 / VVERBOSE=3`。`-q` 直接设 QUIET；`-v` 每次 +1；`urma_log_func` 把 liburma 内部日志接到 `VVERBOSE`。

### 4.2 EID 字符串解析

`str_to_eid()`（[ping_parameters.c:57-89](00_umdk/umdk/src/urma/tools/urma_ping/ping_parameters.c#L57)）按顺序尝试：

1. **IPv6**：`inet_pton(AF_INET6, buf, eid)`，16 字节直填。
2. **IPv4 点分**：`inet_pton(AF_INET, ...)` → `ipv4_map_to_eid`，填成 IPv4-mapped IPv6（`::ffff:a.b.c.d`，前缀 `0x0000ffff`）。
3. **IPv4 数值/十六进制**：`str_to_u32`（支持 `0x...` / 十进制）→ 同上映射。
4. 全 0 EID 判非法。

> 这意味着用户可以用 `fe80::1`、`192.168.1.1`、`0xc0a80101`、`3232235777` 等多种形式传 EID。

### 4.3 ping_cfg_t 配置结构

[ping_parameters.h:19-32](00_umdk/umdk/src/urma/tools/urma_ping/ping_parameters.h#L19)：

```c
typedef struct ping_cfg {
    int argc; char **argv; char *filename;  // getopt 上下文
    urma_eid_t dst_eid;                     // 目的 EID（已解析）
    urma_eid_t src_eid; bool has_src_eid;   // 源 EID（可选）
    uint32_t count;     // UINT32_MAX = 无限
    uint32_t interval;  // 秒
    uint32_t size;      // 字节
    uint32_t deadline;  // 秒，0=不限
    uint32_t timeout;   // 秒，单包超时
    uint32_t verbose_level;
} ping_cfg_t;
```

### 4.4 URMA 资源拓扑（client 侧）

`init_urma_resource`（[ping_run.c:291-486](00_umdk/umdk/src/urma/tools/urma_ping/ping_run.c#L291)）建立的资源树：

```
ctx = urma_create_context(dev, src_eid_idx)
 ├─ buf = memalign(pagesize, size)        // 本地 ping buffer
 ├─ seg = urma_register_seg(ctx, {va=buf, len=size, access=LOCAL_ONLY})
 ├─ send_jfc = urma_create_jfc(ctx, {depth=1024})
 ├─ recv_jfce = urma_create_jfce(ctx)       // 事件 fd 句柄
 ├─ recv_jfc = urma_create_jfc(ctx, {depth=1024, jfce=recv_jfce})
 ├─ jfr = urma_create_jfr(ctx, {depth=1024, trans_mode=RM, max_sge=1, jfc=recv_jfc})
 ├─ jetty = urma_create_jetty(ctx, {
 │        share_jfr=1,
 │        jfs_cfg={depth=1024, trans_mode=RM, priority=6, max_sge=1,
 │                 rnr_retry=TYPICAL, err_timeout=TYPICAL, jfc=send_jfc},
 │        shared.jfr=jfr})
 └─ tjetty = urma_import_jetty(ctx, {
          jetty_id.eid=dst_eid, jetty_id.id=5,   // ← 对接内核 server 的 jetty(5)
          trans_mode=RM, type=JETTY, tp_type=CTP}, &token)
```

收发模型：**RM（Reliable Message）+ jetty** 共享 jfr。发送用 `jfs_wr`(SEND_IMM)，接收用 `jfr_wr`。`recv_jfce` 是事件句柄，`urma_wait_jfc` 阻塞等它，`urma_poll_jfc(recv_jfc)` 拉 CQE。

---

## 5. 内核态实现走读（ubmgr_ping）

### 5.1 关键数据结构

**`ubmgr_ping_ctx`**（[ubmgr_ping.c:34-47](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L34)）——每设备上下文：

```c
struct ubmgr_ping_ctx {
    void *buf;                       // recv buffer，PING_BUF_SIZE = 1024*4096 = 4MB
    struct mutex init_mutex;         // 保护 jetty 懒初始化
    struct ubcore_target_seg *seg;
    struct ubcore_jfc *send_jfc, *recv_jfc;
    struct ubcore_jfr *jfr;
    struct ubcore_jetty *jetty;      // id=5
    struct workqueue_struct *wq;     // "ping_wq"，UNBOUND|MEM_RECLAIM|FREEZABLE
    struct hlist_head tjetty_hlist[1024];  // 对端 tjetty 缓存
    spinlock_t tjetty_lock, wq_lock;
    bool wq_stopped;                 // jetty 拆除期间阻止 queue_work
};
```

**`ubmgr_ping_tjetty_entry`**（[:50-54](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L50)）：缓存已 import 的远端 tjetty，按 `dst_eid` 的 jhash 分桶，`kref` 引用计数，避免对同一对端重复 import。

**`ubmgr_ping_resp_ctx`**（[:247-250](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L247)）：echo 发送期的临时上下文，记 `entry`(tjetty) + `sge_addr`（收到时的 recv buf 地址，echo 完要 refill）。

**`ubmgr_ping_work`**（[:241-245](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L241)）：workqueue 项，载 `jfc` + `ctx`。

### 5.2 client 注册与事件回调

[ubmgr_ping.c:742-748](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L742)：

```c
struct ubcore_client g_ping_client = {
    .list_node = LIST_HEAD_INIT(...),
    .client_name = "ubmgr_ping",
    .add    = ping_on_add_device,     // dev 上线：分配 ctx+buf+wq
    .remove = ping_on_remove_device,  // dev 下线：拆 wq、jetty、释放
    .stop   = NULL,
};
```

另有 `ubcore_register_main_ue_eid_event_cb(ping_on_main_ue_eid_event)`（[:840](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L840)），监听 main UE EID 的 `FIRST_ADD` / `LAST_DEL`：

- `FIRST_ADD` → `ping_try_init_ctx` → `ping_ctx_init_jetty`：真正建 jetty(5) + prefill 1024 个 recv WR。**这是 server "开门"的时刻**。
- `LAST_DEL` → `ping_try_uninit_ctx`：拆 jetty/jfr/jfc/seg（保留 ctx+wq，待下次 FIRST_ADD 重建）。

> 之所以分两段（`add_device` 分配 ctx、`FIRST_ADD` 才建 jetty），是因为 ubcore device 上线时 main UE EID 可能还没配置好，找不到 `eid_index`，必须等 EID 事件。`ping_find_eid_by_main_ue_eid`（[:216](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L216)）在 dev 的 eid_table 里按 main_ue_eid 反查 eid_index。

### 5.3 echo 处理路径（核心）

已在 §3.2.2 给出。补充几个关键设计：

- **workqueue 解耦**：CQE 回调 `ping_recv_jfc_comp` / `ping_send_jfc_comp`（[:466,475](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L466)）不在中断/软IRQ 上下文做 echo，而是 `ping_wq_queue_work` 投递到 `ping_wq`，由工作线程 `ping_recv_work_handler` / `ping_send_work_handler` 处理。这避免在 ubcore completion 路径里递归调用 ubcore send。
- **`wq_stopped` 防撕裂**（[:451-462](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L451)）：jetty 拆除期间，`ping_wq_queue_work` 在 `wq_lock` 保护下检查 `wq_stopped`，拆除后新投递直接返回 `-ESHUTDOWN`，并 `kfree(pwork)`，防止"jetty 已删、work 仍访问"的 use-after-free。
- **recv WR 水位**：echo 发送完成（`ping_wq_on_sended`）后立刻 `ping_refill_recv_wr` 把刚用掉的 recv buf 重新 post 回去（[:359](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L359)）。出错路径（`ping_wq_on_recved` 的 `refill:` 标签）也会 refill，保证始终有 1024 个 recv WR 在挂。
- **tjetty 缓存**：每个对端 EID 只 `ubcore_import_jetty` 一次，hash 缓存 + kref，避免高频 ping 下反复建链。`ping_tjetty_put` 在引用归零时 `ubcore_unimport_jetty`。

### 5.4 ubcore 原语对照（用户态↔内核态 API 对应）

| 操作 | 用户态 (liburma) | 内核态 (ubcore) |
|---|---|---|
| 注册内存段 | `urma_register_seg` | `ubcore_register_seg` |
| 建 jfc | `urma_create_jfc` / `urma_create_jfce` | `ubcore_create_jfc` |
| rearm jfc | `urma_rearm_jfc` | `ubcore_rearm_jfc` |
| 建 jfr | `urma_create_jfr` | `ubcore_create_jfr` |
| 建 jetty | `urma_create_jetty` | `ubcore_create_jetty` |
| import 远端 jetty | `urma_import_jetty` | `ubcore_import_jetty` |
| post recv | `urma_post_jetty_recv_wr` | `ubcore_post_jetty_recv_wr` |
| post send | `urma_post_jetty_send_wr` | `ubcore_post_jetty_send_wr` |
| wait jfc | `urma_wait_jfc` | （内核侧用回调+workqueue，不 wait） |
| poll jfc | `urma_poll_jfc` | `ubcore_poll_jfc` |
| ack jfc | `urma_ack_jfc` | — |

两侧 API 几乎对称（urma_ 前缀 vs ubcore_ 前缀），因为 liburma 用户态库本身就是 ubcore 内核接口的薄封装。

---

## 6. 编译与构建

### 6.1 用户态 urma_ping

[CMakeLists.txt](00_umdk/umdk/src/urma/tools/urma_ping/CMakeLists.txt)：

```cmake
add_executable(urma_ping ping_log.c ping_netlink.c ping_parameters.c ping_run.c ping_stat.c ping.c)
target_include_directories(urma_ping PRIVATE /usr/include/libnl3)
target_link_libraries(urma_ping PRIVATE urma nl-genl-3 nl-3 m)
install(TARGETS urma_ping DESTINATION /usr/bin)
```

依赖：`liburma`（URMA 用户态库）、`libnl3`（genl）、`libm`。产物：`/usr/bin/urma_ping`。

构建方式（两种，见 [README_zh.md](00_umdk/umdk/README_zh.md)）：

**方式 A — RPM 包**（在 `00_umdk/umdk/` 根目录）：
```bash
yum install -y rpm-build make cmake gcc gcc-c++ glibc-devel openssl-devel \
               glib2-devel libnl3-devel libummu-devel kernel-devel
mkdir -p /root/rpmbuild/SOURCES/
tar -czf /root/rpmbuild/SOURCES/umdk-26.06.0.tar.gz --exclude=.git $(ls -A)
rpmbuild -ba umdk.spec
rpm -ivh /root/rpmbuild/RPMS/*/umdk*.rpm
```
RPM 选项：`--with urma`（默认关 urma？需显式开）、`--define 'kernel_version 6.6.92'` 等。

**方式 B — Bazel 脚本**（在 `src/urma/`）：
```bash
cd src/urma
./urma_bazel.sh compile --config=release --config=arm64 --define=build_udma=true
# 产物：src/urma/urma-bazel-<timestamp>.tar.gz
# 目标侧安装：
mkdir -p /tmp/urma-bazel && tar -xzf urma-bazel-<ts>.tar.gz -C /tmp/urma-bazel
/tmp/urma-bazel/urma_bazel.sh install   # 需 root/sudo
```

### 6.2 内核 ubcore（含 ubmgr_ping）

`ubmgr_ping` **不是独立模块**，静态编进 `ubcore.ko`：

- [ubcore/Makefile:37-38](01_kernel/drivers/ub/urma/ubcore/Makefile#L37)：`ubcore-objs` 包含 `ubmgr/ubmgr.o` 和 `ubmgr/ubmgr_ping.o`。
- [drivers/ub/Kconfig](01_kernel/drivers/ub/Kconfig)：`UB_URMA` 为 `tristate "Unified Bus (UB) urma support" default m`，依赖 `UB`（`depends on ARM64`）。`UB_URMA=m` → 编出 `ubcore.ko`。

加载（需先加载 UB 底层依赖，见 README 部署章节）：
```bash
modprobe ubcore      # 含 ubmgr_ping client，但 jetty 尚未建（等 main UE EID 事件）
modprobe ubagg       # 注册 UBAGG_GENL family，提供 topo 查询
modprobe uburma      # 用户态 URMA 字符设备接口
# 其余：ubfi/ummu-core/ummu/ubus/ubase/unic/cdma/udma 按需
```

> 关键：`ubagg` 必须 modprobe，否则用户态 `ping_get_topo_info` 会因 `genl_ctrl_resolve("UBAGG_GENL")` 失败而无法解析 bonding EID（除非用户直接给 primary EID 且加 `-I` 指定源，绕开 topo 查询）。

### 6.3 接缝处的 ABI 约束（升级注意）

| 接缝 | 用户态 | 内核态 | 风险 |
|---|---|---|---|
| `ubagg_nl_topo_info` 内存布局 | `ping_core_cmd_topo_info_t` | `struct ubagg_nl_topo_info` | **无共享头文件**，靠布局对齐，任一侧改字段顺序/大小即破。 |
| `topo_node` 常量 | IODIE_NUM=2,PORT_NUM=9,DEV_NUM=256,MAX_NODE_NUM=1024 | `ubagg_topo_node` 同值 | 用户态常量硬编码在 ping_run.h，须与 ubagg 的 `ubagg_topo_info.h` 保持一致。 |
| jetty id = 5 | `PING_WK_JETTY_ID` | `PING_WK_JETTY_ID` | 双向硬编码，改一侧必改另一侧。 |
| PING_MAX_MSG_SIZE | size ≤ 4096 | 4096 | 用户态可传更小，但超过 4096 会被内核 `min_t(...,PING_MAX_MSG_SIZE)` 截断。 |

---

## 7. 运行命令与参数详解

### 7.1 前置条件检查

```bash
# 1. 确认内核模块已加载
lsmod | grep -E "ubcore|ubagg|uburma|udma"
# 期望看到：ubcore ubagg uburma udma ...

# 2. 确认 UBAGG_GENL family 已注册（用户态 netlink 依赖）
genl-ctrl-list 2>/dev/null | grep -i ubagg   # 需 libnl3 的 genl-ctrl-list
# 或：cat /proc/net/genl 2>/dev/null | grep -i ubagg

# 3. 确认 urma_ping 已安装
which urma_ping && urma_ping --version
# Version: 0.0.1

# 4. 确认设备/EID 已就绪（ubmgr_ping server jetty 是否已建，取决于 main UE EID 是否 FIRST_ADD）
# 可观察 dmesg：
dmesg | grep -iE "ping|ubmgr|jetty|main_ue_eid" | tail
```

权限：
```bash
chmod -R 777 /usr/lib64/urma
chmod 777 /dev/ummu/tid
chmod 755 /usr/lib64/liburma*
```

### 7.2 命令语法

```
urma_ping [option] <destination>
```

### 7.3 典型用法

**① 最常用——ping 一个 bonding EID（自动解析 primary + 选源）**：
```bash
urma_ping 0xfe8000000000000000000000c0a80101
# 或点分/数值形式：
urma_ping fe80::c0a8:0101
urma_ping 192.168.1.1
```
内部：topo netlink 查询 → 解析 bonding → 选 primary EID + chip_id → 建 URMA 资源 → import jetty(5) → 收发。

**② 指定源 primary EID（绕过源端 topo 解析）**：
```bash
urma_ping -I fe80::c0a8:0101 fe80::c0a8:0102
```

**③ 限定发包数与大小**：
```bash
urma_ping -c 10 -s 64 192.168.1.1
```

**④ 长时间压测，设 deadline 与超时**：
```bash
urma_ping -c 100000 -i 0 -w 300 -W 2 192.168.1.1
```
> 注意：`-i 0` 表示无间隔连发；`-w 300` 跑满 300 秒停；`-W 2` 每个回复等 2 秒。

**⑤ 安静模式，只看统计**：
```bash
urma_ping -q -c 100 192.168.1.1
```

**⑥ 详细调试**：
```bash
urma_ping -vv -c 3 192.168.1.1
```
> `-vv` 会打印 EID 解析过程、device 选择、send/recv 时序、liburma 内部日志。

**⑦ 永久 ping 直到 Ctrl+C**：
```bash
urma_ping 192.168.1.1
# Ctrl+C 触发 print_stat() 后退出
```

### 7.4 输出样例

正常单行：
```
URMA_PING fe80::c0a8:0101 4 bytes of data.
4 bytes from fe80::c0a8:0101: seq=1 time=0.123 ms
4 bytes from fe80::c0a8:0101: seq=2 time=0.119 ms
^C
--- ping statistics ---
3 packets transmitted, 3 received, 0.0% packet loss, time 2003ms
rtt min/avg/max/mdev = 0.119/0.121/0.123/0.002 ms
```

超时：
```
Request timeout for seq=2
```

序列不匹配（罕见，通常是并发 ping 串扰）：
```
Unmatched reply received: expected seq=2, got seq=3
```

CQE 错误状态：
```
From fe80::c0a8:0101 seq=2 cr_status=5
```

### 7.5 退出码

- `0`：正常完成（count 跑完或 deadline 到）。
- `1`（`EXIT_FAILURE`）：参数错误、URMA 资源建链失败、send/recv 失败。
  - 注意 `Ctrl+C` 走 `signal_handler` → `exit(0)`，所以中断退出码是 0。

---

## 8. 运维与排障

### 8.1 常见故障与定位

| 现象 | 可能原因 | 定位手段 | 处置 |
|---|---|---|---|
| `Resolving of "UBAGG_GENL" failed` | ubagg 模块未加载 | `lsmod \| grep ubagg` | `modprobe ubagg` |
| `Failed to get ubagg topo` | ubagg family 在但 topo 未生成（设备未上线） | `dmesg \| grep ubagg` | 等设备/EID 就绪 |
| `Failed to find the destination primary EID` | 传的 bonding EID 在 topo 里查不到 | `-vv` 看解析日志 | 确认 EID 正确、对端已配置 |
| `Failed to import target urma jetty` | 对端 server jetty(5) 未建 | 对端 `dmesg \| grep "ping wk jetty"` | 确认对端 ubcore+ubagg 已加载、main UE EID 已 FIRST_ADD |
| `Request timeout for seq=N` | 报文发出但未回，或回的 imm 不匹配 | `-W` 调大；查对端 `dmesg` 是否收到 | 查 ubcore/硬件链路 |
| `Failed to create urma context` | 设备/EID 权限或资源不足 | `ls -l /dev/ummu/*`；`chmod` | 补权限 |
| `Size must be less than or equal to 4096` | `-s` 超 4096 | — | 改 `-s` ≤ 4096 |
| `Unknown option -S` | optstring 含 `S` 但无实现 | — | 当前版本 `-S` 不可用（疑似预留），勿用 |

### 8.2 内核侧观测点

- `dmesg` 关键日志：
  - `"Tjetty already imported. eid ..."` —— 对端重复建链（正常）。
  - `"Ping recv, src_eid=... dst_eid=... comp_len=..."` —— `ubcore_log_info_rl` 限速打印，server 收到包。
  - `"Ping send, src_eid=... dst_eid=..."` —— server echo 回发。
  - `"Primary eid not found, init deferred"` —— main UE EID 未就绪，jetty 暂未建。
  - `"Failed to init ping ctx"` —— jetty 建链失败。

- workqueue：`ps -ef | grep ping_wq` 或 `/sys/kernel/debug/workqueue` 下查 `ping_wq`（WQ_UNBOUND）。

### 8.3 双向验证清单

要确认端到端打通，按序确认：

1. ✅ 两端 `lsmod` 都有 `ubcore ubagg uburma`。
2. ✅ 两端 `/proc/net/genl` 都有 `UBAGG_GENL`。
3. ✅ **被 ping 端** `dmesg` 出现 "ping wk jetty" 成功创建（说明 main UE EID 已 FIRST_ADD，server jetty(5) 已开门）。
4. ✅ **发起端** `urma_ping -vv` 能完成 topo 解析（看到 "Destination EID resolved to ..."）。
5. ✅ `urma_ping -c 3 <dst>` 有 seq 回复。

### 8.4 性能与限制

- 单包 size 上限 **4096 字节**（用户态 `MAX_PING_SIZE`、内核 `PING_MAX_MSG_SIZE` 双重限制）。
- server recv buffer 总量 `PING_BUF_SIZE = 1024 * 4096 = 4 MiB`（每设备），prefill 1024 个 recv WR。
- client/server jfc depth 均 1024。
- 计时精度：`CLOCK_MONOTONIC` 毫秒级（`tv_nsec/1e6`），**亚毫秒级会量化到 ~1ms 粒度**，不适合测 <1ms 的微时延（需改用 `clock_gettime` 纳秒）。
- 内核侧 echo 在 workqueue 线程上下文执行，非硬实时；高 ping 频率受调度抖动影响。

---

## 9. 设计要点总结（给二次开发者）

1. **两条通道分离**：netlink（控制面，查 topo，对接 ubagg）+ URMA 数据面（对接 ubmgr_ping 的 jetty）。不要把两者混淆。
2. **server 是被动 echo**：ubmgr_ping 不主动发包，只回 `SEND_IMM`，`imm_data` 透传 seq。client 用 seq 做请求-响应匹配。
3. **jetty_id = 5 是硬契约**：client 的 `urma_import_jetty(id=5)` 必须命中 server 的 `ubcore_create_jetty(id=5)`。
4. **topo RPC 用"地址传递"**：用户态把结构体缓冲区地址作为 NLA_U64 传给内核，内核直接回填。**无共享头文件**，靠内存布局对齐——这是脆弱点，升级时务必同步两侧 `ubagg_nl_topo_info` 与 `topo_node` 常量。
5. **懒初始化**：ubmgr_ping 的 jetty 建链由 `main UE EID FIRST_ADD` 事件触发，而非 `modprobe` 时刻；`LAST_DEL` 时拆 jetty 但保留 ctx/wq，便于 EID 抖动场景快速重建。
6. **workqueue 解耦 + wq_stopped 防撕裂**：completion 回调里只 queue_work，真正 echo 在工作线程；拆除期间 `wq_stopped` 阻断新 work 并 kfree，避免 UAF。
7. **tjetty 缓存 + kref**：同一对端只 import 一次，高频 ping 不重复建链。

---

## 10. 附录：关键源码索引

### 用户态（`00_umdk/umdk/src/urma/tools/urma_ping/`）

| 主题 | 位置 |
|---|---|
| main 入口 | [ping.c:22](00_umdk/umdk/src/urma/tools/urma_ping/ping.c#L22) |
| 默认参数 | [ping.c:24-34](00_umdk/umdk/src/urma/tools/urma_ping/ping.c#L24) |
| getopt 解析 | [ping_parameters.c:115](00_umdk/umdk/src/urma/tools/urma_ping/ping_parameters.c#L115) |
| EID 字符串解析 | [ping_parameters.c:57](00_umdk/umdk/src/urma/tools/urma_ping/ping_parameters.c#L57) |
| usage/help | [ping_parameters.c:96](00_umdk/umdk/src/urma/tools/urma_ping/ping_parameters.c#L96) |
| netlink 初始化 | [ping_netlink.c:47](00_umdk/umdk/src/urma/tools/urma_ping/ping_netlink.c#L47) |
| topo 查询主逻辑 | [ping_netlink.c:124](00_umdk/umdk/src/urma/tools/urma_ping/ping_netlink.c#L124) |
| bonding→primary EID 解析 | [ping_run.c:123](00_umdk/umdk/src/urma/tools/urma_ping/ping_run.c#L123) |
| URMA 资源建链 | [ping_run.c:291](00_umdk/umdk/src/urma/tools/urma_ping/ping_run.c#L291) |
| import jetty(5) | [ping_run.c:449-457](00_umdk/umdk/src/urma/tools/urma_ping/ping_run.c#L449) |
| ping 主循环 | [ping_run.c:671](00_umdk/umdk/src/urma/tools/urma_ping/ping_run.c#L671) |
| 发送 SEND_IMM | [ping_run.c:528](00_umdk/umdk/src/urma/tools/urma_ping/ping_run.c#L528) |
| 接收+seq 比对 | [ping_run.c:613](00_umdk/umdk/src/urma/tools/urma_ping/ping_run.c#L613) |
| RTT 计时 | [ping_stat.c:33](00_umdk/umdk/src/urma/tools/urma_ping/ping_stat.c#L33) |
| 统计输出 | [ping_stat.c:44](00_umdk/umdk/src/urma/tools/urma_ping/ping_stat.c#L44) |

### 内核态（`01_kernel/drivers/ub/urma/ubcore/ubmgr/`）

| 主题 | 位置 |
|---|---|
| ubmgr 入口 | [ubmgr.c:17](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr.c#L17) |
| ping init/uninit | [ubmgr_ping.c:836](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L836) |
| client 注册 | [ubmgr_ping.c:742](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L742) |
| dev 上线/下线 | [ubmgr_ping.c:686](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L686) / [:722](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L722) |
| main UE EID 事件 | [ubmgr_ping.c:811](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L811) |
| jetty 懒初始化 | [ubmgr_ping.c:513](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L513) |
| recv 处理（echo 回发） | [ubmgr_ping.c:273](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L273) |
| send 完成处理 | [ubmgr_ping.c:338](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L338) |
| workqueue 投递+防撕裂 | [ubmgr_ping.c:427](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L427) |
| tjetty 缓存 | [ubmgr_ping.c:143](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L143) |
| prefill recv WR | [ubmgr_ping.c:484](01_kernel/drivers/ub/urma/ubcore/ubmgr/ubmgr_ping.c#L484) |

### 内核 ubagg（对接 netlink 控制面）

| 主题 | 位置 |
|---|---|
| family 名/version | [ubagg_netlink.h:14-15](01_kernel/drivers/ub/urma/ubagg/ubagg_netlink.h#L14) |
| cmd/attr 枚举 | [ubagg_netlink.h:17-34](01_kernel/drivers/ub/urma/ubagg/ubagg_netlink.h#L17) |
| nla_policy | [ubagg_netlink.c:28-32](01_kernel/drivers/ub/urma/ubagg/ubagg_netlink.c#L28) |
| ubagg_nl_topo_info 结构 | [ubagg_netlink.c:40-47](01_kernel/drivers/ub/urma/ubagg/ubagg_netlink.c#L40) |
| GET_TOPO doit 回调 | [ubagg_netlink.c:50,65-70](01_kernel/drivers/ub/urma/ubagg/ubagg_netlink.c#L50) |
| family 注册 | [ubagg_main.c:163](01_kernel/drivers/ub/urma/ubagg/ubagg_main.c#L163) |

---

*本手册基于源码走读生成，所有结论均可回溯到上述 file:line。如源码版本演进，请以实际代码为准并同步更新接缝处的 ABI 约束（§6.3）。*
