# URMA DT 目录与运行说明

## 目标

URMA 仿真 DT 门禁统一放在 `test/urma/dt` 下，生成的录制样例、运行脚本和说明文档分开管理，避免后续样例增加后和执行框架混在一起。

## 目录结构

```text
test/urma/dt/
  fixture/
    bl_fixture.cpp
    bl_fixture.hpp
    baseline_macros.h
  recorded/
    cases/          # 生成出来的 gtest 样例源码
    sources/        # 用于录制的原始样例源码
    expected/       # 录制得到的 expected 基准
    manifest.conf   # 用例清单
  scripts/
    run_dt.sh
    run_dt_one.sh
  docs/
    dt_layout_plan.md
```

## 当前仿真层位置

当前分支只提交 DT 框架，不提交 simulator 源码。runner 默认查找 `test/urma/dt/simulator`，也可以通过 `UMDK_DT_SIMULATOR_DIR` 指向外部 simulator 源码目录。

在 simulator 内容由后续提交补齐前，直接运行 `run_dt.sh` 会在 simulator 检查阶段失败，并提示提供 `UMDK_DT_SIMULATOR_DIR` 或补充 `test/urma/dt/simulator`。

## 命名约定

- `recorded`：保存录制后生成的、可回放的测试样例。
- `recorded/cases`：保存具体的 gtest 样例源码。
- `recorded/sources`：保存用于录制的原始 C++ 样例源码。
- `recorded/expected`：保存录制得到的 expected 基准。
- `recorded/manifest.conf`：保存门禁要运行的样例清单。
- `fixture`：保存生成样例运行依赖的 C++ 公共测试支撑代码，例如 URMA 初始化、建链、注册内存、post、poll 和资源释放封装。
- `simulator`：DT 使用的 URMA 仿真运行时目录。本框架只预留入口，源码由后续提交补充或通过 `UMDK_DT_SIMULATOR_DIR` 外部指定。
- `scripts`：保存 DT 门禁入口脚本。
- `docs`：保存 DT 框架说明、方案和问题记录。

## 运行命令

在仓库根目录执行：

```bash
cd /path/to/umdk
bash test/urma/dt/scripts/run_dt.sh
```

当前最小门禁默认只运行一个 smoke 样例：

```text
test_read|test/urma/dt/recorded/cases/test_read.cpp|
```

## `run_dt.sh` 参数说明

`run_dt.sh` 当前只支持 `--help`，运行行为通过环境变量配置。

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `UMDK_DT_CASES` | `test/urma/dt/recorded/manifest.conf` | 指定用例清单文件。 |
| `UMDK_DT_SIM_MODE` | `provider` | 仿真模式。`provider` 使用当前可在无硬件环境跑通的 sim provider；`hw` 设置 `URMA_SIM_HW=1`，面向真实 UDMA provider 路径。 |
| `UMDK_DT_BUILD_ROOT` | `build/dt` | DT 编译产物目录，用于保存临时二进制和可选日志。 |
| `UMDK_DT_SAVE_LOGS` | `0` | 是否保存日志文件。`0` 表示默认不保存；`1` 表示保存到 `UMDK_DT_LOG_DIR` 或 `build/dt/logs`。 |
| `UMDK_DT_LOG_DIR` | `build/dt/logs` | `UMDK_DT_SAVE_LOGS=1` 时的日志保存目录。 |
| `UMDK_DT_PRINT_LOGS` | `1` | 是否把每个样例的 stdout 打到控制台。stderr 默认不打到控制台。 |
| `UMDK_DT_LOG_TAIL_LINES` | `0` | 控制 stdout 打印行数。`0` 表示打印完整 stdout；非 0 表示只打印末尾 N 行。构建失败时 gate 脚本默认打印失败日志末尾 120 行。 |
| `UMDK_DT_TIMEOUT` | `30` | 单个样例运行超时时间，单位为秒。 |
| `UMDK_DT_WORK_ROOT` | `/tmp/umdk_dt_work` | 单个样例运行时工作目录的父目录，用于 fake sysfs 和 IPC 隔离。 |
| `UMDK_DT_FIXTURE_DIR` | `test/urma/dt/fixture` | fixture 源码和头文件目录。 |
| `UMDK_DT_SIMULATOR_DIR` | `test/urma/dt/simulator` | simulator 源码目录。 |
| `UA_CORE` | `src/build_noasan/urma/lib/urma/core` | `liburma.so` 所在目录。 |
| `UA_COMMON` | `src/build_noasan/urma/common` | `liburma_common.so` 所在目录。 |
| `SIM_BUILD` | `build/dt/simulator` | simulator 编译目录。 |
| `SIM_SO` | `build/dt/simulator/liburma_sim.so` | 运行时通过 `LD_PRELOAD` 注入的 simulator so。 |
| `SIM_CFG` | `test/urma/dt/simulator/urma_sim_config.json` | 单样例运行时使用的 simulator 配置文件。 |

