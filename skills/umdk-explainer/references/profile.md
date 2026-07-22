# UMDK 框架 Profile（框架特异内容，唯一与具体框架绑定的核心文件）

本文件是 umdk-explainer 中**唯一与 UMDK 强绑定**的知识入口。移植到其他代码库时，只需重写本文件
及其指向的索引文件（api-index / data-structures / call-graphs），SKILL.md 与 methodology.md 原样复用。

> 维护者：本文件由 `umdk-skill-updater` 自动/半自动维护。手动修改后请在文末"变更记录"追加一行。
> 采集基准：仓库根 `d:/gitbase/umdk_blue`，范围限定 `umdk/` 目录，UMDK 版本 26.06.0（见 `umdk/umdk.spec`）。

## 1. 项目一句话定位

UMDK（Unified Memory Development Kit，灵衢统一总线内存开发包）是华为围绕"灵衢总线"（Unified Bus, UB）
硬件打造的**类 RDMA 通信栈**的用户态开发包：上承应用，提供 URMA 内存语义（单边/双边/原子）+ URPC 远程
调用 + DLock 分布式锁 + UMS socket 加速 + CAM 昇腾 AI 通信加速。

## 2. 目录布局

本 skill 覆盖 `umdk/` 目录：

| 子目录                            | 角色           | 说明                                               |
| --------------------------------- | -------------- | -------------------------------------------------- |
| `umdk/src/`                     | 用户态源码     | `{urma,urpc,ulock/dlock,usock/ums,cam}` 五大组件 |
| `umdk/doc/`                     | 文档           | `ch/` 中文、`en/` 英文，按模块分子目录         |
| `umdk/build/`、`umdk/docker/` | 构建与容器     | 构建脚本、Dockerfile                               |
| `umdk/umdk.spec`                | RPM 打包       | 组件编译开关、版本号（26.06.0）                    |
| `umdk/skills/`                  | 开发辅助 skill | c-code-standard、verbs-to-urma-converter 等        |

## 3. 模块地图

每个模块一行速查：目录 → 职责 → 关键头文件 → 入口 → 文档。

### 3.1 用户态组件（umdk/src/）

| 组件             | 目录              | 职责                                                                | 关键头文件（公开 API）                                                              | 文档                                  |
| ---------------- | ----------------- | ------------------------------------------------------------------- | ----------------------------------------------------------------------------------- | ------------------------------------- |
| **URMA**   | `urma/`         | 统一远程内存访问，核心基础库；单边(read/write)/双边(send/recv)/原子 | `urma/lib/urma/core/include/{urma_api.h, urma_types.h}`                           | `doc/ch/urma/URMA User Guide.ch.md` |
| **URMA 管理面** | `urma/lib/uvs/` | UVS 管理面服务（建链/tp 管理）                                      | `urma/lib/uvs/core/include/{uvs_api.h, uvs_types.h}`                              | 同上                                  |
| **URPC**   | `urpc/`         | 统一远程过程调用，UB 设备间对等函数调用                             | `urpc/include/framework/urpc_framework_api.h`、`urpc/include/umq/umq_api.h`     | `doc/ch/urpc/`                      |
| **DLock**  | `ulock/dlock/`  | 分布式锁（原子锁/读写锁/公平锁）+ 状态对象                          | `ulock/dlock/lib/include/{dlock_client_api.h, dlock_server_api.h, dlock_types.h}` | `doc/ch/ulock/dlock/`               |
| **UMS**    | `usock/ums/`    | 兼容 socket 的内核协议栈（SMC-R 二次开发），透明加速 TCP            | `usock/ums/kmod/`（内核模块）、`usock/ums/ums_agent/`                           | `doc/ch/usock/ums/`                 |
| **CAM**    | `cam/`          | 昇腾 NPU 通信加速（EP/FusedMoE/KVCache），对接 vLLM/SGLang          | `cam/comm_operator/`、`cam/comm_operator/pybind/`                               | `doc/ch/cam/CAM API Guide.ch.md`    |

