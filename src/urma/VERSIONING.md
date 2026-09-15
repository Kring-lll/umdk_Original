# URMA 版本规约

版本统一定义于 src/urma/CMakeLists.txt，当前为 0.1.0。
安装的 urma_version.h 由 urma_api.h 引入，对外提供
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

下列路径相对于 umdk 仓库根目录。用户态版本基线已由 1.0.0 重设为 0.1.0。

| 修改位置 | 改动与目的 | 影响 |
|---|---|---|
| src/urma/CMakeLists.txt | 统一设置 PROJECT_VERSION=0.1.0，生成版本头和 ELF note，并启用 ASM | API 字段为 0/1/0；相关库 VERSION=0.1.0、SOVERSION=0 |
| src/urma/lib/urma/core/include/urma_version.h.in | 定义公开版本宏、字段范围检查、指定主版本及最低版本检查 | 调用方可通过预处理器在编译期拒绝不满足要求的头文件 |
| src/urma/lib/urma/core/include/urma_api.h | 引入生成的版本头 | API 使用者可直接使用版本规约宏 |
| src/urma/lib/urma/core/include/urma_types.h | 删除旧的 URMA_API_VERSION=0.9 定义，改为引入统一版本头 | 消除重定义和包含顺序导致的版本冲突 |
| src/urma/urma_version_note.S.in | 定义 URMA ELF note 模板，写入 8 字节版本编码 | liburma 和 liburma_common 可通过 readelf 查看维测版本 |
| src/urma/common/CMakeLists.txt | 将 note 汇编编入动态库并继承统一版本 | liburma_common 与 liburma 使用一致的版本来源 |
| src/urma/lib/urma/core/CMakeLists.txt | 接入 note、版本脚本、立即绑定、生成头文件安装和私有兼容源码 | 构建 liburma.so.0.1.0；兼容头文件不对外安装 |
| src/urma/lib/urma/bond/CMakeLists.txt、src/urma/lib/uvs/core/CMakeLists.txt | 删除各自的旧版本覆盖 | liburma_ubagg、libtpsa 继承 0.1.0，SONAME 主版本为 0 |
| src/urma/lib/urma/core/urma.map | 使用 URMA_0.1 显式符号基线并隐藏未列出的符号 | 本次版本调整保持 367 个列出符号不变；发布前仍需与实际 ELF 核对 |
| src/urma/lib/urma/core/compat/urma_symver.h | 提供历史 @ 和默认 @@ 符号版本宏 | 后续可由私有包装函数转换旧参数并调用当前实现 |
| src/urma/BUILD.bazel | 接入生成头文件、note、符号脚本、SONAME 参数和兼容源码 | Bazel 路径使用与 CMake 相同的版本来源 |
| src/urma/bazel/generate_version.sh | 从 CMakeLists.txt 和模板生成版本文件 | 不在 Bazel 中另行维护版本常量 |
| src/urma/bazel/BUILD.bazel、src/urma/bazel/urma_shared.bzl | 声明生成脚本，并向链接动作传递额外输入 | 版本脚本及 SONAME 参数成为构建依赖 |
| src/urma/urma_bazel.sh | 修正版本读取位置，安装生成的 urma_version.h | 打包版本、动态库文件名和公开头文件保持一致 |
| src/urma/VERSIONING.md | 记录规约、修改位置及使用步骤 | 作为版本维护与验证入口 |

此次调整没有修改实际接口签名、结构体布局或业务语义。
但 SONAME 从 liburma.so.1 变为 liburma.so.0，符号节点从 URMA_1.0 变为
URMA_0.1，这是基线重设，不能当作对旧 1.0.0 二进制的无缝替换。
已经依赖旧 SONAME 或旧符号节点的程序通常需要重新链接。

此前内核仓库中的 UBCORE KAPI/ABI 为 1.0.0，本轮 umdk 调整未修改内核版本。
用户态 API 版本、内核 KAPI/ABI 和通信协议检查属于不同边界，不能仅根据
版本字符串是否相同判断 liburma 与内核是否兼容。

## 构建和安装

CMake 和 Bazel 是两条可选构建路径，不需要依次执行。
以下命令面向已配置依赖的 Linux 构建环境。
不依赖 .vscode/settings.json；通过命令行明确指定源码和构建目录。
不要直接修改生成的 urma_version.h 或 urma_version_note.S。

### CMake 路径

在 umdk 仓库根目录执行：

    cmake -S src -B build/urma-version \
        -DBUILD_ALL=disable -DBUILD_URMA=enable
    cmake --build build/urma-version --parallel

该入口仍可能配置 URMA 的工具、provider 等子目录，需要满足相应依赖。
可用以下命令只构建两个主要库目标：

    cmake --build build/urma-version --target urma urma_common --parallel

生成文件和主要产物位置：

| 文件 | 路径 |
|---|---|
| 版本头 | build/urma-version/urma/include/urma_version.h |
| note 汇编 | build/urma-version/urma/urma_version_note.S |
| liburma | build/urma-version/urma/lib/urma/core/liburma.so.0.1.0 |
| liburma_common | build/urma-version/urma/common/liburma_common.so.0.1.0 |

