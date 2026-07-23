# urma_mesh_perftest 需求文档

## 1. 概述

### 1.1 背景

在 URMA（Unified Remote Memory Access）网络中，需要验证多节点间的全连接（mesh）打流性能。现有 `urma_perftest` 工具仅支持点对点（client-server）模式的性能测试，无法满足 n 个节点互相打流的场景需求。

### 1.2 目标

编写一个二进制程序 `urma_mesh_perftest`，支持 n 个节点以 mesh 拓扑互相打流。每个节点接收其他 n-1 个节点的 IP 列表作为参数，通过 URMA RDMA Write 操作向所有对端发送 4MB 数据，并测量每次发送的延迟（从 `urma_post_jetty_send_wr` 到 `urma_poll_jfc` 的时间）。

### 1.3 参考实现

- `src/urma/tools/urma_perftest/` - URMA 性能测试工具（点对点模式）
- `src/urma/examples/urma_sample.c` - URMA API 使用示例
- `src/urma/lib/uvs/core/include/uvs_api.h` - UVS 路径查询 API

---

## 2. 功能需求

### 2.1 本端资源初始化

每个节点启动后，按以下顺序初始化本端 URMA 资源：

| 步骤 | URMA API | 说明 |
|------|----------|------|
| 1 | `urma_init()` | 初始化 URMA 库 |
| 2 | `urma_get_device_by_name(dev_name)` | 查找指定设备（默认 `bonding_dev_0`） |
| 3 | `urma_query_device()` | 查询设备属性 |
| 4 | `urma_get_eid_list()` | 获取本端所有 EID |
| 5 | `uvs_get_path_set(src, dst)` | 查询路径集，获取互相连通的 EID（src=dst 为本端端口 EID） |
| 6 | `urma_register_seg()` | 注册 4MB 内存段 |
| 7 | `urma_create_jetty()` | 为每个 local_port_eid 创建 jetty |

**详细流程：**

```
urma_init()
urma_get_device_by_name("bonding_dev_0")
urma_get_eid_list()  ->  获取本端 EID 列表
uvs_get_path_set(src=本端EID, dst=本端EID)  ->  获取互相连通的 EID
urma_register_seg()  ->  注册 4MB 内存段
foreach local_port_eid:
    urma_create_context(eid_index)
    urma_alloc_token_id()        [仅 UB 设备]
    urma_create_jfc()
    urma_create_jfr()             [共享 JFR 模式]
    urma_create_jetty()           [share_jfr=1]
    urma_register_seg()           [每个 EID 注册 4MB 段]
```

### 2.2 Socket 连接管理

使用 TCP socket 在节点间交换 URMA 资源信息。

**连接建立流程：**

```
foreach node (IP 列表中的每个 IP):
    while true:
        connect(ip, port)  ->  检查是否连接成功
        如果失败则重试（间隔 200ms）

foreach node:
    sync()  ->  确认建链已完成（类似 perftest 的 sync_time 机制）
```

**Mesh 连接去重机制：**

由于 n 个节点互相连接，每对节点之间可能建立两条 TCP 连接（A->B 和 B->A）。需要通过以下机制去重：

- 每个节点启动时生成一个随机 `node_id`
- 每条连接建立后交换 `node_id`
- **出向连接（connect）**：当 `my_node_id < peer_node_id` 时保留，否则关闭
- **入向连接（accept）**：当 `my_node_id > peer_node_id` 时保留，否则关闭
- 保证每对节点之间恰好保留一条 TCP 连接

**监听线程：**

- 节点启动后创建监听线程，`bind` + `listen` 在指定端口
- 监听线程循环 `accept`，通过 `getpeername` 获取对端 IP
- 对每个入向连接执行 node_id 交换和去重判断

### 2.3 信息交换与远端资源导入

连接建立并 sync 后，与每个对端节点交换 URMA 资源信息，并导入远端资源。

**交换信息内容：**

每个 EID 的交换信息包括：

| 字段 | 类型 | 说明 |
|------|------|------|
| eid | `uint8_t[16]` | EID 原始值 |
| uasid | `uint32_t` | 用户地址空间 ID |
| seg_va | `uint64_t` | 注册段的虚拟地址 |
| seg_len | `uint64_t` | 注册段长度 |
| seg_attr | `uint32_t` | 段属性（access、token_policy 等） |
| seg_token_id | `uint32_t` | 段的 token ID |
| jetty_eid | `uint8_t[16]` | jetty 的 EID |
| jetty_uasid | `uint32_t` | jetty 的 uasid |
| jetty_id | `uint32_t` | jetty ID |

