# UMDK 公开 API 索引（框架特异，由 umdk-skill-updater 维护）

> 采集自 `umdk/` 目录下真实头文件，行号为实测值。API 变更后由 `umdk-skill-updater` 刷新。
> 权威来源始终是头文件本身；本索引用于快速定位，不替代读头文件。

## URMA 用户态 API（urma_api.h）

头文件：[urma_api.h](umdk/src/urma/lib/urma/core/include/urma_api.h)（1177 行）

### 初始化与设备管理

| API | 行号 | 一句话 |
|-----|------|--------|
| `urma_init` | [L27](umdk/src/urma/lib/urma/core/include/urma_api.h#L27) | 初始化 urma 环境，分配 uasid |
| `urma_uninit` | [L33](umdk/src/urma/lib/urma/core/include/urma_api.h#L33) | 反初始化，释放 uasid |
| `urma_get_device_list` | [L42](umdk/src/urma/lib/urma/core/include/urma_api.h#L42) | 获取设备列表（需配对 free） |
| `urma_free_device_list` | [L50](umdk/src/urma/lib/urma/core/include/urma_api.h#L50) | 释放设备列表 |
| `urma_get_eid_list` | [L59](umdk/src/urma/lib/urma/core/include/urma_api.h#L59) | 获取设备 eid 列表 |
| `urma_free_eid_list` | [L66](umdk/src/urma/lib/urma/core/include/urma_api.h#L66) | 释放 eid 列表 |
| `urma_get_device_by_name` | [L73](umdk/src/urma/lib/urma/core/include/urma_api.h#L73) | 按名查设备 |
| `urma_get_device_by_eid` | [L81](umdk/src/urma/lib/urma/core/include/urma_api.h#L81) | 按 eid + 传输类型查设备 |
| `urma_query_device` | [L89](umdk/src/urma/lib/urma/core/include/urma_api.h#L89) | 查询设备属性/能力 |
| `urma_create_context` | [L97](umdk/src/urma/lib/urma/core/include/urma_api.h#L97) | 在设备上创建上下文 |
| `urma_delete_context` | [L104](umdk/src/urma/lib/urma/core/include/urma_api.h#L104) | 删除上下文 |
| `urma_set_context_opt` | [L111](umdk/src/urma/lib/urma/core/include/urma_api.h#L111) | 设置上下文选项 |

### JFC（完成队列）

| API | 行号 | 一句话 |
|-----|------|--------|
| `urma_create_jfc` / `delete_jfc` | [L120](umdk/src/urma/lib/urma/core/include/urma_api.h#L120) / [L135](umdk/src/urma/lib/urma/core/include/urma_api.h#L135) | 创建/删除完成队列 |
| `urma_alloc_jfc` / `free_jfc` | [L144](umdk/src/urma/lib/urma/core/include/urma_api.h#L144) / [L186](umdk/src/urma/lib/urma/core/include/urma_api.h#L186) | 预分配/释放变体 |
| `urma_active_jfc` / `deactive_jfc` | [L161](umdk/src/urma/lib/urma/core/include/urma_api.h#L161) / [L178](umdk/src/urma/lib/urma/core/include/urma_api.h#L178) | 激活/去激活 |
| `urma_modify_jfc` | [L128](umdk/src/urma/lib/urma/core/include/urma_api.h#L128) | 修改 jfc 属性 |
| `urma_set_jfc_opt` / `get_jfc_opt` | [L154](umdk/src/urma/lib/urma/core/include/urma_api.h#L154) / [L171](umdk/src/urma/lib/urma/core/include/urma_api.h#L171) | 设置/获取选项 |
| `urma_delete_jfc_batch` | [L198](umdk/src/urma/lib/urma/core/include/urma_api.h#L198) | 批量删除 |

### JFS（发送队列）

| API | 行号 | 一句话 |
|-----|------|--------|
| `urma_create_jfs` / `delete_jfs` | [L206](umdk/src/urma/lib/urma/core/include/urma_api.h#L206) / [L230](umdk/src/urma/lib/urma/core/include/urma_api.h#L230) | 创建/删除发送队列 |
| `urma_alloc_jfs` / `free_jfs` | [L262](umdk/src/urma/lib/urma/core/include/urma_api.h#L262) / [L304](umdk/src/urma/lib/urma/core/include/urma_api.h#L304) | 预分配变体 |
| `urma_active_jfs` / `deactive_jfs` | [L279](umdk/src/urma/lib/urma/core/include/urma_api.h#L279) / [L296](umdk/src/urma/lib/urma/core/include/urma_api.h#L296) | 激活/去激活 |
| `urma_modify_jfs` / `query_jfs` | [L214](umdk/src/urma/lib/urma/core/include/urma_api.h#L214) / [L223](umdk/src/urma/lib/urma/core/include/urma_api.h#L223) | 修改/查询属性 |
| `urma_flush_jfs` | [L253](umdk/src/urma/lib/urma/core/include/urma_api.h#L253) | flush 发送队列的 wr |

### JFR（接收队列）

| API | 行号 | 一句话 |
|-----|------|--------|
| `urma_create_jfr` / `delete_jfr` | [L312](umdk/src/urma/lib/urma/core/include/urma_api.h#L312) / [L336](umdk/src/urma/lib/urma/core/include/urma_api.h#L336) | 创建/删除接收队列 |
| `urma_import_jfr` / `unimport_jfr` | [L358](umdk/src/urma/lib/urma/core/include/urma_api.h#L358) / [L379](umdk/src/urma/lib/urma/core/include/urma_api.h#L379) | 导入/注销对端 jfr |
| `urma_import_jfr_ex` | [L371](umdk/src/urma/lib/urma/core/include/urma_api.h#L371) | 增强版导入（带配置） |
| `urma_advise_jfr` / `unadvise_jfr` | [L387](umdk/src/urma/lib/urma/core/include/urma_api.h#L387) / [L409](umdk/src/urma/lib/urma/core/include/urma_api.h#L409) | 建立 JFS→JFR 通道（IB 传输层） |
| `urma_advise_jfr_async` | [L400](umdk/src/urma/lib/urma/core/include/urma_api.h#L400) | 异步 advise |

### Jetty（全功能通信对象）

| API | 行号 | 一句话 |
|-----|------|--------|
| `urma_create_jetty` / `delete_jetty` | [L472](umdk/src/urma/lib/urma/core/include/urma_api.h#L472) / [L496](umdk/src/urma/lib/urma/core/include/urma_api.h#L496) | 创建/删除 jetty |
| `urma_import_jetty` / `unimport_jetty` | [L518](umdk/src/urma/lib/urma/core/include/urma_api.h#L518) / [L539](umdk/src/urma/lib/urma/core/include/urma_api.h#L539) | 导入/注销对端 jetty（不感知传输层） |
| `urma_import_jetty_ex` | [L531](umdk/src/urma/lib/urma/core/include/urma_api.h#L531) | 感知传输层的导入 |
| `urma_bind_jetty` / `unbind_jetty` | [L567](umdk/src/urma/lib/urma/core/include/urma_api.h#L567) / [L586](umdk/src/urma/lib/urma/core/include/urma_api.h#L586) | 绑定/解绑本地与对端 jetty（RC） |
| `urma_bind_jetty_ex` | [L579](umdk/src/urma/lib/urma/core/include/urma_api.h#L579) | 带配置的绑定 |
| `urma_advise_jetty` / `unadvise_jetty` | [L549](umdk/src/urma/lib/urma/core/include/urma_api.h#L549) / [L558](umdk/src/urma/lib/urma/core/include/urma_api.h#L558) | advise jetty |
| `urma_get_rjetty` / `put_rjetty` | [L606](umdk/src/urma/lib/urma/core/include/urma_api.h#L606) / [L612](umdk/src/urma/lib/urma/core/include/urma_api.h#L612) | 获取/释放 remote jetty 描述（带外交换用） |
| `urma_import_jetty_async` 等 | [L624+](umdk/src/urma/lib/urma/core/include/urma_api.h#L624) | 异步 import/bind/unbind 系列 |

### 异步事件 / Token / Segment

| API | 行号 | 一句话 |
|-----|------|--------|
| `urma_get_async_event` / `ack_async_event` | [L776](umdk/src/urma/lib/urma/core/include/urma_api.h#L776) / [L783](umdk/src/urma/lib/urma/core/include/urma_api.h#L783) | 获取/确认异步异常事件 |
| `urma_create_jfce` / `delete_jfce` | [L761](umdk/src/urma/lib/urma/core/include/urma_api.h#L761) / [L768](umdk/src/urma/lib/urma/core/include/urma_api.h#L768) | 创建/删除完成事件通道（中断模式） |
| `urma_create_notifier` / `delete_notifier` | [L659](umdk/src/urma/lib/urma/core/include/urma_api.h#L659) / [L666](umdk/src/urma/lib/urma/core/include/urma_api.h#L666) | 异步通知器 |
| `urma_alloc_token_id` / `free_token_id` | [L790](umdk/src/urma/lib/urma/core/include/urma_api.h#L790) / [L807](umdk/src/urma/lib/urma/core/include/urma_api.h#L807) | 分配/释放 token id |
| `urma_register_seg` / `unregister_seg` | [L816](umdk/src/urma/lib/urma/core/include/urma_api.h#L816) / [L823](umdk/src/urma/lib/urma/core/include/urma_api.h#L823) | 注册/注销本地内存 segment |
| `urma_import_seg` / `unimport_seg` | [L834](umdk/src/urma/lib/urma/core/include/urma_api.h#L834) / [L842](umdk/src/urma/lib/urma/core/include/urma_api.h#L842) | 导入/注销对端 segment（单边访问前置） |

### 数据面（WR 提交与完成轮询）

> 以下在 urma_api.h 843 行之后。提交 WR、poll 完成队列、read/write/send/atomic 的便捷封装。
> 详细签名以头文件为准。常用入口：`urma_post_jetty_send_wr`、`urma_post_jfs_wr`、`urma_post_jfr_wr`、
> `urma_read`、`urma_write`、`urma_send`、`urma_recv`、`urma_poll_jfc`、`urma_wait_jfc`。

## UVS 管理面 API

头文件：[uvs_api.h](umdk/src/urma/lib/uvs/core/include/uvs_api.h)、[uvs_types.h](umdk/src/urma/lib/uvs/core/include/uvs_types.h)
用途：建链/tp 生命周期管理（分布式/集中式管控）。需补充时由 updater 采集。

## URPC 框架 API

头文件：[urpc_framework_api.h](umdk/src/urpc/include/framework/urpc_framework_api.h)、[urpc_framework_types.h](umdk/src/urpc/include/framework/urpc_framework_types.h)
底层 UMQ：[umq_api.h](umdk/src/urpc/include/umq/umq_api.h)、[umq_pro_api.h](umdk/src/urpc/include/umq/umq_pro_api.h)
角色：Client / Server / Worker；消息类型 URPC Request / Ack / Response。需补充时由 updater 采集。

## DLock API

头文件：[dlock_client_api.h](umdk/src/ulock/dlock/lib/include/dlock_client_api.h)、[dlock_server_api.h](umdk/src/ulock/dlock/lib/include/dlock_server_api.h)、[dlock_types.h](umdk/src/ulock/dlock/lib/include/dlock_types.h)
锁类型：原子锁 / 读写锁 / 公平锁 + 分布式状态对象。需补充时由 updater 采集。

## 工具入口

| 工具 | main | 说明 |
|------|------|------|
| urma_perftest | [urma_perftest.c](umdk/src/urma/tools/urma_perftest/urma_perftest.c) | 8 命令：read/write/send/atomic × lat/bw；参数见 [perftest_parameters.c](umdk/src/urma/tools/urma_perftest/perftest_parameters.c) usage() |
| urma_admin | `urma/tools/urma_admin/` | 设备属性查询配置 |
| urma_sample | [urma_sample.c](umdk/src/urma/examples/urma_sample.c) | 最小 client/server 示例 |
