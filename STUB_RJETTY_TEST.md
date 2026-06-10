# Rjetty信息交换打桩测试说明

## 修改概述

为了在 urma 提供真实接口前验证 bind 流程中交换 rjetty 信息的功能，实现了以下打桩方案：

### 1. 打桩函数实现

在 `src/urpc/umq/umq_ub/core/private/umq_symbol_private.c` 中添加了两个打桩函数：

- `stub_urma_get_rjetty`: 创建并填充 rjetty 结构体
- `stub_urma_put_rjetty`: 释放 rjetty 内存

### 2. 可选符号加载

使用 `LOAD_OPTIONAL_SYMBOL_WITH_STUB` 宏：
- 如果 liburma.so 提供真实接口，使用真实接口
- 如果 liburma.so 未提供接口，使用打桩函数
- 日志会记录使用的是真实接口还是打桩函数

### 3. 测试方法

#### 方法1：使用打桩函数（默认）

直接编译运行，因为打桩函数已内置：

```bash
# 编译
cd build
make

# 运行测试程序或应用
./your_test_program
```

#### 方法2：使用真实接口

当 urma 提供真实接口后：

```bash
# 确保 liburma.so 包含 urma_get_rjetty 和 urma_put_rjetty
ldd your_test_program | grep liburma

# 运行时会自动检测并使用真实接口
./your_test_program
```

### 4. 验证点

检查日志输出：

```bash
# 应该看到类似日志：
# stub: get rjetty for jetty_id xxx, length 56
# stub: put rjetty
```

或使用真实接口时：

```bash
# 不会有 stub 相关日志
# 只会看到正常的功能日志
```

### 5. 打桩函数行为

打桩函数会：
- 从 jetty 结构中提取关键信息填充到 rjetty
- 设置 rjetty_size = sizeof(urma_rjetty_t)
- 确保 bind 流程可以正常序列化和反序列化

### 6. 关键文件

- `src/urpc/umq/umq_ub/core/private/umq_symbol_private.c`: 打桩函数实现
- `src/urpc/umq/umq_ub/core/private/umq_ub.c`: 序列化/反序列化逻辑
- `src/urpc/umq/umq_ub/core/private/umq_ub_private.h`: 数据结构定义

## 后续工作

当 urma 提供真实接口后：
1. 打桩函数会自动失效，真实接口会被使用
2. 无需修改任何代码
3. 删除打桩函数即可（可选）