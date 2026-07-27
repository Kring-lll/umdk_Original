---
name: baseline-test-template
description: 编写 baseline 测试模板并通过 gen_baseline 生成 gtest 测试用例
---

# Skill: baseline 测试模板编写与生成

## 何时使用

当需要为 URMA 仿真代码编写或扩展测试时，使用本 skill 规范模板写法并生成 gtest 测试用例。

## 前置条件

- 在 umdk 仓库根目录
- liburma_sim.so 已编译（`umdk/sim/urma/build/liburma_sim.so`）
- gtest 已安装或 `GTEST_DIR` 已设
- build_noasan 的 liburma 已编译（`umdk/src/build_noasan/`）

## gen_baseline 工具

`gen_baseline` 是核心工具，三个模式：

### gen — 录制基准

```bash
gen_baseline gen <run_output.txt> <expected.txt>
```

**输入**：模板跑出来的 stderr（含 `BASELINE|file|line|kind|name|value` 行）。

**逻辑**：
1. `parse_line` 逐行扫描，找 `BASELINE|` 开头的行
2. 按 `|` 分割：跳过 file 和 line，提取 kind（VAL/HEX/STR/NONEMPTY）、name、value
3. 分类存进 expected.txt：
   - VAL/HEX/STR → `kind|name|value`（值必须逐次一致）
   - NONEMPTY → `NONEMPTY|name|`（不存值，只验非空——地址每次跑不同）

**输出**：`expected.txt`，每行一条基准，格式 `<kind>|<name>|<value>`。

### gen_test — 生成 gtest .cpp

```bash
gen_baseline gen_test <src.c> <expected.txt> <out.cpp>
```

**输入**：模板源 `.c` + 基准 `expected.txt`。

**生成逻辑**（逐行扫描源文件）：

| 源行类型 | 处理 |
|---|---|
| `#include` | 跳过（头部已加 gtest + fixture 头） |
| `#define` / `typedef` | 原样保留 |
| `int main(...)` | 整体跳过（gtest_main 提供 main） |
| `void test_xxx(void)` | 输出 `TEST(test_xxx, Run)` + `{\n` |
| `static void *xxx(void *)` | 原样输出（不包 TEST，供 test 函数调用） |
| `WRITE_VAL("name", var)` | `find_entry(name)` 查基准值 → `EXPECT_EQ(value, var)` |
| `WRITE_HEX("name", var)` | 同上 |
| `WRITE_STR("name", var)` | `EXPECT_EQ(0, strncmp("baseline", var, strlen("baseline")))` |
| `ASSERT_NONEMPTY("name", ptr)` | `ASSERT_NE(nullptr, ptr)`（不查基准） |
| `return ...;` | 删除（TEST 返回 void） |
| `goto cleanup` / `cleanup:` | 原样保留（C++ 允许 goto 跨 NULL 初始化变量） |
| 其他行 | 原样输出 |

`find_entry` 按宏的 name 字段查基准——**name 必须唯一**，否则匹配到第一个值。

生成后自动删 expected.txt（值已编进 EXPECT 断言，不再需要）。

**输出**：`.cpp`，头部含 gtest + fixture 头，每个 `void test_xxx(void)` 对应一个 `TEST(test_xxx, Run)`。

### check — 回归比对

```bash
gen_baseline check <new_output.txt> <expected.txt>
```

不编译不跑 gtest，直接文本比对——重跑模板拿新输出和基准逐条比。适合 CI 快速验门禁。

## 模板写法

一个 `.c` 文件，含 `void test_xxx(void)` 函数 + `int main`。gen_test 给每个函数生成一个 TEST。

```c
// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "bl_fixture.h"
#include "baseline_macros.h"

#define SEG_LEN 4096

void test_xxx(void)
{
    // 变量声明（全部在最前，C++ goto 不跨初始化）
    ctx_t *c = NULL;
    char *buf = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    // 建链（用 fixture，不裸调 urma API）
    c = setup(0, URMA_TM_UM, URMA_UTP);
    if (!c) goto cleanup;
    buf = calloc(1, SEG_LEN); memset(buf, 'A', SEG_LEN);
    tseg = register_seg(c, buf, SEG_LEN);
    if (!tseg) goto cleanup;
    tj = import_self(c);
    if (!tj) goto cleanup;

    // 操作 + 标注验证点
    ret = post_read(c, tj, buf, buf, tseg, SEG_LEN, 0x42);
    WRITE_VAL("post_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("poll_n", n);
    WRITE_VAL("poll_status", cr.status);
    WRITE_HEX("poll_user_ctx", cr.user_ctx);
    WRITE_VAL("buf_first", buf[0]);
    WRITE_STR("buf_content", buf);

cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(buf);
    if (c) teardown(c);
}

int main(void)
{
    test_xxx();
    return 0;
}
```

