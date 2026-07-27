# baseline：URMA 仿真测试框架

为 URMA 仿真代码提供自动化回归测试：录制一次正确运行的输出值固化为基准，版本更迭后自动比对，改了行为就 FAIL 挡住。

## 工作流程

```
写模板（.c，用 fixture 建链 + WRITE_VAL 标注验证点）
  ↓ 编译跑
BASELINE 行输出（stderr，含验证点的名字和值）
  ↓ gen_baseline gen
基准文件（expected.txt，固化这次运行的值）
  ↓ gen_baseline gen_test
gtest 测试（.cpp，宏行换成 EXPECT_EQ，值从基准来）
  ↓ g++ + gtest 编译跑
[ PASSED ] N tests.（自动判定 PASS/FAIL）
```

版本更迭后重跑：行为没变 → EXPECT_EQ 通过 → PASS；行为变了 → 不匹配 → FAIL 挡住，强制开发者确认。

## 三件事

baseline 做了三件事，解决传统手写测试的痛点：

| 痛点 | 解法 |
|---|---|
| 断言靠手写期望值（容易漏、写错） | 录制断言——跑一次成功运行，自动提取输出值固化为基准 |
| 没有基准，改了不知道影响范围 | 回归比对——重跑后和基准逐条比，变了就 FAIL |
| 新测试要手抄 50 行建链样板 | fixture 模块化——建链/释放/操作封装成函数，写新测试只调 fixture + 标注宏 |

## 目录结构

```
baseline/
├── gen/                          工具层
│   ├── gen_baseline.c              生成器/比对器
│   ├── baseline_macros.h           录制宏
│   ├── bl_fixture.h / .c           C 版 fixture
│   └── bl_fixture.hpp / .cpp       C++ 版 fixture
├── src/                          测试模板
│   ├── test_read.c                 基础：READ
│   ├── test_write.c                基础：WRITE
│   ├── ...                         基础模板共 10 个
│   ├── test_comm.c                 通讯测试集 #1-#12
│   └── test_comm_ext.c            通讯测试补充集 #13-#20
├── out/                          生成产物
│   └── *.cpp                      gtest 测试用例
├── build_and_test.sh             一站式脚本
└── SKILL.md                      模板编写规范
```

## 快速开始

```bash
# 在 umdk 根目录执行
bash baseline/build_and_test.sh
```

## gen_baseline 三模式

| 模式 | 命令 | 作用 |
|---|---|---|
| gen | `gen_baseline gen <out.txt> <expected.txt>` | 从跑模板的 BASELINE 行生成基准 |
| gen_test | `gen_baseline gen_test <src.c> <expected.txt> <out.cpp>` | 从模板 + 基准生成 gtest .cpp，生成后自动删 expected.txt |
| check | `gen_baseline check <new.txt> <expected.txt>` | 重跑模板拿新输出和基准文本比对（不重编） |

### gen_test 提取规则

- 找 `void test_xxx(void)` → 包成 `TEST(test_xxx, Run) { body }`
- `static void *xxx(void *)` → 原样保留（不包 TEST）
- `#define` / `typedef` → 保留
- `#include` / `int main` → 跳过
- 宏行换成 EXPECT_EQ（值从基准来）
- `return` → 删（TEST 返回 void）
- 一个文件多个函数 → 每个各一个 TEST

## 录制宏

宏是模板和生成器之间的协议——标注"这里要验什么"，跑时输出固定格式行。

| 宏 | 适用场景 | gen_test 产出 |
|---|---|---|
| `WRITE_VAL("name", val)` | 逻辑值（status/ret/计数），每次跑一样 | `EXPECT_EQ(value, var)` |
| `WRITE_HEX("name", val)` | hex 值（user_ctx/handle） | `EXPECT_EQ(0xNN, var)` |
| `WRITE_STR("name", str)` | 数据搬运结果（buf 内容） | `EXPECT_EQ(0, strncmp(...))` |
| `ASSERT_NONEMPTY("name", ptr)` | 地址/句柄（每次跑不同，只验非空） | `ASSERT_NE(nullptr, ptr)` |

