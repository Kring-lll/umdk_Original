# UMDK 调用图 Profile（框架特异内容）

本文件是 umdk-callgraph 中**唯一与 UMDK 强绑定**的部分。
移植到其他代码库时重写本文件（覆盖范围 + 复用来源 + 调用边界规则 + 框架特异 ops/宏清单），
methodology.md 与 output-format.md 原样复用。

> 维护者：本文件由 `umdk-skill-updater` 维护（随 umdk-explainer 的 profile 一起刷新）。
> 采集基准：仓库根 `d:/gitbase/umdk_blue`，范围限定 `umdk/` 目录。

## 1. 覆盖范围

`umdk/` 目录下的用户态代码：`src/{urma,urpc,ulock/dlock,usock/ums,cam}`、
`src/urma/tools/`、`src/urma/examples/`、`src/urpc/tools/`、`src/cam/examples/`。
其中 `umdk/src/urma/hw/udma/` 的 ops 表实际实现也在此范围内，可展开。

调用图遇到 `umdk/` 内看不到实现的下层时止步并标注：

| 边界形态 | 处理 |
|----------|------|
| `ioctl(fd, CMD, ...)` | 止于 umdk 侧调用点，标 `↓ 下层不在 umdk 内` |
| `ops->xxx()` 且实现注册点不在 umdk | 标 `→ 实现不在 umdk 内，止步` |
| `ops->xxx()` 且实现注册于 umdk（如 `hw/udma/`、`bond/`） | **可展开**，追到实现函数 |
| 系统调用（read/write/mmap 等） | 标 `（系统调用，不展开）` |

## 2. 功能解释复用来源

为图中节点取一句话解释时，按以下优先级复用已有 skill 成果，避免重复采集：

1. **umdk-explainer 的 [api-index.md](../umdk-explainer/references/api-index.md)**：公开 API 已有
   一句话说明 + 行号链接。目标函数或其 callees 若在 api-index 中，直接引用其说明。
2. **umdk-explainer 的 [data-structures.md](../umdk-explainer/references/data-structures.md)**：
   涉及结构体字段的解释。
3. **umdk-explainer 的 [profile.md](../umdk-explainer/references/profile.md) §4 API 命名约定**：
   对未见过的函数，按前缀(模块)/后缀(变体)/对称命名/词根推断语义，标 `（按命名推断）`。
4. **Doxygen 注释**：函数声明上方 `/** */`，取首行。
5. **函数体推断**：读入口校验+主逻辑，标 `（按实现推断）`。

复用时无需重新采集，直接写"（见 [api-index](../umdk-explainer/references/api-index.md)）"。

## 3. 调用边界规则（UMDK 特异）

UMDK 代码有大量通过 `urma_ops_t` 函数指针表分发的调用（provider 多态）。识别与处理：

### 3.1 ops 表识别

- ops 表类型：`urma_ops_t`（定义于 [urma_provider.h:60](umdk/src/urma/lib/urma/core/include/urma_provider.h#L60)，含 create_jfc/create_jetty/create_jfs/... 一系列字段）。
- 调用形态：`ops->create_jetty(...)`、`ops->delete_jfc(...)` 等。
- 注册形态：`.create_jetty = xxx`（C99 designated initializer）或 `ops.create_jetty = xxx`。

### 3.2 ops 注册点（实现函数定位）

已知注册点（按需扩展，updater 可补充）：

| ops 表 | 注册文件 | 说明 |
|--------|----------|------|
| udma 用户态 ops | `umdk/src/urma/hw/udma/udma_u_ops.c` | udma 硬件的用户态 ops（在 umdk 内，可展开） |
| bond provider ops | `umdk/src/urma/lib/urma/bond/bondp_provider_ops.c` | bond 聚合设备 ops（在 umdk 内，可展开） |

追 ops 实现时：grep `.字段名 =` 找注册点 → 若注册文件在 `umdk/` 下，展开实现函数；若不在 `umdk/`，止步标注。

### 3.3 常见分发/校验宏

UMDK 用宏封装调用与校验，直接 grep 函数名会漏。遇到这些宏按规则处理：

| 宏 | 含义 | 处理 |
|----|------|------|
| `URMA_CHECK_OP_INVALID_RETURN_POINTER` | 检查 ops 字段是否注册，未注册返回 NULL | 标 `（宏展开：ops 字段检查）`，内部含 `ops->xxx` 调用按 §3.2 展开 |
| `URMA_CHECK_OP_INVALID_RETURN` | 同上，返回错误码 | 同上 |
| `URMA_LOG_ERR` / `URMA_LOG_ERR_RL` | 日志打印（[urma_log.h:104](umdk/src/urma/lib/urma/core/include/urma_log.h#L104)） | 标 `（日志，不展开）` |
| `URMA_LOG_*` 系列 | 各级别日志 | 同上，不展开 |
| `atomic_fetch_add/sub` | 引用计数（标准库） | 标 `（标准库，不展开）` |

> 宏定义可能随版本变化，updater 刷新时应重新 grep 确认。若遇到未见过的 `URMA_*` 大写宏，
> 先 grep 其 `#define` 看是否含函数调用，再决定展开与否。

### 3.4 入口函数（无 caller）

以下属入口函数，无 callers（或仅被外部框架/进程调用）：

- `main`（工具入口：urma_perftest、urma_admin、urma_sample 等）
- 库初始化入口（`urma_init`、`uvs_*` 等，被应用进程直接调用）
- 回调函数（注册给内核/上层框架，由其回调）

入口函数的 callers 区标"入口函数，无 umdk 内 callers（被应用/外部框架调用）"。

## 4. 模块内调用约定

不同模块的调用图特点：

| 模块 | 调用图特点 |
|------|-----------|
| URMA `lib/urma/core/` | 大量 ops 分发 + 配置校验链（check_* 系列函数）；用户态 API 多是"校验+分发+收尾"三段式 |
| URMA `lib/urma/bond/` | bond 聚合层，转发到成员设备的 ops；多路径/故障恢复逻辑 |
| URMA `hw/udma/` | ops 实现，向下经 ioctl 提交（ioctl 之后不在 umdk 内） |
| URPC `framework/` | channel/queue 协作，控制面（allocator/crypto/dfx）+ 数据面 |
| DLock `lib/` | client/server 两态，TCP 管理通道 + URMA 操作通道 |
| UMS `kmod/` | socket 兼容层模块源码（在 umdk 下） |
| CAM `comm_operator/` | Python/pybind，调用图按 Python 模块组织（C 扩展部分按 C 规则） |

CAM 是 Python 为主，其调用图用 Python 的 import/调用关系，不适用 C 的 grep 函数签名法。
处理 CAM 时改用 Python 的 ast/grep 模块调用。

## 5. 与 explainer/updater 的协作

- **功能解释**：复用 explainer 的 api-index/data-structures，不重复采集。
- **调用链沉淀**：本 skill 现编的高频 callee 子链，提示 updater 沉淀进
  [call-graphs.md](../umdk-explainer/references/call-graphs.md)（explainer 的调用链索引），
  两个 skill 共享该索引。
- **行号失效**：本 skill 依赖 api-index 的行号；若失效，提示调 updater 刷新。
- **覆盖范围**：与 explainer 完全一致（`umdk/` 目录），边界处理规则也一致。

## 6. 变更记录

| 日期 | 变更 | 采集方式 |
|------|------|----------|
| 2026-07-07 | 初始创建，含 ops 表/宏清单与真实示例数据 | 头文件 grep + 实测 |
