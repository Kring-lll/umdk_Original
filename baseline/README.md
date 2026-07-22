# baseline：门禁式回归测试框架

从测试模板（纯 C + 宏标注）自动生成 gtest 风格 C++ 测试用例，版本更迭改了行为就 FAIL 挡住。

## 结构

```
baseline/
├── gen/                        工具层
│   ├── gen_baseline.c           生成器/比对器（gen / check / gen_test 三模式）
│   ├── baseline_macros.h        录制宏（WRITE_VAL / WRITE_HEX / WRITE_STR / ASSERT_NONEMPTY）
│   ├── bl_fixture.h / .c        C 版 fixture（setup / post_rw / poll_cr / teardown）
│   └── bl_fixture.hpp / .cpp    C++ 版 fixture（同接口 + RAII 析构）
├── src/                        测试模板（纯 C，gcc 编译）
│   ├── test_baseline_demo2.c    READ 模板
│   ├── test_write.c             WRITE 模板
│   ├── test_cas.c               CAS 模板（匹配 + 不匹配）
│   ├── test_faa.c               FAA 模板
│   └── test_inject_err.c        异常注入模板
└── out/                        生成产物
    ├── *_expected.txt           基准（gen 产出）
    └── *.cpp                    gtest 测试用例（gen_test 产出）
```

## 编译

### 前提

在 oe-arm 容器内，liburma_sim.so + build_noasan 的 liburma 已编好。

### 编译模板（纯 C，gcc）

```bash
UA_CORE=/umdk/src/build_noasan/urma/lib/urma/core
UA_COMMON=/umdk/src/build_noasan/urma/common
URMA_INC=/umdk/src/urma/lib/urma/core/include
URMA_COMMON_INC=/umdk/src/urma/common/include

# 编一个模板（例：test_write）
gcc -I/umdk/baseline/gen -I$URMA_INC -I$URMA_COMMON_INC \
    -L/umdk/sim/urma/build -lurma_sim -L$UA_CORE -lurma -L$UA_COMMON -lurma_common \
    -Wl,-rpath,/umdk/sim/urma/build -Wl,-rpath,$UA_CORE -Wl,-rpath,$UA_COMMON \
    /umdk/baseline/gen/bl_fixture.c /umdk/baseline/src/test_write.c -o /tmp/test_write
```

### 编译生成测试（C++，g++ + gtest）

```bash
g++ -std=c++17 -fpermissive -Wno-error \
    -I/usr/local/include -I/umdk/baseline/gen -I$URMA_INC -I$URMA_COMMON_INC \
    -L/umdk/sim/urma/build -lurma_sim -L$UA_CORE -lurma -L$UA_COMMON -lurma_common \
    -Wl,-rpath,/umdk/sim/urma/build -Wl,-rpath,$UA_CORE -Wl,-rpath,$UA_COMMON \
    /umdk/baseline/gen/bl_fixture.cpp /umdk/baseline/out/test_write.cpp \
    -L/usr/local/lib64 -lgtest_main -lgtest -lpthread -o /tmp/test_write_gen
```

### 编译生成器

```bash
gcc /umdk/baseline/gen/gen_baseline.c -o /tmp/gen_baseline
```

## 测试流程

### 1. 跑模板 → 录制基准

```bash
export LD_LIBRARY_PATH=$UA_CORE:$UA_COMMON
export LD_PRELOAD=/umdk/sim/urma/build/liburma_sim.so
export URMA_SIM_CONFIG=/umdk/sim/urma/urma_sim_config.json
export URMA_SIM_HW=1

# 跑模板，BASELINE 行进 stderr
/tmp/test_write 2> /tmp/write_out.txt

# 生成基准
/tmp/gen_baseline gen /tmp/write_out.txt /umdk/baseline/out/test_write_expected.txt
```

### 2. 生成 gtest 测试用例