## 常用示例

默认运行，控制台打印样例 stdout，不保存日志文件：

```bash
bash test/urma/dt/scripts/run_dt.sh
```

保存日志到 `build/dt/logs`：

```bash
UMDK_DT_SAVE_LOGS=1 bash test/urma/dt/scripts/run_dt.sh
```

关闭样例 stdout 打印：

```bash
UMDK_DT_PRINT_LOGS=0 bash test/urma/dt/scripts/run_dt.sh
```

只打印样例 stdout 末尾 120 行：

```bash
UMDK_DT_LOG_TAIL_LINES=120 bash test/urma/dt/scripts/run_dt.sh
```

指定其他用例清单：

```bash
UMDK_DT_CASES=/tmp/my_dt_cases.conf bash test/urma/dt/scripts/run_dt.sh
```

尝试真实 UDMA provider 路径：

```bash
UMDK_DT_SIM_MODE=hw bash test/urma/dt/scripts/run_dt.sh
```

当前环境下 `hw` 模式预计会失败为 `INFRA_FAIL`，因为本机没有可用的真实 `urma-udma.so` provider。

## 日志行为

默认情况下，runner 会把样例的 stdout 打到控制台，但不会保留 `test_read.stdout`、`test_read.stderr`、`test_read.xml` 等文件。

设置 `UMDK_DT_SAVE_LOGS=1` 后，会保留以下文件：

```text
build/dt/logs/
  summary.status
  test_read.compile.log
  test_read.status
  test_read.stdout
  test_read.stderr
  test_read.xml
```

stderr 默认不打印到控制台，主要用于保存后排查仿真层、fixture 或基础设施问题。

## 如何证明跑的是 simulator

脚本层面，`run_dt.sh` 会构建 `test/urma/dt/simulator` 或 `UMDK_DT_SIMULATOR_DIR` 指定的 simulator，产物默认放在 `build/dt/simulator/liburma_sim.so`。单样例运行时，`run_dt_one.sh` 会通过 `LD_PRELOAD="$SIM_SO"` 注入该 so，同时设置：

```text
URMA_SIM_CONFIG=<simulator_dir>/urma_sim_config.json
URMA_SIM_HW=<mode>
URMA_SIM_WORKDIR=<per-case workdir>
URMA_SIM_IPC_DIR=<per-case workdir>/ipc
```

此外，`run_dt.sh` 会在 no-asan `liburma.so` 的同级 provider 目录下准备一个指向 `liburma_sim.so` 的 shim：

```text
src/build_noasan/urma/lib/urma/core/urma/liburma_sim.so -> build/dt/simulator/liburma_sim.so
```

这是为了满足 `urma_init()` 中 `urma_open_drivers()` 对 provider 目录的扫描要求；实际运行时仍由 `LD_PRELOAD` 注入的 simulator constructor 完成仿真能力初始化。

可以用以下命令保存日志并验证动态加载器实际加载了 simulator：

```bash
UMDK_DT_SAVE_LOGS=1 UMDK_DT_PRINT_LOGS=0 \
  bash test/urma/dt/scripts/run_dt_one.sh \
  test_read test/urma/dt/recorded/cases/test_read.cpp LD_DEBUG=libs

grep -n "liburma_sim\\|DBG ioctl\\|num_devs" build/dt/logs/test_read.stderr
```

在 simulator 实现支持对应日志时，预期能看到类似内容：

```text
calling init: .../build/dt/simulator/liburma_sim.so
DBG ioctl: cmd=40 args_len=832
[setup] num_devs=4
```

其中 `calling init: .../liburma_sim.so` 证明运行时注入了 simulator；`DBG ioctl` 和 `num_devs=4` 来自 simulator 对 ioctl/sysfs 设备发现路径的拦截与回填。