## 六条规则

### 1. 函数签名 `void test_xxx(void)`

gen_test 找 `void` 开头的函数，提取函数名，生成 `TEST(test_xxx, Run) { body }`。一个文件多个函数 → 每个各一个 TEST。main 里只放纯调用，不放 sleep/fork/IO。

### 2. 子函数用 `static`

`static void *xxx(void *)` 或 `static void xxx(void)` → gen_test 原样保留不包 TEST。跨进程的 fork/sleep/IO 收在 static 子函数内。

### 3. 局部变量全部在最前声明

C++ 的 goto 不允许跨带初始化的变量声明。所有变量声明放函数体第一块，初始化为 NULL/0。用 `goto cleanup` 统一释放——变量预初始化 NULL 保证 free/teardown 安全。

### 4. 用 fixture 函数，不裸调 urma API

```c
// 正确
c = setup(0, URMA_TM_RC, URMA_CTP);
tj = bind_remote(c, eid, uasid, jetty_id);
ret = post_write(c, tj, src, tseg, dst, len, 0x43);
teardown(c);
```

### 5. 用宏标注验证点（名字必须唯一）

| 宏 | 适用 | gen_test 产出 |
|---|---|---|
| `WRITE_VAL("name", val)` | 逻辑值，每次跑一样 | `EXPECT_EQ(value, var)` |
| `WRITE_HEX("name", val)` | hex 值 | `EXPECT_EQ(0xNN, var)` |
| `WRITE_STR("name", str)` | 数据搬运结果 | `EXPECT_EQ(0, strncmp(...))` |
| `ASSERT_NONEMPTY("name", ptr)` | 地址/句柄（每次不同，只验非空） | `ASSERT_NE(nullptr, ptr)` |

宏跑时输出 `BASELINE|file|line|kind|name|value` 到 stderr → gen 生成基准 → gen_test 把宏换成断言。

**名字必须唯一**：同一文件多个函数不能用同名宏。`find_entry` 按名字查基准，同名会匹配到第一个值。用前缀区分：`read_dst_first` / `write_dst_first`。

### 6. 不手写 ok/fprintf("TEST PASS")

gtest 自动判定。模板只标注验证点（WRITE_*），判定交给 EXPECT 断言。

## 生成流程

```bash
# 1. 编模板
gcc -I baseline/gen -I <urma_inc> -L <sim_lib> -lurma_sim -L <urma_lib> -lurma -lurma_common \
    -Wl,-rpath,<...> baseline/gen/bl_fixture.c src/test_xxx.c -o /tmp/test_xxx -w

# 2. 跑模板（BASELINE 行进 stderr）
/tmp/test_xxx 2> /tmp/out.txt

# 3. 生成基准
baseline/gen/gen_baseline gen /tmp/out.txt out/test_xxx_expected.txt

# 4. 生成 gtest .cpp（自动删 expected.txt）
baseline/gen/gen_baseline gen_test src/test_xxx.c out/test_xxx_expected.txt out/test_xxx.cpp

# 5. 编 + 跑
g++ -std=c++17 -fpermissive -w -I <gtest_inc> -I baseline/gen -I <urma_inc> \
    -L <sim_lib> -lurma_sim -L <urma_lib> -lurma -lurma_common \
    -Wl,-rpath,<...> baseline/gen/bl_fixture.cpp out/test_xxx.cpp \
    -L <gtest_lib> -lgtest_main -lgtest -lpthread -o /tmp/test_xxx_gen
/tmp/test_xxx_gen
```

或一键：`bash baseline/build_and_test.sh`

## 生成的 .cpp 怎么用

gen_test 生成的 `.cpp` 是独立可编译的 gtest 文件：

- 头部自动加了 `#include <gtest/gtest.h>` + fixture 头 + 常用系统头
- 每个 `void test_xxx(void)` 变成 `TEST(test_xxx, Run) { ... }`
- `static` 子函数原样保留（test 函数可调用）
- `#define` / `typedef` 原样保留
- `int main` 删了（gtest_main 提供）
- 宏行换成 `EXPECT_EQ` / `ASSERT_NE`，值从基准来
- `return` 删了（TEST 返回 void）
- `goto cleanup` / `cleanup:` 保留（C++ 允许）