完成所需目标构建后，可先安装到暂存目录查看结果：

    DESTDIR="$PWD/build/urma-stage" cmake --install build/urma-version

安装规则使用 /usr/lib64 和 /usr/include/ub/umdk/urma 等绝对路径。
DESTDIR 会为这些路径增加暂存前缀；仅设置 CMAKE_INSTALL_PREFIX 不会改写
已有的绝对安装目标。完整安装可能要求先完成其他子目标构建。

### Bazel 路径

在 umdk/src/urma 目录执行，先配置 WORKSPACE 中所需的外部依赖：

    bazel build //:urma //:urma_common

生成文件位于 bazel-bin/version：

    bazel-bin/version/urma_version.h
    bazel-bin/version/urma_version_note.S
    bazel-bin/version/urma.ldflags
    bazel-bin/version/urma_common.ldflags
    bazel-bin/version/urma_ubagg.ldflags
    bazel-bin/version/tpsa.ldflags

构建输出使用 bazel-bin/liburma.so 等名称，ELF 内部的 SONAME 由生成参数
指定为 liburma.so.0。打包阶段再生成带完整版本的文件名和软链接。

构建并打包可使用现有辅助脚本：

    bash urma_bazel.sh compile --config=release

产物为当前目录下的 urma-bazel-<timestamp>.tar.gz。
安装时指定实际生成的包名：

    bash urma_bazel.sh install /path/to/urma-bazel-<timestamp>.tar.gz

辅助脚本会检查依赖，可能通过 yum 安装缺失包；compile 会获取配置的
libummu 源码，install 会写入系统目录、刷新 ldconfig，并在可用时重启
rsyslog。请在适合构建或安装的 Linux 环境使用。

## 调用方如何使用

### 编译期要求与普通链接

在应用首次包含 URMA 头文件之前声明版本要求：

    #define URMA_API_REQUIRED_MAJOR 0
    #define URMA_API_MIN_VERSION URMA_API_VERSION_ENCODE(0, 1, 0)
    #include <urma_api.h>

安装到系统后的链接示例：

    cc app.c -I/usr/include/ub/umdk/urma \
        -L/usr/lib64 -Wl,-z,now -lurma -o app

应用无需直接包含私有的 compat/urma_symver.h。
版本不满足要求时由头文件触发编译错误；二进制加载时由 SONAME、
所需符号版本和符号解析执行检查。

如果仅希望在编译期验证版本头，可创建 version_check.c：

    #define URMA_API_REQUIRED_MAJOR 0
    #define URMA_API_MIN_VERSION URMA_API_VERSION_ENCODE(0, 1, 0)
    #include <urma_version.h>
    _Static_assert(URMA_API_VERSION_NUMBER == 0x0000000000010000ULL,
                   "unexpected URMA version");

使用安装后的头文件进行检查：

    cc -std=c11 -fsyntax-only -I/usr/include/ub/umdk/urma version_check.c

将 REQUIRED_MAJOR 改为 1，或者将最低版本改为
URMA_API_VERSION_ENCODE(0, 1, 1)，应分别触发编译失败。

### 查看实际二进制信息

在 umdk 仓库根目录，以 CMake 产物为例：

    readelf -d build/urma-version/urma/lib/urma/core/liburma.so.0.1.0
    readelf --version-info build/urma-version/urma/lib/urma/core/liburma.so.0.1.0
    readelf --dyn-syms --wide build/urma-version/urma/lib/urma/core/liburma.so.0.1.0
    readelf -n build/urma-version/urma/lib/urma/core/liburma.so.0.1.0
    readelf -n build/urma-version/urma/common/liburma_common.so.0.1.0

预期结果：

- SONAME 为 liburma.so.0。
- 现有符号版本节点为 URMA_0.1。
- URMA note 描述数据长度为 8 字节，版本值为 65536。
- 字节序由目标平台决定；小端显示为 00 00 01 00 00 00 00 00。
- 新链接应用应记录所需的 URMA_0.1 节点，可用 readelf --version-info app 检查。

## 后续维护步骤与验证状态

1. 在 src/urma/CMakeLists.txt 修改统一版本，重新配置并构建；不要手改生成物。
2. 对于兼容演进，保留旧节点及其接口；仅为新增接口或新 ABI 实现增加节点。
3. 若需兼容旧接口，在私有 compat 目录保存旧布局并实现转换，按前文示例
   为旧实现使用 @、当前默认实现使用 @@。CMake 新增兼容源文件后需重新配置。
4. 仅安装当前公共头文件。无法保持历史语义时，不能以包装或符号别名宣称兼容。
5. 核对 ELF 导出集合，运行旧版未修改二进制和新版调用程序，验证行为及加载结果。

已完成的检查包括：旧版本引用检索、367 个基线符号集合比较、
Bash 语法检查、版本生成器实际运行，以及差异格式检查。
生成器已验证输出 0.1.0 版本头、值为 65536 的 note 汇编和四个 .so.0
SONAME 参数文件。尚未完成完整 CMake/Bazel 编译、实际 ELF 检查和
动态加载兼容性测试，上述构建与验证步骤不能视为已经执行通过。
