#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#
# 批量编译+跑 out/ 下所有 .cpp 测试用例，汇总结果。
# 用法：bash /umdk/baseline/run_all_tests.sh

set -e

BL=/umdk/baseline
UA_CORE=/umdk/src/build_noasan/urma/lib/urma/core
UA_COMMON=/umdk/src/build_noasan/urma/common
URMA_INC=/umdk/src/urma/lib/urma/core/include
URMA_COMMON_INC=/umdk/src/urma/common/include

export LD_LIBRARY_PATH=$UA_CORE:$UA_COMMON
export LD_PRELOAD=$BL/../sim/urma/build/liburma_sim.so
export URMA_SIM_CONFIG=$BL/../sim/urma/urma_sim_config.json
export URMA_SIM_HW=1

pass=0
fail=0

for cpp in $BL/out/*.cpp; do
    name=$(basename "$cpp" .cpp)
    echo "── $name ──"

    # 编译
    if ! g++ -std=c++17 -fpermissive -Wno-error \
        -I/usr/local/include -I$BL/gen -I$URMA_INC -I$URMA_COMMON_INC \
        -L$BL/../sim/urma/build -lurma_sim -L$UA_CORE -lurma -L$UA_COMMON -lurma_common \
        -Wl,-rpath,$BL/../sim/urma/build -Wl,-rpath,$UA_CORE -Wl,-rpath,$UA_COMMON \
        $BL/gen/bl_fixture.cpp "$cpp" \
        -L/usr/local/lib64 -lgtest_main -lgtest -lpthread \
        -o /tmp/${name}_run 2>/dev/null; then
        echo "  COMPILE FAIL"
        fail=$((fail+1))
        continue
    fi

    # 跑
    if timeout 30 /tmp/${name}_run 2>&1 | grep -q "\[  PASSED  \]"; then
        echo "  PASSED"
        pass=$((pass+1))
    else
        echo "  FAILED"
        fail=$((fail+1))
    fi
    rm -f /tmp/${name}_run
done

echo ""
echo "========== $pass passed, $fail failed =========="
exit $fail
