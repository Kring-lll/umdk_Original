# URMA 版本规约

当前版本为 0.1.0。版本值和编码直接定义于现有的 urma_types.h，
urma_api.h、urma_provider.h 和 urma_cmd.h 通过包含关系使用这些定义。
各入口保留版本要求检查，不再生成或安装额外的版本头文件。对外提供
URMA_API_VERSION_MAJOR、URMA_API_VERSION_MINOR、URMA_API_VERSION_PATCH、
URMA_API_VERSION_NUMBER 和 URMA_API_VERSION_ENCODE(major, minor, patch)。
版本采用无符号 64 位编码，主版本、次版本和补丁版本分别占 32、16、16 位。

## 源码兼容性

调用方可在首次包含头文件之前声明版本要求：

    #define URMA_API_REQUIRED_MAJOR 0
    #define URMA_API_MIN_VERSION URMA_API_VERSION_ENCODE(0, 1, 0)
    #include <urma_api.h>

主版本不匹配或版本低于要求时，通过 #error 触发编译失败。
编译器仍会执行类型检查。调用方也可以自行编写检查：

    #include <urma_api.h>
    #if URMA_API_VERSION < URMA_API_VERSION_ENCODE(3, 2, 0)
    #error "URMA API >= 3.2 is required"
    #endif

第二个示例在当前 0.1.0 版本下会按预期编译失败。

## 二进制兼容性

liburma.so.0.1.0 的 SONAME 为 liburma.so.0，现有导出符号归属于稳定的
URMA_0.1 符号版本节点。新链接的调用方会为引用的符号记录该节点。
兼容版本必须保留已有节点；不兼容的 ABI 变更需要升级 SONAME 主版本。
当前节点使用显式源码符号基线，发布前必须与实际发布库的 ELF 符号表核对。

找不到所需 SONAME 对应的库，或所需符号版本不可用时，由动态链接器报错。
采用延迟绑定时，缺失函数符号可能到首次调用才报错。
通过 CMake 的 urma 目标链接的调用方会继承 -Wl,-z,now；
其他调用方需要在链接可执行文件时添加 -Wl,-z,now，或在运行时设置
LD_BIND_NOW=1，以要求启动时完成符号解析。
动态库自身的 NOW 标志不会强制可执行文件的所有重定位立即绑定。
使用 dlopen 的调用方应指定 RTLD_NOW。

符号版本机制不会追溯修改旧的、未记录符号版本的二进制文件。
它既不能证明类型和内存布局兼容，也不能拒绝所有不带符号版本的替换库：
动态链接器可能允许回退到无版本符号。

liburma 和 liburma_common 中的 .note.urma.version 是维测元信息，
不会自动触发加载器兼容性检查。

在 Linux 环境完成构建后，可执行以下命令验证：

    readelf -d liburma.so.0.1.0
    readelf --version-info liburma.so.0.1.0
    readelf --dyn-syms --wide liburma.so.0.1.0
    readelf -n liburma.so.0.1.0

预期 SONAME 为 liburma.so.0，符号版本节点为 URMA_0.1。
URMA note 的描述数据为 8 字节，采用本机字节序，值为 0x0000000000010000。

## 接口演进中的后向兼容

只安装当前版本的公共头文件。历史结构体布局和适配代码放在
lib/urma/core/compat 中，不对外安装。
兼容 C 源文件只编入动态库目标。
静态链接只提供当前 API，不承诺对旧二进制的兼容性。

发布兼容的 0.2.0 版本时，保留 SONAME 主版本 0 和 URMA_0.1 节点。
未变更的符号留在原节点，新增符号放入继承 URMA_0.1 的 URMA_0.2 节点，
不要将所有符号移入最新节点。
ABI 已变更的函数保留 foo@URMA_0.1，同时通过 foo@@URMA_0.2 指定新的默认版本。
两个均使用单个 @ 的别名无法为新链接指定默认版本。

以下仅为未来接口变更的示例，不是已发布的 URMA API：

    /* Current public header */
    struct foo_args { int value; unsigned int flags; };
    int foo(const struct foo_args *args);

    /* Private historical layout */
    struct foo_args_v1 { int value; };

实现文件需要包含 errno.h、当前公共头文件、私有历史布局定义和
compat/urma_symver.h：

    static int foo_current(const struct foo_args *args)
    {
        if (!args || args->flags != 0)
            return -EINVAL;
        return args->value;
    }

    #ifdef URMA_BUILD_SHARED
    int urma_compat_foo_v1(const struct foo_args_v1 *old)
    {
        struct foo_args current;
        if (!old)
            return -EINVAL;
        current.value = old->value;
        current.flags = 0; /* Preserve the historical behavior. */
        return foo_current(&current);
    }
    int urma_compat_foo_current(const struct foo_args *args)
    {
        return foo_current(args);
    }
    URMA_SYMVER_COMPAT(urma_compat_foo_v1, foo, "URMA_0.1");
    URMA_SYMVER_DEFAULT(urma_compat_foo_current, foo, "URMA_0.2");
    #else
    int foo(const struct foo_args *args)
    {
        return foo_current(args);
    }
    #endif

