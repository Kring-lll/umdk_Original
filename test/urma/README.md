# URMA UT 编写指导

本文档用于指导新增和维护 `test/urma` 下的正式 GTest 用例。目标是让用例稳定、可复用、可持续提高覆盖率，而不是为了覆盖率强行打到真实设备路径。

## 新增 UT 的推荐流程

新增一个用例前，按下面顺序处理。

1. 先确认测试对象属于哪类代码：
   - common 纯工具函数：直接构造输入和断言输出。
   - core TLV/ioctl wrapper：用 `-Wl,--wrap=ioctl` 捕获命令，不访问真实 ioctl。
   - bond 纯逻辑：构造内存态的 context、target、segment、WR、CR。
   - bond 对外 API：优先覆盖非法输入、非法状态、引用计数保护、空成员集合和失败回收路径。
2. 再确认是否已有 mock 数据可复用：
   - command/TLV 数据优先查 `include/urma_cmd_mock.h`。
   - bonding 路径优先复用 `BondPathFixture` 或 `BondPublicApiFixture`。
   - 只有已有数据无法表达新场景时，才补充共享数据集。
3. 写用例时先覆盖稳定接口行为：
   - public/internal header 暴露的函数优先。
   - 不 include `.c` 文件去测 static 函数。
   - 不依赖 `/dev/uburma`、sysfs、netlink、真实 provider 或 `/lib64/urma/*`。
4. 最后运行完整脚本和门禁：

```bash
bash test/urma/script/urma_UT.sh
git diff --check
```

## 选择测试边界

优先测试可稳定复现的边界。遇到真实系统边界时，不要直接调用真实环境。

| 边界 | 推荐方式 | 禁止方式 |
| --- | --- | --- |
| ioctl | `-Wl,--wrap=ioctl` 捕获 hdr 和 attrs | 访问真实 `/dev/uburma` |
| 命令/TLV | 比较 command、args_len、attr type、field size、array metadata、字段地址 | 比较不稳定的运行时绝对地址值 |
| 内存分配失败 | link wrap 或最小 test-only seam | 在生产逻辑里加测试专用分支 |
| bonding provider | 构造无设备 context，覆盖非法状态和失败回收 | 依赖真实 provider 动态库 |
| netlink/sysfs | 暂不纳入首阶段分母，后续加 seam 后再测 | 在 UT 中访问系统真实状态 |

如果一个路径必须依赖硬件、内核驱动、真实 netlink 或 provider 正常工作，先不要把它加入阶段覆盖率门禁。先为它设计可控 seam。

## Mock 数据使用规则

新增用例时必须先复用已有 mock 数据。只有以下情况允许补充数据：

1. 新场景需要新的 command 结构体或 attr 顺序。
2. 新场景需要一组可被多个 test 文件复用的 bonding topology、WR、CR、segment 或 context 数据。
3. 新场景无法用已有 fixture 表达，且不是一次性非法输入。

一次性非法输入可以留在单个 test 文件内，例如 `nullptr`、`UINT32_MAX`、`dev_fd=-1`、空数组、空成员集合。

### command/TLV 数据补充方式

在 `include/urma_cmd_mock.h` 中按同一模式补：

```cpp
inline urma_cmd_xxx_t MakeXxxCmd()
{
    urma_cmd_xxx_t arg = {};
    arg.in.some_field = MOCK_VALUE;
    return arg;
}

inline std::vector<ExpectedAttr> ExpectedXxxAttrs(urma_cmd_xxx_t *arg)
{
    return {
        { XXX_IN_FIELD, sizeof(arg->in.some_field), 1, 0,
          reinterpret_cast<uintptr_t>(&arg->in.some_field) },
    };
}
```

断言应验证稳定字段：

- command id。
- `args_len`。
- attr 顺序。
- attr `type`。
- attr `field_size`。
- array metadata。
- attr data 是否指向原始字段。

不要断言内核会如何处理该命令；TLV UT 只验证用户态命令构造和错误传播。

### bonding 数据补充方式

优先扩展已有 fixture：

- `BondPathFixture`：用于 datapath convert/schedule、WR/CR、target/segment/path 映射。
- `BondPublicApiFixture`：用于 `bondp_api.h`、`bondp_datapath.h`、`bondp_segment.h`、`bondp_provider_ops.h` 的无设备 public API 测试。

