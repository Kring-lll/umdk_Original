# URMA 版本规约

当前版本为 0.1.0。版本值和编码直接定义于现有的 urma_types.h，
urma_api.h、urma_provider.h 和 urma_cmd.h 通过包含关系使用这些定义。
调用方自行编写版本要求检查，不再生成或安装额外的版本头文件。对外提供
URMA_API_VERSION_MAJOR、URMA_API_VERSION_MINOR、URMA_API_VERSION_PATCH、
URMA_API_VERSION_NUMBER 和 URMA_GET_VERSION(major, minor, patch)。
版本采用无符号 64 位编码，主版本、次版本和补丁版本分别占 32、16、16 位。

## 源码兼容性

头文件仅提供版本信息，不规定调用方所需的最低版本或主版本。
调用方在自己的源码中包含头文件后编写检查，例如：

    #include <urma_api.h>
    #if URMA_API_VERSION_MAJOR != 0
    #error "URMA API major 0 is required"
    #endif
    #if URMA_API_VERSION_NUMBER < URMA_GET_VERSION(0, 1, 0)
    #error "URMA API >= 0.1.0 is required"
    #endif

将示例最低版本改成 0.1.1 后，在当前头文件下由调用方的 #error 触发编译失败。
编译器仍会执行类型检查；库内部只保留版本字段自身的范围校验。

## API 与 ABI 版本

urma_types.h 同时提供 URMA_API_VERSION_* 和 URMA_ABI_VERSION_*，
两者当前均为 0.1.0，采用相同的无符号 64 位编码格式，但版本数值独立维护。
各组提供 MAJOR、MINOR、PATCH、STRING、NUMBER 和版本别名，统一使用 URMA_GET_VERSION(major, minor, patch) 编码。

URMA_ABI_VERSION_MAJOR=0、URMA_ABI_VERSION_MINOR=1 对应 liburma.map
中的 URMA_0.1 符号节点；URMA_PRIVATE_0.1 为内部使用的导出符号节点，
名称中的 PRIVATE 不代表这些符号被隐藏。未列出的符号才由 local: * 隐藏。
补丁版本不参与当前符号节点命名。以后 API 与 ABI 不要求同步递增。

ABI 宏是头文件提供的信息，不会代替 SONAME、符号版本或动态链接器检查。
ABI 演进需同步审核符号脚本和 SONAME，并保留需要兼容的历史符号节点。

## 二进制兼容性

liburma.so.0.1.0 的 SONAME 为 liburma.so.0，公开符号归属于 URMA_0.1 节点，内部使用的导出符号归属于
URMA_PRIVATE_0.1 节点。新链接的调用方会为引用的符号记录该节点。
兼容版本必须保留已有节点；不兼容的 ABI 变更需要升级 SONAME 主版本。
当前节点使用显式源码符号基线，发布前必须与实际发布库的 ELF 符号表核对。

找不到所需 SONAME 对应的库，或所需符号版本不可用时，由动态链接器报错。
采用延迟绑定时，缺失函数符号可能到首次调用才报错。
调用方需要在链接可执行文件时添加 -Wl,-z,now，或在运行时设置
LD_BIND_NOW=1，以要求启动时完成符号解析。
动态库自身的 NOW 标志不会强制可执行文件的所有重定位立即绑定。
使用 dlopen 的调用方应指定 RTLD_NOW。

符号版本机制不会追溯修改旧的、未记录符号版本的二进制文件。
它既不能证明类型和内存布局兼容，也不能拒绝所有不带符号版本的替换库：
动态链接器可能允许回退到无版本符号。

liburma 中的 .note.urma.version 是维测元信息，
不会自动触发加载器兼容性检查。

在 Linux 环境完成构建后，可执行以下命令验证：

    readelf -d liburma.so.0.1.0
    readelf --version-info liburma.so.0.1.0
    readelf --dyn-syms --wide liburma.so.0.1.0
    readelf -n liburma.so.0.1.0

预期 SONAME 为 liburma.so.0，符号版本节点为 URMA_0.1。
URMA note 的描述数据为 16 字节，依次存放 API、ABI 两个本机字节序 uint64_t，当前均为 0x0000000000010000（0.1.0）。

## 修改位置、目的和影响

以下路径相对于 umdk。保留当前提交历史，在现有代码上调整版本实现。

