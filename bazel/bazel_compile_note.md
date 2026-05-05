# UMDK Bazel 构建使用手册

本文说明如何在 `umdk` 仓库根目录使用 Bazel，按与 `umdk.spec` 一致的 CMake 选项完成**配置与编译**。

## 1. 前提条件

- 已安装 **Bazel**（与仓库当前开发环境一致即可）。
- 与 **CMake + Make** 直编相同：需本机已具备 `umdk.spec` 中 `BuildRequires` 所述开发依赖（如 `gcc`、`cmake`、`openssl-devel`、`glib2-devel`、`libnl3-devel`、**`kernel-devel`** 等，视你要编译的子集而定）。
- 工作区根目录为 **`umdk/`**（与 `WORKSPACE` 同级），不要只在 `bazel/` 子目录下执行 Bazel 作为工作区根。

## 2. 基本命令（默认：只编译，不执行安装）

在 **`umdk` 仓库根**执行：

```bash
cd /path/to/umdk
bazel build //bazel:umdk_install
```

- **`umdk_cmake` 默认 `run_install = False`**：只做 **`cmake` 配置 + `cmake --build`**，**不会**执行 **`cmake --install`**（等价于只做编译，不做 `make install`）。
- **默认目标** `//bazel:umdk_install`：全量 CMake 选项，对应 `umdk.spec` 中**未**开启 `asan/tsan/...` 等额外 `%with`、且为**完整 `build_all`** 的 RPM 场景。
- **默认产物**为 **`bazel-bin/bazel/<install_dir_name>.tar.gz`**：其中是按目录打包的 **CMake 构建树**（解压后结构与本地 `cmake --build` 生成目录一致）。详见「产物位置」。

### 若需要与 RPM 一致的 DESTDIR 安装树

少数场景需要 **`usr/`、`etc/`、`lib/modules/...`** 这类「安装阶段」布局（等同 `make install DESTDIR=<staging>`），可使用单独目标：

```bash
bazel build //bazel:umdk_install_destdir
```

或在自定义目标里设置 **`run_install = True`**（见第 7 节）。

## 3. 预设目标（与 spec 常见选项对应）

这些目标在 `bazel/BUILD.bazel` 中已配置好，名称即用途。每个目标使用**不同的** `install_dir_name`，对应生成 **`…/<install_dir_name>.tar.gz`**，可并行构建、互不覆盖。**默认均为仅编译、不安装**。

| Bazel 目标 | 说明（与 `rpmbuild --with …` / 子集构建类似） |
|------------|-----------------------------------------------|
| `//bazel:umdk_install` | 全量默认（仅编译） |
| `//bazel:umdk_install_destdir` | 全量 + **`cmake --install`**，生成 DESTDIR 风格目录树 |
| `//bazel:umdk_with_asan` | ASan |
| `//bazel:umdk_with_tsan` | TSan |
| `//bazel:umdk_with_gcov` | 覆盖率 |
| `//bazel:umdk_with_test` | URMA 测试相关（与 spec 中 `URMA_TEST` 等一致） |
| `//bazel:umdk_with_release_enable` | `RELEASE_ENABLE` |
| `//bazel:umdk_only_urma` | 仅 urma 子集 |
| `//bazel:umdk_only_urpc` | 仅 urpc 子集 |
| `//bazel:umdk_only_ums` | 仅 UMS 子集 |
| `//bazel:umdk_only_dlock` | 仅 dlock 子集 |
| `//bazel:umdk_without_udma` | 关闭 UDMA provider |
| `//bazel:umdk_without_udma_st64b` | 关闭 UDMA ST64B 相关选项 |

示例：

```bash
bazel build //bazel:umdk_with_asan
bazel build //bazel:umdk_only_urma
```

可同时编多个预设（不同输出目录，一般不冲突）：

```bash
bazel build //bazel:umdk_install //bazel:umdk_without_udma
```

## 4. 与 `umdk.spec` 的对应关系

- CMake 参数由 Starlark 函数 **`cache_entries_for_spec(...)`**（`bazel/spec_cmake_defaults.bzl`）生成，逻辑对齐 **`umdk.spec` 的 `%build` 段**。
- **选择性构建**：只要指定 `with_urma` / `with_urpc` / `with_ums` / `with_dlock` 中任一类比选项，即等价于 spec 里将 **`build_all` 置为 0** 的那类子集构建，并设置相应的 `BUILD_*`。
- **`run_install`**：spec 里 **`make install DESTDIR=...`** 对应本仓库中的 **`run_install = True`**；默认关闭安装，仅对齐 **`cmake` + `make`** 编译阶段。