**交换流程：**

```
foreach node:
    exchange_info():
        发送本端所有 EID 的 seg/jetty 信息
        接收对端所有 EID 的 seg/jetty 信息
    urma_import_seg()           ->  导入对端内存段
    foreach path (对端每个 EID):
        urma_import_jetty()     ->  导入对端 jetty
```

### 2.4 打流与延迟测量

**打流流程：**

```
foreach tjetty (每个已导入的远端 jetty):
    启动独立线程，循环执行：
        1. 构建 4MB WRITE WR（src=本端 seg, dst=远端 imported seg, tjetty=远端 jetty）
        2. 记录 t_post = get_cycles()（CPU TSC 时间戳）
        3. urma_post_jetty_send_wr()
        4. urma_poll_jfc() 直到获取完成
        5. 记录 t_poll = get_cycles()
        6. 计算 delta = t_poll - t_post
        7. 间隔 n ms 后继续下一次发送
```

**延迟统计指标：**

| 指标 | 计算方式 |
|------|----------|
| 平均值（avg） | `sum(delta) / count`，转换为微秒 |
| P99 | 排序后取 `ceil(count * 0.99)` 位置的值 |
| P99.99 | 排序后取 `ceil(count * 0.9999)` 位置的值 |
| Max | 排序后取最大值（去除 top-2 异常值后） |
| Min | 排序后取最小值 |

**并发模型：**

- 每个远端 tjetty 对应一个独立的打流线程
- 本端 jetty 按 round-robin 方式分配给打流线程
- 同一个本端 jetty 上的 post+poll 操作通过 mutex 串行化，避免 JFC 竞争

---

## 3. 命令行接口

### 3.1 参数定义

| 参数 | 短选项 | 类型 | 默认值 | 说明 |
|------|--------|------|--------|------|
| `--dev` | `-d` | string | `bonding_dev_0` | URMA 设备名称 |
| `--ips` | `-i` | string | **必填** | 逗号分隔的远端节点 IP 列表 |
| `--port` | `-p` | uint16 | `21115` | TCP 信息交换端口 |
| `--interval` | `-t` | uint32 | `1000` | 发送间隔（毫秒） |
| `--size` | `-s` | uint32 | `4194304` (4MB) | 每次发送的数据大小（字节） |
| `--iters` | `-n` | uint32 | `1000` | 每个 tjetty 的发送次数 |
| `--verbose` | `-v` | - | 关闭 | 打印 DEBUG 级维测日志 |
| `--help` | `-h` | - | - | 打印帮助信息 |

### 3.2 使用示例

```bash
# 3 节点 mesh 打流示例
# 节点 A (10.0.0.1):
urma_mesh_perftest --ips 10.0.0.2,10.0.0.3 --dev bonding_dev_0 --interval 100 --iters 5000

# 节点 B (10.0.0.2):
urma_mesh_perftest --ips 10.0.0.1,10.0.0.3 --dev bonding_dev_0 --interval 100 --iters 5000

# 节点 C (10.0.0.3):
urma_mesh_perftest --ips 10.0.0.1,10.0.0.2 --dev bonding_dev_0 --interval 100 --iters 5000
```

### 3.3 输出格式

```
=== Latency Statistics (post_jetty_send_wr -> poll_jfc) ===
  Peer                EID    Sends   Errors  avg[us]      p99[us]      p99.99[us]     max[us]     min[us]
  -------------------------------------------------------------------------------------------
  10.0.0.2            0      5000    0       42.35        68.12        85.44          92.31       38.01
  10.0.0.2            1      5000    0       41.88        65.77        82.19          89.05       37.55
  10.0.0.3            0      5000    0       55.12        88.34        105.67         118.92      50.23
  10.0.0.3            1      5000    0       54.67        85.91        102.33         115.78      49.88
```

---

## 4. 架构设计

### 4.1 模块划分