### 3.3 工具与示例

| 工具          | 路径                            | 用途                                                             |
| ------------- | ------------------------------- | ---------------------------------------------------------------- |
| urma_perftest | `urma/tools/urma_perftest/`   | URMA 时延/带宽测试（8 种命令：read/write/send/atomic × lat/bw） |
| urma_admin    | `urma/tools/urma_admin/`      | 查询/配置 UB 设备属性（eid、feature、队列深度等）                |
| urma_ping     | `urma/tools/urma_ping/`       | 连通性测试                                                       |
| urpc perftest | `urpc/tools/perftest/`        | URPC/UMQ 性能测试                                                |
| urma 示例     | `urma/examples/urma_sample.c` | 最小 client/server 示例                                          |
| cam 示例      | `cam/examples/*.py`           | MoE dispatch/combine、fused_deep_moe 示例                        |

## 4. API 命名约定（推断陌生符号语义的关键）

### 4.1 前缀 = 模块归属

| 前缀       | 模块                      | 层       |
| ---------- | ------------------------- | -------- |
| `urma_`  | URMA 用户态 API           | 用户态库 |
| `uvs_`   | UVS 管理面                | 用户态库 |
| `urpc_`  | URPC 框架                 | 用户态库 |
| `umq_`   | UMQ 消息队列（URPC 底层） | 用户态库 |
| `dlock_` | DLock                     | 用户态库 |

### 4.2 后缀 = 变体语义

| 后缀       | 含义                                                                                     |
| ---------- | ---------------------------------------------------------------------------------------- |
| `_ex`    | 增强版，多一个配置参数（如`urma_import_jetty` vs `urma_import_jetty_ex` 带 tp 配置） |
| `_async` | 异步变体，配 notifier 或回调                                                             |
| `_batch` | 批量操作（如`urma_delete_jfc_batch`）                                                  |
| `_opt`   | 选项设置/获取（`set_*_opt` / `get_*_opt`）                                           |

### 4.3 对称命名（生命周期与配对）

理解配对就能推断另一半的存在和用途：

| 创建侧         | 销毁侧           | 语义                                                                    |
| -------------- | ---------------- | ----------------------------------------------------------------------- |
| `create_*`   | `delete_*`     | 常规创建/销毁（如`create_jetty`/`delete_jetty`）                    |
| `alloc_*`    | `free_*`       | 批量/预分配变体（`alloc_jfc`/`free_jfc`，配合 `active/deactive`） |
| `active_*`   | `deactive_*`   | 激活/去激活（资源就绪后激活才可用）                                     |
| `import_*`   | `unimport_*`   | 导入对端资源到本地（`import_jetty`/`unimport_jetty`）               |
| `advise_*`   | `unadvise_*`   | 通知 UDK 建立 JFS→JFR 通道（IB 传输层需要，UB 天然一对多可省）         |
| `bind_*`     | `unbind_*`     | 绑定本地与对端 jetty（RC 模式建链）                                     |
| `register_*` | `unregister_*` | 注册内存 segment（`register_seg`/`unregister_seg`）                 |

### 4.4 关键词根

| 词根                  | 语义                                                        |
| --------------------- | ----------------------------------------------------------- |
| `jetty`             | 事务层操作对象（"港口"），含 JFS/JFR/JFC/JFCE               |
| `jfs`               | Jetty for Send，发送队列                                    |
| `jfr`               | Jetty for Receive，接收队列                                 |
| `jfc`               | Jetty for Completion，完成队列                              |
| `jfce`              | JFC event，完成事件通道（中断模式）                         |
| `seg` / `segment` | 连续 UBVA 地址空间 + 物理内存映射                           |
| `target_*`          | 导入后的对端资源本地代理（`target_jetty`/`target_seg`） |
| `rjetty` / `rjfr` | remote jetty/jfr，对端 jetty 的描述信息（用于 import）      |
| `tp`                | transport layer，传输层                                     |
| `ctx` / `context` | 上下文句柄                                                  |
| `eid`               | Endpoint ID，16 字节端点标识                                |
| `ubva`              | Unified Bus Virtual Address（EID + VA）                     |
| `token`             | 访问令牌，权限校验                                          |
| `uasid`             | User Assisted Segment ID，进程标识                          |