## 5. 环境变量（可选）

仓库根 **`umdk/.bazelrc`** 通过 `import` 加载 **`bazel/bazel.rc`**，其中可将下列变量传入动作环境（与 CMake / spec 用法一致时有用）：

| 变量 | 含义 |
|------|------|
| `KERNEL_RELEASE` | 内核版本字符串 |
| `KERNEL_PATH` | 内核构建目录（如 `/lib/modules/<ver>/build`） |
| `BUILD_DATE_OVERRIDE` | 覆盖构建时间戳（等同 spec 里 `BUILD_DATE` 一类用途） |

示例：

```bash
export KERNEL_RELEASE="$(uname -r)"
export KERNEL_PATH="/lib/modules/${KERNEL_RELEASE}/build"
bazel build //bazel:umdk_install
```

也可单行传入：

```bash
KERNEL_RELEASE=$(uname -r) KERNEL_PATH=/lib/modules/$(uname -r)/build \
  bazel build //bazel:umdk_install
```

## 6. 产物位置

- **`run_install = False`（默认）**：产物为单个 **`gzip` 压缩包**，路径：

  `bazel-bin/bazel/<install_dir_name>.tar.gz`

  内容为 **CMake 构建目录**整棵树的归档（含各子目录下的 `.so`、可执行文件、`CMakeFiles`、`CMakeCache.txt` 等），**不是** `DESTDIR` 展开后的 `usr/` 前缀布局。

  解压示例：

  ```bash
  mkdir -p /tmp/umdk_build && tar -xzf bazel-bin/bazel/umdk_install.tar.gz -C /tmp/umdk_build
  find /tmp/umdk_build -name '*.so' | head
  ```

- **`run_install = True`**：产物为 **目录**（不是 `.tar.gz`），路径：

  `bazel-bin/bazel/<install_dir_name>/`

  其内容为 **`cmake --install` + `DESTDIR`**，含 **`usr/`、`etc/`、`lib/modules/...`** 等，与 **`make install DESTDIR=<staging>`** 一致。

  示例：

  ```bash
  ls -la bazel-bin/bazel/umdk_destdir/usr/lib64/
  ```

## 7. 自定义组合（高级）

若预设不满足需求，可在 **`bazel/BUILD.bazel`** 中新增一个 **`umdk_cmake`** 目标：

1. `load("//bazel:spec_cmake_defaults.bzl", "cache_entries_for_spec")`
2. `cache_entries = cache_entries_for_spec(with_asan=True, with_urpc=True, …)`（参数见 `spec_cmake_defaults.bzl` 内注释）
3. **必须**设置与其它目标不同的 **`install_dir_name`**，避免并行或顺序构建时输出文件名冲突（默认扩展名为 **`.tar.gz`**，安装模式为目录名）。
4. 默认不安装（生成 **`<install_dir_name>.tar.gz`**）；若需要 **`cmake --install`**，设置 **`run_install = True`**。

示例（片段）：

```python
umdk_cmake(
    name = "umdk_my_combo",
    cmake_lists = "//src:CMakeLists.txt",
    cache_entries = cache_entries_for_spec(
        with_asan = True,
        with_urpc = True,
    ),
    install_dir_name = "umdk_install_my_combo",
    # run_install = False,  # 默认，可省略
)
```

## 8. 清理与排错

```bash
bazel clean          # 清理构建产物
bazel clean --expunge  # 更深清理（慎用）
```

- 若 CMake 报错缺少头文件或内核路径，请对照 **`umdk.spec` 的 BuildRequires** 与本机内核开发包是否安装完整。
- 本规则在实现上对源码树做了**可写副本**并使用 **`no-sandbox`** 执行外链 CMake，行为与在仓库内直接 `cmake`/`make` 接近；仍应保证磁盘空间充足。

## 9. 相关文件一览

| 文件 | 作用 |
|------|------|
| `WORKSPACE`（仓库根） | Bazel 工作区 |
| `.bazelrc`（仓库根） | 导入 `bazel/bazel.rc` |
| `bazel/bazel.rc` | 常用 `build --action_env=...` |
| `bazel/BUILD.bazel` | 预设 `umdk_cmake` 目标 |
| `bazel/umdk_build.bzl` | `umdk_cmake` 规则实现（含 **`run_install`**） |
| `bazel/spec_cmake_defaults.bzl` | `cache_entries` / `cache_entries_for_spec` |
| `src/BUILD.bazel` | 导出 `//src:CMakeLists.txt` 供规则引用 |

---

如有新的常用组合，可将新的 `umdk_cmake` 目标追加到 `bazel/BUILD.bazel`，便于团队复用。
