# Updater Profile：目标 skill 与采集锚点（框架特异）

本文件是 umdk-skill-updater 中**唯一与 UMDK 强绑定**的部分。
移植到其他框架时重写本文件（目标文件清单 + 采集锚点），extraction-patterns.md 原样复用。

**覆盖范围**：`umdk/` 目录下的用户态开发包。采集与索引都限定在 `umdk/` 内。

## 1. 目标 skill 与索引文件清单

updater 维护的对象（写入目标）：

| 目标 skill | 索引文件 | 路径（相对仓库根） |
|------------|----------|---------------------|
| umdk-explainer | api-index | `umdk/skills/umdk-explainer/references/api-index.md` |
| umdk-explainer | data-structures | `umdk/skills/umdk-explainer/references/data-structures.md` |
| umdk-explainer | call-graphs | `umdk/skills/umdk-explainer/references/call-graphs.md` |
| umdk-explainer | profile（模块地图/概念/变更记录） | `umdk/skills/umdk-explainer/references/profile.md` |

> 通用文件（SKILL.md、methodology.md）原则上不写。若确需改方法论，须提示用户人工评审。

## 2. 采集锚点（API 源头）

### 2.1 用户态公开 API 头文件（→ api-index）

| 模块 | 头文件（相对仓库根） | 说明 |
|------|----------------------|------|
| URMA | `umdk/src/urma/lib/urma/core/include/urma_api.h` | 主 API，1177 行，Doxygen 规范 |
| URMA 类型 | `umdk/src/urma/lib/urma/core/include/urma_types.h` | 结构体/枚举 |
| UVS | `umdk/src/urma/lib/uvs/core/include/uvs_api.h` | 管理面 API |
| URPC 框架 | `umdk/src/urpc/include/framework/urpc_framework_api.h` | RPC API |
| UMQ | `umdk/src/urpc/include/umq/umq_api.h`、`umq_pro_api.h` | 消息队列 API |
| DLock | `umdk/src/ulock/dlock/lib/include/dlock_client_api.h`、`dlock_server_api.h` | 锁 API |

### 2.2 工具入口（→ api-index 工具段）

| 工具 | main 文件 |
|------|-----------|
| urma_perftest | `umdk/src/urma/tools/urma_perftest/urma_perftest.c`（参数 usage 在 `perftest_parameters.c`） |
| urma_admin | `umdk/src/urma/tools/urma_admin/` |
| urma_sample | `umdk/src/urma/examples/urma_sample.c` |

### 2.3 文档锚点（→ profile 概念速查）

| 文档 | 路径 |
|------|------|
| URMA 用户指南 | `umdk/doc/ch/urma/URMA User Guide.ch.md` |
| URPC 概览 | `umdk/doc/ch/urpc/Overview.ch.md` |
| DLock 指南 | `umdk/doc/ch/ulock/dlock/DLock User Guide.ch.md` |
| UMS 指南 | `umdk/doc/ch/usock/ums/UMS User Guide.ch.md` |
| CAM 指南 | `umdk/doc/ch/cam/CAM API Guide.ch.md` |

## 3. 各索引文件的标准结构

写入时保持下列结构，便于下次增量比对：

### api-index.md 结构
- 按模块分二级标题（## URMA / ## UVS / ## URPC / ## DLock / ## 工具入口）。
- 每模块下用表格：`| API | 行号链接 | 一句话 |`。
- 顶部固定声明行（采集来源 + 维护说明 + 覆盖范围 `umdk/`）。

### data-structures.md 结构
- 按"URMA 核心结构体 / 关键枚举宏 / 待补充"分节。
- 结构体表格：`| 结构体 | 行号 | 用途 | 关键字段 |`。
- 末尾保留"待补充"段（低频结构体不预填，按需补）。

### call-graphs.md 结构
- 每条链路一个二级标题（## 链路 N：...）。
- 链路用代码块 + 缩进树，每跳标 `file:line` 或注释"待 updater 补全"。
- 末尾"待沉淀"段登记现编候选。

### profile.md 结构
- 固定章节：项目定位 / 分层 / 模块地图 / API 命名约定 / 核心概念速查 / 通信流程阶段 / 索引指引 / 变更记录。
- 只刷新"模块地图""概念速查""变更记录"三节；命名约定与流程阶段相对稳定，改动需人工确认。

## 4. 采集范围控制（避免全量爆炸）

全量刷新成本高，按需触发：

- **api-index**：默认只刷新 URMA 段（最高频）；UVS/URPC/DLock 段在用户提及或 explainer 标记失效时刷新。
- **data-structures**：默认保留现有 + 末尾"待补充"；用户问某结构体时按需补到正文。
- **call-graphs**：只刷新被标记"行号待补全"的链路，或 explainer 现编后登记的候选。
- **profile**：模块地图在目录结构变更时刷新；概念速查在文档变更时刷新；变更记录每次必更。

## 5. 版本基线

- 仓库根：`d:/gitbase/umdk_blue`
- UMDK 版本：26.06.0（见 `umdk/umdk.spec` `Version` 字段）
- 首次采集日期：2026-07-07

代码版本升级后，updater 应先读 `umdk.spec` 确认版本，若变化则在变更记录标注"跨版本刷新"。