> **注意**：同一文件内多个函数的 WRITE_VAL 名字必须唯一（如 `read_dst_first` / `write_dst_first`），
> 否则 gen_test 的 find_entry 按名字匹配到第一个值导致期望值错配。

## fixture 模块

封装了每个测试都会做的建链/释放/操作。写新测试只调 fixture，不裸调 urma API：

```c
// 建链
ctx_t *c = setup(0, URMA_TM_UM, URMA_UTP);          // UM + UTP（默认最常用）
ctx_t *c = setup(0, URMA_TM_RC, URMA_CTP);          // RC + CTP

// 注册 + import
urma_target_seg_t *tseg = register_seg(c, buf, len);
urma_target_jetty_t *tj = import_self(c);             // UM 单进程自引用
urma_target_jetty_t *tj = bind_remote(c, eid, ...); // RC 跨进程 import+bind

// 操作
int ret = post_read(c, tj, dst, src, tseg, len, 0x42);
int ret = post_write(c, tj, src, tseg, dst, len, 0x43);
int ret = post_send(c, tj, data, len, 0x42);
post_recv(c, recv_buf, len, 0xA1);

// poll + 释放
int n = poll_cr(c, &cr);
teardown(c);
```

### 函数速查

| 函数 | 用途 |
|---|---|
| `setup(dev_idx, trans_mode, tp_type)` | 建 jetty(share_jfr)，支持 UM/RC/RM × RTP/CTP |
| `setup_default(dev_idx)` | 便捷：UM + UTP |
| `register_seg(c, buf, len)` | 注册 seg |
| `import_self(c)` | import 自引用 jetty（UM） |
| `bind_remote(c, eid, uasid, jetty_id)` | import + bind 对端 jetty（RC） |
| `import_jetty_remote(c, eid, ...)` | import 对端 jetty（UM 跨进程） |
| `import_seg_remote(c, eid, ...)` | import 对端 seg |
| `post_rw(c, tj, opcode, ...)` | 统一 post（READ/WRITE/CAS/FAA/NOP） |
| `post_read/write/cas/faa(...)` | 便捷包装 |
| `post_send(c, tj, buf, len, ctx)` | post SEND |
| `post_recv(c, buf, len, ctx)` | post recv WQE |
| `poll_cr(c, &cr)` | poll 重试 100 次 |
| `get_jetty_id(c)` | 取 jetty_id（OOB 传给对端用） |
| `teardown(c)` | 逆序释放 |

## 模板写法

详见 [SKILL.md](SKILL.md)。六条规则：

1. 函数是 `void test_xxx(void)` → gen_test 生成 `TEST(test_xxx, Run)`
2. 子函数用 `static` → gen_test 原样保留
3. 局部变量全部最前声明（C++ goto 不跨初始化）
4. 用 fixture 函数不裸调 urma API
5. 用宏标注验证点（名字必须唯一）
6. 不手写 ok/fprintf（gtest 自动判定）

## 现有测试

### 基础模板（10 个）

| 模板 | 验什么 | BL 条数 | gtest |
|---|---|---|---|
| test_read | READ 搬数据 dst Z→A | 12 | PASS ✅ |
| test_write | WRITE 搬数据 dst Z→B | 11 | PASS ✅ |
| test_cas | CAS 匹配 swap + 不匹配不变 | 15 | PASS ✅ |
| test_faa | FAA atomic += operand | 10 | PASS ✅ |
| test_nop | NOP 不搬数据只产 CQE | 10 | PASS ✅ |
| test_multi_post | 8 post 全 poll 到 | 9 | PASS ✅ |
| test_inject_err | 注入 ACK_TIMEOUT cr=9 | 9 | PASS ✅ |
| test_inject_rnr | 注入 RNR_RETRY cr=10 | 9 | PASS ✅ |
| test_inject_local_err | 注入 UNSUPPORTED_OPCODE cr=1 | 9 | PASS ✅ |
| test_cross_read | 跨进程 B 读 A 的 seg | 17 | PASS ✅ |

### 通讯测试（20 个，8 维度全覆盖）

**test_comm.c — 12 个 TEST**