```bash
/tmp/gen_baseline gen_test /umdk/baseline/src/test_write.c \
    /umdk/baseline/out/test_write_expected.txt \
    /umdk/baseline/out/test_write.cpp
```

### 3. 编译 + 跑生成测试

```bash
g++ ...（见上文编译生成测试）...
/tmp/test_write_gen
# [  PASSED  ] 1 test.
```

### 4. 回归比对（不重编，轻量验门禁）

```bash
# 版本更迭后重新跑模板
/tmp/test_write 2> /tmp/write_new.txt

# 比对基准
/tmp/gen_baseline check /tmp/write_new.txt /umdk/baseline/out/test_write_expected.txt
# 11 passed, 0 failed / BASELINE PASS
```

### 5. 异常注入测试

```bash
# 注入 ACK_TIMEOUT error CQE
URMA_SIM_INJECT_STATUS=5 /tmp/test_inject_err 2> /tmp/inj_out.txt

# 生成注入后的基准（poll_status 变成 9=ACK_TIMEOUT_ERR）
/tmp/gen_baseline gen /tmp/inj_out.txt /umdk/baseline/out/test_inject_err_expected.txt

# 生成测试（断言 poll_status==9）
/tmp/gen_baseline gen_test /umdk/baseline/src/test_inject_err.c \
    /umdk/baseline/out/test_inject_err_expected.txt \
    /umdk/baseline/out/test_inject_err.cpp
```

## 现有模板

| 模板 | 验什么 | BASELINE 条数 | 关键断言 |
|---|---|---|---|
| test_baseline_demo2.c | READ 搬数据 | 11 | post_ret=0, poll_n=1, status=0, user_ctx=0x42, dst_buf='A' |
| test_write.c | WRITE 反向搬数据 | 11 | post_ret=0, poll_n=1, status=0, user_ctx=0x43, dst_buf='B' |
| test_cas.c | CAS 匹配+不匹配 | 15 | atomic 0x100→0x200, orig=0x100；不匹配不变 |
| test_faa.c | FAA fetch-and-add | 10 | atomic 0x200→0x205, orig=0x200 |
| test_inject_err.c | 异常注入（正常+注入） | 9 | 正常: status=0；注入: status=9 (ACK_TIMEOUT) |

## fixture 函数

```c
ctx_t *setup(int dev_idx, urma_transport_mode_t trans_mode);  // 建链（RAII 析构自动释放）
urma_target_seg_t *register_seg(ctx_t *c, void *buf, size_t len);  // 注册 seg
urma_target_jetty_t *import_self(ctx_t *c);  // import 自引用 jetty
int post_rw(ctx_t *c, urma_target_jetty_t *tj, urma_opcode_t opcode,
            void *local, void *remote_buf, urma_target_seg_t *remote_tseg,
            size_t len, uint64_t user_ctx, uint64_t extra1, uint64_t extra2);  // 统一 post
// 便捷包装：
int post_read(...);   int post_write(...);   int post_cas(...);   int post_faa(...);
int poll_cr(ctx_t *c, urma_cr_t *cr);  // poll 循环（重试 100 次）
void teardown(ctx_t *c);  // 释放（C++ 版析构自动调）
```

## 录制宏

```c
WRITE_VAL("post_ret", ret);        // 值断言（生成 EXPECT_EQ）
WRITE_HEX("user_ctx", cr.user_ctx); // hex 值断言（生成 EXPECT_EQ）
WRITE_STR("dst_buf", dst_buf);     // 字符串断言（生成 EXPECT_EQ(strncmp)）
ASSERT_NONEMPTY("ctx", ctx);       // 非空断言（生成 ASSERT_NE(nullptr)）——fixture 内部已调
```

## gen_baseline 三模式

```bash
gen_baseline gen   <run_output.txt> <expected.txt>           # 生成基准
gen_baseline check <new_output.txt> <expected.txt>           # 回归比对（不重编）
gen_baseline gen_test <src.c> <expected.txt> <out.cpp>       # 生成 gtest 测试用例
```
