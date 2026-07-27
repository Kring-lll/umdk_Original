#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#
# 批量编译+跑 out/ 下所有 .cpp 测试用例（带 sim）。
# 假设在 umdk 根目录执行：bash baseline/run_all_tests.sh

BL=$(cd "$(dirname "$0")" && pwd)
SIM=$BL/../sim/urma
SRC=$BL/../src
UA_CORE=$SRC/build_noasan/urma/lib/urma/core
UA_COMMON=$SRC/build_noasan/urma/common
URMA_INC=$SRC/urma/lib/urma/core/include
URMA_COMMON_INC=$SRC/urma/common/include

# gtest 路径
if [ -n "$GTEST_DIR" ]; then GTEST_INC=$GTEST_DIR/googletest/include; GTEST_LIB=$GTEST_DIR/build/lib;
elif [ -f /usr/local/lib64/libgtest.a ]; then GTEST_INC=/usr/local/include; GTEST_LIB=/usr/local/lib64;
elif [ -f /usr/lib64/libgtest.a ]; then GTEST_INC=/usr/include; GTEST_LIB=/usr/lib64;
else echo "✗ gtest 未找到。export GTEST_DIR=/path/to/googletest-x.x.x"; exit 1; fi

export LD_LIBRARY_PATH=$UA_CORE:$UA_COMMON
export LD_PRELOAD=$SIM/build/liburma_sim.so
export URMA_SIM_CONFIG=$SIM/urma_sim_config.json
export URMA_SIM_HW=1

pass=0
fail=0

for cpp in $BL/out/*.cpp; do
    name=$(basename "$cpp" .cpp)
    echo "── $name ──"
    if ! g++ -std=c++17 -fpermissive -w \
        -I$GTEST_INC -I$BL/gen -I$URMA_INC -I$URMA_COMMON_INC \
        -L$SIM/build -lurma_sim -L$UA_CORE -lurma -L$UA_COMMON -lurma_common \
        -Wl,-rpath,$SIM/build -Wl,-rpath,$UA_CORE -Wl,-rpath,$UA_COMMON \
        $BL/gen/bl_fixture.cpp "$cpp" \
        -L$GTEST_LIB -lgtest_main -lgtest -lpthread \
        -o /tmp/${name}_run 2>/dev/null; then
        echo "  COMPILE FAIL"
        fail=$((fail+1))
        continue
    fi
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