新增字段时保持 fixture 仍然是纯内存对象，不引入真实 fd、真实 context 创建或真实 provider 依赖。需要触发 ioctl 失败时，使用 `dev_fd=-1` 这类稳定失败路径。

## 用例编写模板

### 普通接口用例

```cpp
TEST(UrmaCommonTest, FunctionNameCoversNormalAndInvalidInput)
{
    InputType input = MakeReusableInput();

    EXPECT_EQ(EXPECTED_OK, function_under_test(&input));
    EXPECT_EQ(EXPECTED_ERR, function_under_test(nullptr));
}
```

要求：

- 测试名描述行为，不描述实现细节。
- 同一个 test 内覆盖一组强相关路径，不把无关函数塞在一起。
- 断言返回值后，再断言关键输出字段或状态变化。

### TLV/ioctl wrapper 用例

```cpp
TEST(UrmaCmdTlvTest, XxxTlvAttrsMatchExpected)
{
    urma_cmd_xxx_t arg = urma_test::MakeXxxCmd();

    ResetIoctlCapture();
    EXPECT_EQ(0, urma_ioctl_xxx(urma_test::MOCK_IOCTL_FD, &arg));
    ExpectCapturedCommand(URMA_CMD_XXX, urma_test::ExpectedXxxAttrs(&arg));
}
```

如果暂时没有完整 attr 预期，只允许作为补齐函数覆盖的过渡方式使用 header 级断言：

```cpp
EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_xxx, URMA_CMD_XXX, urma_cmd_xxx_t);
```

后续补 mock 数据时，应把 header 级断言升级为完整 attr 断言。

### bonding public API 用例

```cpp
TEST(UrmaBondTest, PublicApiRejectsInvalidState)
{
    BondPublicApiFixture fixture;

    EXPECT_EQ(URMA_EAGAIN, bondp_delete_jfs(&fixture.jfs.v_jfs));
}
```

要求：

- 优先覆盖外层 contract：参数校验、状态校验、引用计数、失败回收。
- 不为了进入深层正常路径而伪造不完整对象。
- 一旦路径需要真实 provider、netlink、sysfs 或 health thread，先停在稳定失败点。

## 覆盖率门禁维护

首阶段门禁只统计已经可稳定 UT 的文件组，规则在 `script/urma_UT.sh` 的 `check_phase1_coverage()` 中维护。

当前门禁要求：

- function coverage: `100%`
- line coverage: `>= 80%`

扩大门禁分母前必须满足：

1. 新文件不依赖真实硬件或真实系统状态。
2. 新文件的主要外部边界已有 mock、wrap 或 seam。
3. 新文件可以在本地和 CI 稳定通过。
4. 新文件加入后仍能维持阶段门禁。

不要把真实设备发现、netlink、provider、health thread、完整 bond API 正常路径提前加入首阶段门禁。

## Do / Don't

Do:

- 优先复用已有 mock 数据。
- 先测 public/internal header 暴露的函数。
- 对失败路径断言返回码和关键状态不被误改。
- 对 TLV 命令断言 attr 顺序和字段映射。
- 对 bonding API 使用纯内存 fixture 和稳定失败路径。
- 每次改完运行 `bash test/urma/script/urma_UT.sh`。

Don't:

- 不 include `.c` 文件测试 static 函数。
- 不硬编码 `/lib64/urma/*`。
- 不访问真实 `/dev`、sysfs、netlink 或 UDMA provider。
- 不为覆盖率修改生产默认行为。
- 不把一次性边界数据塞进共享 mock。
- 不为了覆盖率把不稳定路径加入门禁。

## 提交前检查

提交前逐项确认：

1. 新用例能稳定构建和运行。
2. 复用了已有 mock 数据；确实无法覆盖时才补充数据集。
3. 新增共享数据有清晰命名，能被后续用例复用。
4. 没有真实设备、sysfs、netlink 或安装库依赖。
5. `bash test/urma/script/urma_UT.sh` 通过。
6. 阶段覆盖率门禁通过。
7. `git diff --check` 通过。