| # | 语义 | 模式 | 共享 | 通讯 | 方向 | 设备 |
|---|---|---|---|---|---|---|
| 1 | read | UM | TP | jetty | 单向 | udma |
| 2 | write | UM | CTP | jfs+jfr | 单向 | udma |
| 3 | send | UM | TP | jfs+jfr | 双向 | udma |
| 4 | send_imm | UM | CTP | jetty | 双向 | udma |
| 5 | write_imm | UM | TP | jfs+jfr | 单向 | udma |
| 6 | read | RC | CTP | jetty | 双向 | udma |
| 7 | write | RM | TP | jfs+jfr | 双向 | udma |
| 8 | send | RC | TP | jetty | 单向 | bonding |
| 9 | read | RM | CTP | jfs+jfr | 单向 | bonding |
| 10 | send_imm | RC | CTP | jetty | 双向 | bonding |
| 11 | send | RC | TP | jetty | 双向 | bonding+故障切换 |
| 12 | read | RM | CTP | jfs+jfr | 双向 | bonding+故障回切 |

`[ PASSED ] 12 tests.` ✅

**test_comm_ext.c — 8 个 TEST（补充组合）**

| # | 语义 | 模式 | 共享 | 通讯 | 方向 | 设备 |
|---|---|---|---|---|---|---|
| 13 | read | UM | CTP | jetty | 单向 | udma |
| 14 | write | RM | CTP | jetty | 双向 | udma |
| 15 | send | RC | CTP | jfs+jfr | 双向 | udma |
| 16 | read | RC | TP | jfs+jfr | 单向 | udma |
| 17 | write | RC | CTP | jetty | 单向 | bonding |
| 18 | send | RM | CTP | jetty | 双向 | bonding |
| 19 | read | UM | TP | jfs+jfr | 双向 | bonding |
| 20 | write_imm | RC | CTP | jetty | 单向 | bonding |

`[ PASSED ] 8 tests.` ✅

### 维度覆盖

| 维度 | 取值 | 覆盖 |
|---|---|---|
| 语义 | send / read / write / send_imm / write_imm | ✅ 全 5 |
| 模式 | RC / RM / UM | ✅ 全 3 |
| 共享 | TP / CTP | ✅ 全 2 |
| 通讯 | jetty / jfs+jfr | ✅ 全 2 |
| 方向 | 单向 / 双向 | ✅ 全 2 |
| 设备 | udma / bonding | ✅ 全 2 |
| 故障切换 | bonding | ✅ #11 |
| 故障回切 | bonding | ✅ #12 |

## 故障注入

### CQE 错误注入

```bash
# 录制时设环境变量注入 error CQE
URMA_SIM_INJECT_STATUS=5 ./test_inject_err 2> out.txt
# poll_status=9 (ACK_TIMEOUT) ← 错误转换链验证

| URMA_SIM_INJECT_STATUS | cr.status | 语义 |
|---|---|---|
| 0 | 0 (SUCCESS) | 正常 |
| 1 | 1 | UNSUPPORTED_OPCODE |
| 4 | 10 | RNR_RETRY |
| 5 | 9 | ACK_TIMEOUT |
```

### 链路故障注入

```bash
# 注入链路 down（按 eid 前 8 字节匹配）
URMA_SIM_LINK_DOWN=010aa8c0ffff0000 ./test_cross_read
# IPC 请求返错 → error CQE (cr.status=9) → 模拟 bonding 故障切换
```

## 脚本

| 脚本 | 用途 |
|---|---|
| `build_and_test.sh` | 一站式：编译→录制→生成→gtest(sim+no-sim)→check→汇总 |
| `run_all_tests.sh` | 只跑 out/*.cpp（带 sim） |
| `run_all_tests_no_sim.sh` | 只跑 out/*.cpp（不带 sim，真机用） |
| `run_baseline_demo.sh` | 演示门禁流程 |

## gtest 路径

gtest 不在标准位置时：
```bash
export GTEST_DIR=/path/to/googletest-x.x.x
```
脚本自动检测 `$GTEST_DIR/googletest/include` 和 `$GTEST_DIR/build/lib`。
