# URMA DT 目录与运行说明

## 目标

URMA 仿真 DT 门禁统一放在 `test/urma/dt` 下。当前提交只提供框架、最小录制样例和运行脚本，不提交 simulator 源码；后续 simulator 可直接补到 `test/urma/dt/simulator`，也可通过 `UMDK_DT_SIMULATOR_DIR` 指向外部目录。

## 目录结构

```text
test/urma/dt/
  fixture/          # C++ 公共测试支撑代码
  recorded/
    manifest.conf   # 门禁用例清单
    test_read/
      source.cpp    # 用于录制的原始 C++ 样例
      case.cpp      # 生成后的 gtest 样例
      expected.txt  # 录制得到的 expected 基准
  scripts/          # DT 入口脚本
  docs/
```

当前不包含 `test/urma/dt/simulator`。在 simulator 合入前，直接运行 `run_dt.sh` 会提示缺少 simulator；这不影响后续同事把 simulator 源码补到该目录。

## 运行方式

在仓库根目录执行：

```bash
bash test/urma/dt/scripts/run_dt.sh
```

默认用例清单：

```text
test/urma/dt/recorded/manifest.conf
```

当前最小 smoke 样例：

```text
test_read|test/urma/dt/recorded/test_read/case.cpp|
```

## 关键环境变量

| 变量 | 默认值 | 说明 |
| --- | --- | --- |
| `UMDK_DT_CASES` | `test/urma/dt/recorded/manifest.conf` | 用例清单。 |
| `UMDK_DT_SIMULATOR_DIR` | `test/urma/dt/simulator` | simulator 源码目录。 |
| `UMDK_DT_SIM_MODE` | `provider` | simulator 模式；也支持 `hw`。 |
| `UMDK_DT_BUILD_ROOT` | `build/dt` | DT 构建产物目录。 |
| `UMDK_DT_SAVE_LOGS` | `0` | 是否保留日志文件。 |
| `UMDK_DT_PRINT_LOGS` | `1` | 是否打印样例 stdout。 |
| `UMDK_DT_TIMEOUT` | `30` | 单样例超时时间，单位秒。 |
| `UMDK_DT_FIXTURE_DIR` | `test/urma/dt/fixture` | fixture 目录。 |
| `SIM_SO` | `build/dt/simulator/liburma_sim.so` | 运行时注入的 simulator so。 |
| `SIM_CFG` | `<simulator_dir>/urma_sim_config.json` | simulator 配置文件。 |

常用示例：

```bash
UMDK_DT_SAVE_LOGS=1 bash test/urma/dt/scripts/run_dt.sh
UMDK_DT_PRINT_LOGS=0 bash test/urma/dt/scripts/run_dt.sh
UMDK_DT_CASES=/tmp/my_dt_cases.conf bash test/urma/dt/scripts/run_dt.sh
UMDK_DT_SIMULATOR_DIR=/tmp/simulator bash test/urma/dt/scripts/run_dt.sh
```

## 框架约定

- `fixture` 封装 URMA 初始化、建链、注册内存、post、poll 和资源释放。
- `recorded/<case>/` 按用例聚合录制前源码、生成后的 gtest 和 expected 基准。
- `run_dt.sh` 负责编译 no-asan `liburma`、构建 simulator、遍历 manifest。
- `run_dt_one.sh` 负责编译并运行单个 gtest，运行时通过 `LD_PRELOAD="$SIM_SO"` 注入 simulator。
- 默认打印样例 stdout；stderr 默认不打印，只在 `UMDK_DT_SAVE_LOGS=1` 时保留用于排查。