## 5. 核心概念速查（一句话定义 + 文档锚点）

| 概念                       | 定义                                                         | 文档位置                                      |
| -------------------------- | ------------------------------------------------------------ | --------------------------------------------- |
| **UB**               | Unified Bus，灵衢统一总线（终端+交换机+软件）                | `doc/ch/urma/URMA User Guide.ch.md` §2.1.1 |
| **UBVA**             | UB 虚拟地址（EID+VA），跨节点统一编址                        | §2.1.2                                       |
| **Segment**          | 连续 UBVA 空间 + 物理内存，单边内存访问基础对象              | §2.1.3 / §5.2.3                             |
| **Jetty**            | 事务层操作"港口"，管理收发队列                               | §2.1.4 / §5.2.2                             |
| **JFS/JFR/JFC/JFCE** | 发送/接收/完成/完成事件四种 jetty 变体                       | §5.2.2                                       |
| **UBoE**             | UB over Ethernet，UB 承载在以太网上的报文格式                | §2.1.5                                       |
| **EID**              | Endpoint ID，16 字节端点全局标识                             | §5.2.1                                       |
| **tp**               | transport layer 传输层，建链/数据传输的底层通道              | §5.1                                         |
| **传输模式**         | RC(可靠连接,保序) / RM(可靠消息,多对多) / UM(不可靠)         | §5.3                                         |
| **三平面**           | 管理面(建链管理) / 控制面(jetty/seg 生命周期) / 数据面(传输) | §5                                           |
| **URPC**             | UB 事务层上的对等 RPC，Client/Server/Worker 三角色           | `doc/ch/urpc/Overview.ch.md`                |
| **DLock**            | 基于 URMA 的分布式锁，TCP 管理通道 + URMA 操作通道           | `doc/ch/ulock/dlock/DLock User Guide.ch.md` |
| **UMS**              | 兼容 socket 的内核协议栈，AF_SMC 或 LD_PRELOAD 透明替换      | `doc/ch/usock/ums/UMS User Guide.ch.md`     |
| **CAM**              | 昇腾 NPU 通信加速，EP/FusedMoE/KVCache                       | `doc/ch/cam/CAM API Guide.ch.md`            |

## 6. 通信流程阶段（框定代码属于哪一阶段）

URMA 通信四阶段（见 `doc/ch/urma/URMA User Guide.ch.md` §4），用于判断代码片段属于哪一阶段：

1. **资源准备**：`urma_init` → `urma_create_context` → 创建 JFC/JFS/JFR/Jetty → `urma_register_seg`。
2. **连接建立**：交换 rjetty/rjfr（带外）→ `urma_import_jetty`/`import_jfr` → `urma_bind_jetty`（RC）/`advise_jfr`（IB）。
3. **数据传输**：`urma_post_jetty_send_wr` / `urma_read` / `urma_write` → `urma_poll_jfc` 取完成。
4. **资源释放**：逆序销毁 → `urma_delete_context` → `urma_uninit`。

## 7. 索引文件指引

回答具体问题时按需加载：

- 找某 API 的 file:line 或按模块浏览 API → [api-index.md](api-index.md)
- 查结构体字段/枚举值/宏含义 → [data-structures.md](data-structures.md)
- 查调用链 → [call-graphs.md](call-graphs.md)

## 8. 变更记录

| 日期       | 变更                                                | 采集方式           |
| ---------- | --------------------------------------------------- | ------------------ |
| 2026-07-07 | 初始创建，基于仓库实测采集                          | 手动 + 头文件 grep |
| 2026-07-07 | 范围限定 `umdk/` 目录                                | 手动调整           |