编译命令（带 sim）：
```bash
g++ -std=c++17 -fpermissive -w \
    -I $GTEST_INC -I baseline/gen -I $URMA_INC \
    -L $SIM_LIB -lurma_sim -L $URMA_LIB -lurma -lurma_common \
    -Wl,-rpath,$SIM_LIB -Wl,-rpath,$URMA_LIB \
    baseline/gen/bl_fixture.cpp out/test_xxx.cpp \
    -L $GTEST_LIB -lgtest_main -lgtest -lpthread \
    -o /tmp/test_xxx_gen
```

编译命令（不带 sim，真机用）：
```bash
g++ -std=c++17 -fpermissive -w \
    -I $GTEST_INC -I baseline/gen -I $URMA_INC \
    -L $URMA_LIB -lurma -lurma_common \
    -Wl,-rpath,$URMA_LIB \
    baseline/gen/bl_fixture.cpp out/test_xxx.cpp \
    -L $GTEST_LIB -lgtest_main -lgtest -lpthread \
    -o /tmp/test_xxx_gen
```

跑：`/tmp/test_xxx_gen` → `[ PASSED ] N tests.`

## fixture 函数

### 建链

| 函数 | 用途 |
|---|---|
| `setup(dev_idx, trans_mode, tp_type)` | 建 jetty(share_jfr)，UM/RC/RM × RTP/CTP |
| `setup_default(dev_idx)` | 便捷：UM + UTP |

### 注册 + import

| 函数 | 用途 |
|---|---|
| `register_seg(c, buf, len)` | 注册 seg |
| `import_self(c)` | import 自引用 jetty（UM） |
| `bind_remote(c, eid, uasid, jetty_id)` | import + bind 对端 jetty（RC） |
| `import_jetty_remote(c, eid, ...)` | import 对端 jetty（UM 跨进程） |
| `import_seg_remote(c, eid, ...)` | import 对端 seg |

### 操作

| 函数 | 用途 |
|---|---|
| `post_rw(c, tj, opcode, local, remote_buf, remote_tseg, len, user_ctx, extra1, extra2)` | 统一 post |
| `post_read(c, tj, dst, src, tseg, len, ctx)` | READ |
| `post_write(c, tj, src, dst_tseg, dst_buf, len, ctx)` | WRITE |
| `post_cas(c, tj, atomic_buf, tseg, orig_buf, cmp, swap, len, ctx)` | CAS |
| `post_faa(c, tj, atomic_buf, tseg, orig_buf, operand, len, ctx)` | FAA |
| `post_send(c, tj, data, len, ctx)` | SEND |
| `post_recv(c, buf, len, ctx)` | post recv WQE |

### poll + 释放

| 函数 | 用途 |
|---|---|
| `poll_cr(c, &cr)` | poll 重试 100 次 |
| `get_jetty_id(c)` | 取 jetty_id（OOB 传给对端） |
| `teardown(c)` | 逆序释放全部 |

## 异常注入

### CQE 错误注入

录制时设环境变量 `URMA_SIM_INJECT_STATUS=<val>`，生成的 EXPECT_EQ 验证错误转换链。

| URMA_SIM_INJECT_STATUS | cr.status | 语义 |
|---|---|---|
| 0 | 0 | 正常 |
| 1 | 1 | UNSUPPORTED_OPCODE |
| 4 | 10 | RNR_RETRY |
| 5 | 9 | ACK_TIMEOUT |

### 链路故障注入

设 `URMA_SIM_LINK_DOWN=<eid前8字节hex>` 注入链路 down，IPC 请求返错 → error CQE。

## 跨进程模板

```c
static void do_a_side(void) {
    ctx_t *c = setup(0, URMA_TM_UM, URMA_UTP);
    // 注册 seg → 写 OOB 文件（token_id + eid + jetty_id）→ sleep
    teardown(c);
}

void test_cross_read(void) {
    // fork → do_a_side()  子进程跑 A
    // setup(1) → 读 OOB → import_seg_remote → bind_remote
    // → post_read → poll_cr → WRITE_VAL
    // waitpid
}

int main(void) { test_cross_read(); return 0; }
```

A/B 各用不同 dev_idx，sim 用 eid 寻址对端。OOB 文件传 token_id + eid + jetty_id。fork/sleep 在 static 子函数内。