| 位置 | 改动与用途 |
|---|---|
| src/urma/lib/urma/core/include/urma_types.h | 直接定义 API/ABI 0/1/0 版本字段、64 位编码和字符串，校验版本字段范围 |
| 同目录 urma_api.h、urma_provider.h、urma_cmd.h | 通过包含关系提供版本值，不在库内执行调用方的版本要求检查 |
| src/urma/lib/urma/core/CMakeLists.txt | 设置 liburma 的 API/ABI 版本 0.1.0，启用 ASM 并生成双版本 note |
| src/urma/lib/urma/core/urma_version_note.S.in | note 模板放在 liburma core 目录，依次写入 API、ABI 编码值，当前均为 65536 |
| src/urma/lib/urma/core/CMakeLists.txt | 链接 note 和版本脚本；移除生成版本头的包含和安装规则 |
| src/urma/common/CMakeLists.txt | 恢复原有配置，不向 liburma_common 添加 note 或修改其版本 |
| src/urma/lib/uvs/core/CMakeLists.txt | 恢复原版本 0.0.3、主版本 0 |
| src/urma/lib/urma/bond/CMakeLists.txt | 保留原版本 0.0.3、主版本 0 |
| src/urma/lib/urma/core/liburma.map | 保留 URMA_0.1 和 URMA_PRIVATE_0.1 显式符号基线 |
| src/urma/BUILD.bazel | 仅生成 note；通过现有 soname 参数设置 liburma.so.0 |
| src/urma/urma_bazel.sh | 按统一库版本打包，只安装现有公共头文件 |

不修改 umdk/src/CMakeLists.txt 和 src/urma/CMakeLists.txt。升级 liburma 版本时，
API/ABI 版本在 urma_types.h 中维护，并同步 core/CMakeLists.txt 中的 URMA_API_VERSION/URMA_ABI_VERSION；
common、bond 和 UVS 保持各自原有版本，不随 liburma 升级。
变更 SONAME 主版本时，还需同步 BUILD.bazel 中的 soname 参数。
模板形式的独立版本头已删除，使用源码头文件不再依赖构建时生成头文件。

此次调整不修改实际接口签名、结构体布局或业务语义。
0.1.0 基线不能直接视为对依赖 liburma.so.1、URMA_1.0 的旧程序兼容，
这类程序通常需要重新链接。内核 UBCORE KAPI/ABI 此前仍为 1.0.0，
本轮不修改内核版本。

## 构建、使用和验证

CMake 和 Bazel 是可选构建路径。以下命令用于依赖已配置好的 Linux 环境，
无需 .vscode/settings.json。从 umdk 根目录使用 CMake：

    cmake -S src -B build/urma-version -DBUILD_ALL=disable -DBUILD_URMA=enable
    cmake --build build/urma-version --parallel
    DESTDIR="$PWD/build/urma-stage" cmake --install build/urma-version

note 生成于 build/urma-version/urma/lib/urma/core/urma_version_note.S。
liburma 输出位于 build/urma-version/urma/lib/urma/core/liburma.so.0.1.0。
安装规则使用 /usr/lib64 和 /usr/include/ub/umdk/urma 等绝对路径，
DESTDIR 用于增加暂存目录前缀。

也可以在 src/urma 目录使用 Bazel：

    bazel build //:urma //:urma_common

liburma 的 note 生成于 bazel-bin/version。
库输出为 bazel-bin/liburma.so 等名称，内部 SONAME 为 liburma.so.0。
需要打包时运行：

    bash urma_bazel.sh compile --config=release

辅助脚本可能安装缺失依赖并获取 libummu 源码；其 install 操作会写入
系统目录、刷新 ldconfig，并在可用时重启 rsyslog。

应用包含现有的 urma_api.h 后，在自身源码中按前文示例检查版本：

    cc app.c -I/usr/include/ub/umdk/urma -L/usr/lib64 -Wl,-z,now -lurma -o app

调用方将示例中的最低版本改为 URMA_GET_VERSION(0, 1, 1)，
应由调用方代码触发编译错误。

发布前执行前文的 readelf 检查，确认 SONAME 为 liburma.so.0、
符号节点为 URMA_0.1、note 描述数据为 16 字节，API/ABI 编码值均为 65536。
读取旧版单版本 note 的工具需按描述长度区分格式。
还需运行未修改的旧版本二进制和使用当前头文件的新程序。

本地可执行静态一致性检查；完整 CMake/Bazel 构建、
实际 ELF 检查和动态加载测试仍需目标 Linux 环境完成。