```
urma_mesh_perftest.c
├── 参数解析模块 (parse_args)
├── 本端资源管理模块 (init_local_resources / destroy_local_resources)
├── TCP mesh 连接管理模块
│   ├── 监听线程 (listen_thread_main)
│   ├── 连接重试 (connect_to_peer)
│   └── 建链同步 (establish_mesh_connections)
├── 信息交换模块 (exchange_info_with_peer)
├── 远端资源导入模块 (import_remote_resources / unimport_remote_resources)
├── 打流引擎模块
│   ├── 打流线程 (traffic_thread_main)
│   └── 启动/等待 (start_traffic / wait_traffic_done)
└── 延迟统计模块 (print_lat_stats)
```

### 4.2 主流程

```
main()
├── 1. parse_args()                           解析命令行参数
├── 2. init_local_resources()                 初始化本端 URMA 资源
│   ├── urma_init()
│   ├── urma_get_device_by_name()
│   ├── urma_get_eid_list()
│   ├── uvs_get_path_set()                    查询路径集
│   └── foreach EID:
│       ├── urma_create_context()
│       ├── urma_alloc_token_id()
│       ├── urma_create_jfc() / urma_create_jfr()
│       ├── urma_create_jetty()
│       └── urma_register_seg()
├── 3. establish_mesh_connections()           建立 TCP mesh 连接
│   ├── start_listener()                      启动监听线程
│   ├── connect_to_peer() × N                 连接所有对端（重试）
│   └── sync() × N                            确认所有建链完成
├── 4. exchange_info_with_peer() × N          与每个对端交换 seg/jetty 信息
├── 5. import_remote_resources()              导入远端 seg 和 jetty
│   ├── urma_import_seg()
│   ├── urma_import_jetty()
│   └── sync() × N                            确认导入完成
├── 6. start_traffic()                        启动打流线程
├── 7. wait_traffic_done()                    等待所有打流完成
├── 8. print_lat_stats()                      打印延迟统计
└── 9. cleanup()                              清理资源
    ├── unimport_remote_resources()
    ├── close_connections()
    └── destroy_local_resources()
```

### 4.3 关键数据结构

```c
/* 本端每个 EID 的资源 */
typedef struct local_eid {
    urma_eid_t         eid;
    uint32_t           eid_index;
    urma_context_t    *ctx;
    urma_jfc_t        *jfc;
    urma_jfr_t        *jfr;
    urma_jetty_t      *jetty;
    urma_target_seg_t *tseg;
    urma_token_id_t   *token_id;
    void              *buf;          /* 4MB 注册内存 */
    pthread_mutex_t    jetty_mutex;  /* 串行化 post+poll */
} local_eid_t;

/* 远端每个 EID 的导入资源 */
typedef struct remote_eid_res {
    urma_target_seg_t   *import_tseg;
    urma_target_jetty_t *import_tjetty;
    seg_jetty_info_t     info;       /* 从对端接收的信息 */
} remote_eid_res_t;

/* 远端节点 */
typedef struct remote_node {
    char              ip[64];
    int               sock_fd;
    pthread_mutex_t   sock_mutex;
    uint32_t          eid_cnt;
    remote_eid_res_t  eids[MESH_MAX_EIDS];
} remote_node_t;

/* 打流线程参数 */
typedef struct traffic_arg {
    struct mesh_ctx   *mesh;
    remote_node_t     *node;
    uint32_t           remote_eid_idx;
    local_eid_t       *local;        /* 使用的本端 jetty */
    lat_stats_t        stats;        /* 延迟统计数据 */
    uint64_t           send_cnt;
    uint64_t           err_cnt;
} traffic_arg_t;
```

---

## 5. 非功能需求

### 5.1 性能要求

| 指标 | 要求 |
|------|------|
| 最大节点数 | 支持 64 个节点 |
| 最大 EID 数 | 每节点支持 16 个 EID |
| 最大打流线程数 | 64 × 16 = 1024 |
| JFC 深度 | 1024 |
| JFS 深度 | 128 |
| JFR 深度 | 128 |
| 连接重试间隔 | 200ms |
| 最大重试次数 | 10000 次（约 33 分钟） |

### 5.2 可靠性要求

- TCP 连接建立失败时自动重试，不退出程序
- `urma_post_jetty_send_wr` 失败时记录错误并继续下一次发送
- `urma_poll_jfc` 返回错误状态时记录 CR status 并继续
- 所有 URMA 资源在程序退出时正确释放（unimport -> unregister -> delete -> uninit）

