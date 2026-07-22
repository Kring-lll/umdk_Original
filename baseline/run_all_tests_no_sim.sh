#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#
# 批量编译+跑 out/ 下所有 .cpp 测试用例（不带 sim）。
# 不链 liburma_sim.so，不设 LD_PRELOAD/URMA_SIM_*——直连真 urma 库 + 真设备。
# 用于真机/真内核环境验证。容器里会失败（无真硬件）。
#
# 用法：bash /umdk/baseline/run_all_tests_no_sim.sh

set -e

BL=/umdk/baseline
UA_CORE=/umdk/src/build_noasan/urma/lib/urma/core
UA_COMMON=/umdk/src/build_noasan/urma/common
URMA_INC=/umdk/src/urma/lib/urma/core/include
URMA_COMMON_INC=/umdk/src/urma/common/include

# 不链 -lurma_sim，不设 LD_PRELOAD / URMA_SIM_*
export LD_LIBRARY_PATH=$UA_CORE:$UA_COMMON

pass=0
fail=0

for cpp in $BL/out/*.cpp; do
    name=$(basename "$cpp" .cpp)
    echo "── $name ──"

    # 编译（不链 sim 库，只链真 urma + gtest）
    if ! g++ -std=c++17 -fpermissive -Wno-error \
        -I/usr/local/include -I$BL/gen -I$URMA_INC -I$URMA_COMMON_INC \
        -L$UA_CORE -lurma -L$UA_COMMON -lurma_common \
        -Wl,-rpath,$UA_CORE -Wl,-rpath,$UA_COMMON \
        $BL/gen/bl_fixture.cpp "$cpp" \
        -L/usr/local/lib64 -lgtest_main -lgtest -lpthread \
        -o /tmp/${name}_nosim 2>/dev/null; then
        echo "  COMPILE FAIL"
        fail=$((fail+1))
        continue
    fi

    # 跑（直连真 urma，无 sim）
    if timeout 30 /tmp/${name}_nosim 2>&1 | grep -q "\[  PASSED  \]"; then
        echo "  PASSED"
        pass=$((pass+1))
    else
        echo "  FAILED (无真硬件正常)"
        fail=$((fail+1))
    fi
    rm -f /tmp/${name}_nosim
done

echo ""
echo "========== $pass passed, $fail failed =========="
exit $fail
