#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#
# baseline 门禁流程演示：跑测试 → 生成基准 → 正常 PASS → 篡改 FAIL。
# 假设在 umdk 根目录执行：bash baseline/run_baseline_demo.sh

set -e

BL=$(cd "$(dirname "$0")" && pwd)
SIM=$BL/../sim/urma
SRC=$BL/../src
UA_CORE=$SRC/build_noasan/urma/lib/urma/core
UA_COMMON=$SRC/build_noasan/urma/common
URMA_INC=$SRC/urma/lib/urma/core/include
URMA_COMMON_INC=$SRC/urma/common/include

export LD_LIBRARY_PATH=$UA_CORE:$UA_COMMON
export LD_PRELOAD=$SIM/build/liburma_sim.so
export URMA_SIM_CONFIG=$SIM/urma_sim_config.json
export URMA_SIM_HW=1

DEMO=/tmp/test_demo2
GEN=/tmp/gen_baseline
OUT=/tmp/baseline_out.txt
BAD=/tmp/baseline_bad.txt
EXP=/tmp/baseline_expected.txt

if [ ! -x "$DEMO" ] || [ ! -x "$GEN" ]; then
    echo "=== 编译 ==="
    gcc -I$BL/gen -I$URMA_INC -I$URMA_COMMON_INC \
        -L$SIM/build -lurma_sim -L$UA_CORE -lurma -L$UA_COMMON -lurma_common \
        -Wl,-rpath,$SIM/build -Wl,-rpath,$UA_CORE -Wl,-rpath,$UA_COMMON \
        $BL/gen/bl_fixture.c $BL/src/test_read.c -o $DEMO
    gcc $BL/gen/gen_baseline.c -o $GEN
fi

echo "1. 跑测试"
timeout 30 $DEMO 2>$OUT
echo "   $(grep -c '^BASELINE|' $OUT) 条验证点"

echo "2. 生成基准"
$GEN gen $OUT $EXP
cat $EXP

echo "3. 正常比对（应 PASS）"
$GEN check $OUT $EXP | tail -2

echo "4. 篡改模拟回归（应 FAIL）"
sed 's/|VAL|poll_status|0$/|VAL|poll_status|99/' $OUT > $BAD
$GEN check $BAD $EXP | grep -E "FAIL|BASELINE"
