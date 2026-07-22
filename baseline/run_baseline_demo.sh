#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#
# baseline 门禁流程演示：跑测试 → 生成基准 → 正常比对 PASS → 篡改模拟回归 → 比对 FAIL（门禁挡住）。
#
# 这个 example 展示 baseline 门禁怎么用：
#   1. 跑一次测试，输出 BASELINE|... 格式行（宏标注的验证点）
#   2. gen_baseline 生成基准 expected.txt（固化这次运行）
#   3. check 比对（正常应 PASS）
#   4. 篡改一个值模拟版本回归 → check 比对应 FAIL（门禁挡住）
#
# 用法：bash /umdk/baseline/run_baseline_demo.sh

set -e

UA_CORE=/umdk/src/build_noasan/urma/lib/urma/core
UA_COMMON=/umdk/src/build_noasan/urma/common
export LD_LIBRARY_PATH=$UA_CORE:$UA_COMMON
export LD_PRELOAD=/umdk/sim/urma/build/liburma_sim.so
export URMA_SIM_CONFIG=/umdk/sim/urma/urma_sim_config.json
export URMA_SIM_HW=1

DEMO=/tmp/test_baseline_demo
GEN=/tmp/gen_baseline
OUT=/tmp/baseline_demo_out.txt
BAD=/tmp/baseline_demo_bad.txt
EXP=/tmp/baseline_demo_expected.txt

# 若二进制不在（容器重启清了 /tmp），重新编
BL=/umdk/baseline   # baseline 根：gen/（生成器） src/（测试源） out/（产物）
if [ ! -x "$DEMO" ] || [ ! -x "$GEN" ]; then
    echo "=== 重新编译 demo + 生成器（/tmp 清了）==="
    URMA_INC=/umdk/src/urma/lib/urma/core/include
    URMA_COMMON_INC=/umdk/src/urma/common/include
    gcc -I$BL/gen -I$URMA_INC -I$URMA_COMMON_INC \
        -L/umdk/sim/urma/build -lurma_sim \
        -L$UA_CORE -lurma -L$UA_COMMON -lurma_common \
        -Wl,-rpath,/umdk/sim/urma/build -Wl,-rpath,$UA_CORE -Wl,-rpath,$UA_COMMON \
        $BL/src/test_baseline_demo.c -o $DEMO
    gcc $BL/gen/gen_baseline.c -o $GEN
fi

echo "================================================"
echo "1. 跑 demo test（输出 BASELINE 行）"
echo "================================================"
timeout 30 $DEMO 2>$OUT
echo "产出 $(grep -c '^BASELINE|' $OUT) 个验证点（BASELINE 行）"
echo "--- 前 5 行 ---"
grep '^BASELINE|' $OUT | head -5
echo "..."

echo
echo "================================================"
echo "2. 生成基准 expected.txt（固化这次运行）"
echo "================================================"
$GEN gen $OUT $EXP
echo "--- expected.txt（NONEMPTY 只存名字不存值，VAL/HEX/STR 存值）---"
cat $EXP

echo
echo "================================================"
echo "3. 正常回归比对（行为未变，应 PASS）"
echo "================================================"
$GEN check $OUT $EXP | tail -4

echo
echo "================================================"
echo "4. 模拟版本回归：篡改 poll_status 0→99（行为变了）"
echo "================================================"
sed 's/|VAL|poll_status|0$/|VAL|poll_status|99/' $OUT > $BAD
echo "篡改后跑 check，门禁应 FAIL（挡住回归）："
$GEN check $BAD $EXP | grep -E "FAIL|passed|BASELINE" | head -5

echo
echo "================================================"
echo "演示完成。门禁机制：版本更迭改了行为 → BASELINE FAIL → 挡住，强制开发者确认。"
echo "================================================"