### 5.3 传输模式

- 使用 **RM（Reliable Message）** 传输模式（`URMA_TM_RM`）
- UB 设备无需 `urma_advise_jetty` / `urma_bind_jetty`
- 使用 **RTP（Reliable Transport Path）** 路径类型

### 5.4 编码规范

- 遵循项目 C 编码规范（`skills/c-code-standard/references/c-rules.md`）
- C99 标准
- 函数不超过 50 行非空非注释行
- 嵌套不超过 4 层
- 全局变量加 `g_` 前缀，静态变量加 `s_` 前缀
- 源文件内函数加 `static`

---

## 6. 构建与部署

### 6.1 CMakeLists.txt

```cmake
add_executable(urma_mesh_perftest
    urma_mesh_perftest.c
)

set_target_properties(urma_mesh_perftest
    PROPERTIES
        C_STANDARD 99
)

target_include_directories(urma_mesh_perftest
    PRIVATE
        ${CMAKE_SOURCE_DIR}/urma/lib/urma/core/include
        ${CMAKE_SOURCE_DIR}/urma/lib/urma/bond/include
        ${CMAKE_SOURCE_DIR}/urma/lib/uvs/core/include
        ${CMAKE_SOURCE_DIR}/urma/common/include
)

target_link_libraries(urma_mesh_perftest
    PRIVATE
        urma tpsa pthread m
)

install(TARGETS urma_mesh_perftest
    DESTINATION /usr/bin
)
```

### 6.2 依赖库

| 库 | 用途 |
|----|------|
| `urma` | URMA 核心库（设备管理、jetty、seg、post/poll） |
| `tpsa` | UVS 库（`uvs_get_path_set` 路径查询） |
| `pthread` | 多线程（监听线程、打流线程） |
| `m` | 数学库（`ceil`、`sqrt` 等统计计算） |

### 6.3 目录结构

```
src/urma/tools/urma_mesh_perftest/
├── urma_mesh_perftest.c      # 主程序
├── CMakeLists.txt            # 构建配置
└── urma_mesh_perftest_req.md # 本需求文档
```

---

## 7. 错误处理

### 7.1 错误处理策略

| 场景 | 处理方式 |
|------|----------|
| `urma_init` 失败 | 打印错误，退出程序 |
| 设备未找到 | 打印错误，退出程序 |
| `urma_create_context` 失败 | 打印错误，清理已创建资源，退出 |
| TCP connect 失败 | 重试（间隔 200ms），超过最大次数退出 |
| TCP 信息交换失败 | 打印错误，退出程序 |
| `urma_import_seg/jetty` 失败 | 打印错误，退出程序 |
| `urma_post_jetty_send_wr` 失败 | 记录错误计数，继续下一次发送 |
| `urma_poll_jfc` CR status 非 SUCCESS | 记录错误计数，继续下一次发送 |

### 7.2 资源清理顺序

```
unimport_remote_resources()     ->  urma_unimport_jetty, urma_unimport_seg
close_connections()             ->  close TCP sockets, join listen thread
destroy_local_resources()       ->  urma_unregister_seg, urma_delete_jetty,
                                    urma_delete_jfr, urma_delete_jfc,
                                    urma_free_token_id, urma_delete_context,
                                    urma_uninit
```

---

## 8. 验收标准

- [ ] 程序可通过 CMake 正确编译，生成 `urma_mesh_perftest` 二进制
- [ ] 支持 `--ips` 参数传入逗号分隔的 IP 列表
- [ ] 支持 `--dev`、`--port`、`--interval`、`--size`、`--iters`、`--verbose` 参数
- [ ] 能正确查找设备、获取 EID 列表、查询路径集
- [ ] 能为每个 EID 创建 context、jetty、seg
- [ ] 能与所有对端节点建立 TCP 连接（含重试和去重）
- [ ] 能正确交换 seg/jetty 信息并导入远端资源
- [ ] 能向每个远端 tjetty 周期性发送 4MB WRITE WR
- [ ] 能正确测量 post->poll 延迟并计算 avg/p99/p9999/max
- [ ] 统计结果以表格形式打印在终端
- [ ] 程序退出时正确释放所有资源
