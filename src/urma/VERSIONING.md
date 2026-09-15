# URMA 版本规约

版本统一定义于 src/urma/CMakeLists.txt，当前为 1.0.0。
安装的 urma_version.h 由 urma_api.h 引入，对外提供
URMA_API_VERSION_MAJOR、URMA_API_VERSION_MINOR、URMA_API_VERSION_PATCH、
URMA_API_VERSION_NUMBER 和 URMA_API_VERSION_ENCODE(major, minor, patch)。
版本采用无符号 64 位编码，主版本、次版本和补丁版本分别占 32、16、16 位。

## 源码兼容性

调用方可在首次包含头文件之前声明版本要求：

    #define URMA_API_REQUIRED_MAJOR 1
    #define URMA_API_MIN_VERSION URMA_API_VERSION_ENCODE(1, 0, 0)
    #include <urma_api.h>

主版本不匹配或版本低于要求时，通过 #error 触发编译失败。
编译器仍会执行类型检查。调用方也可以自行编写检查：

    #include <urma_api.h>
    #if URMA_API_VERSION < URMA_API_VERSION_ENCODE(3, 2, 0)
    #error "URMA API >= 3.2 is required"
    #endif

第二个示例在当前 1.0.0 版本下会按预期编译失败。

## 二进制兼容性

liburma.so.1.0.0 的 SONAME 为 liburma.so.1，现有导出符号归属于稳定的
URMA_1.0 符号版本节点。新链接的调用方会为引用的符号记录该节点。
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

    readelf -d liburma.so.1.0.0
    readelf --version-info liburma.so.1.0.0
    readelf --dyn-syms --wide liburma.so.1.0.0
    readelf -n liburma.so.1.0.0

预期 SONAME 为 liburma.so.1，符号版本节点为 URMA_1.0。
URMA note 的描述数据为 8 字节，采用本机字节序，值为 0x0000000100000000。

## 接口演进中的后向兼容

只安装当前版本的公共头文件。历史结构体布局和适配代码放在
lib/urma/core/compat 中，不对外安装。
兼容 C 源文件只编入动态库目标。
静态链接只提供当前 API，不承诺对旧二进制的兼容性。

发布兼容的 1.1.0 版本时，保留 SONAME 主版本 1 和 URMA_1.0 节点。
未变更的符号留在原节点，新增符号放入继承 URMA_1.0 的 URMA_1.1 节点，
不要将所有符号移入最新节点。
ABI 已变更的函数保留 foo@URMA_1.0，同时通过 foo@@URMA_1.1 指定新的默认版本。
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
    URMA_SYMVER_COMPAT(urma_compat_foo_v1, foo, "URMA_1.0");
    URMA_SYMVER_DEFAULT(urma_compat_foo_current, foo, "URMA_1.1");
    #else
    int foo(const struct foo_args *args)
    {
        return foo_current(args);
    }
    #endif

每个符号别名必须与其实现定义在同一个编译单元中。
对应上述示例，未来的版本脚本应包含：

    URMA_1.0 {
        global: foo; /* Also retain every existing baseline symbol. */
        local: *;
    };
    URMA_1.1 {
        global: foo;
    } URMA_1.0;

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
当前 1.0.0 不添加虚构的历史接口或 1.1 节点。