每个符号别名必须与其实现定义在同一个编译单元中。
对应上述示例，未来的版本脚本应包含：

    URMA_0.1 {
        global: foo; /* Also retain every existing baseline symbol. */
        local: *;
    };
    URMA_0.2 {
        global: foo;
    } URMA_0.1;

不能将历史结构体直接强制转换为当前布局。
必须显式转换字段、默认值、错误结果、所有权和生命周期。
回调和不透明状态可能需要私有句柄适配。
必须在每种支持的架构上保留历史布局的大小、对齐方式和字段偏移。
旧二进制中已编译的内联代码及直接字段访问无法通过符号别名修复。
如果无法保持历史语义，就不能宣称后向兼容。

发布前，应使用替换后的动态库运行未经修改的旧版调用程序，验证历史语义，
同时测试使用当前头文件编译的新程序。
通过 readelf --dyn-syms --wide 和 --version-info 检查历史 @ 与当前 @@ 条目，
并确认 urma_compat_* 实现名称未对外导出。
当前 0.1.0 不添加虚构的历史接口或 0.2 节点。

## 修改位置、目的和影响

以下路径相对于 umdk。保留当前提交历史，在现有代码上调整版本实现。

| 位置 | 改动与用途 |
|---|---|
| src/urma/lib/urma/core/include/urma_types.h | 直接定义 0/1/0 版本字段、64 位编码和字符串，执行字段范围和调用方版本要求检查 |
| 同目录 urma_api.h、urma_provider.h、urma_cmd.h | 通过包含关系取得版本值，各入口执行主版本和最低版本检查 |
| src/urma/CMakeLists.txt | 设置库及 note 版本 0.1.0，启用 ASM，仅生成 note |
| src/urma/urma_version_note.S.in | note 模板保留在 URMA 子目录，描述值为 65536 |
| src/urma/lib/urma/core/CMakeLists.txt | 链接 note 和版本脚本、编译兼容实现；移除生成版本头的包含和安装规则 |
| src/urma/common/CMakeLists.txt | 将 note 链接进 liburma_common |
| src/urma/lib/uvs/core/CMakeLists.txt | 显式设置 PROJECT_VERSION="0.1.0"、PROJECT_VERSION_MAJOR="0" |
| src/urma/lib/urma/core/urma.map | 保留 URMA_0.1 显式符号基线 |
| src/urma/lib/urma/core/compat/urma_symver.h | 保留历史 @ 和默认 @@ 符号映射宏 |
| src/urma/BUILD.bazel、bazel/generate_version.sh | 仅生成 note 与 SONAME 参数，移除生成版本头目标和依赖 |
| src/urma/urma_bazel.sh | 按统一库版本打包，只安装现有公共头文件 |

不修改 umdk/src/CMakeLists.txt。升级版本时需同步 urma_types.h 中的数值、
src/urma/CMakeLists.txt 的库版本以及 UVS 中的显式版本配置。
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

note 生成于 build/urma-version/urma/urma_version_note.S。
主要库输出位于 build/urma-version/urma/lib/urma/core/liburma.so.0.1.0
和 build/urma-version/urma/common/liburma_common.so.0.1.0。
安装规则使用 /usr/lib64 和 /usr/include/ub/umdk/urma 等绝对路径，
DESTDIR 用于增加暂存目录前缀。

也可以在 src/urma 目录使用 Bazel：

    bazel build //:urma //:urma_common

note 和四个 SONAME 参数文件生成于 bazel-bin/version。
库输出为 bazel-bin/liburma.so 等名称，内部 SONAME 为 liburma.so.0。
需要打包时运行：

    bash urma_bazel.sh compile --config=release

辅助脚本可能安装缺失依赖并获取 libummu 源码；其 install 操作会写入
系统目录、刷新 ldconfig，并在可用时重启 rsyslog。

应用按前文声明主版本和最低版本要求，包含现有的 urma_api.h 即可：

    cc app.c -I/usr/include/ub/umdk/urma -L/usr/lib64 -Wl,-z,now -lurma -o app

当前头文件下，REQUIRED_MAJOR=1 或最低版本
URMA_API_VERSION_ENCODE(0, 1, 1) 应触发编译错误。
版本要求必须在首次包含对应公共头文件之前声明。

发布前执行前文的 readelf 检查，确认 SONAME 为 liburma.so.0、
符号节点为 URMA_0.1、note 为 8 字节且值为 65536。
还需运行未修改的旧版本二进制和使用当前头文件的新程序。
新增私有兼容源文件后需重新运行 CMake 配置。保持旧语义、只安装当前公共头文件，
不要将历史结构体或私有兼容接口暴露给应用。

本地可执行版本生成脚本与静态一致性检查；完整 CMake/Bazel 构建、
实际 ELF 检查和动态加载测试仍需目标 Linux 环境完成。
