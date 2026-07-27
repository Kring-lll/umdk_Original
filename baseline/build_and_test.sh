#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#
# 一站式：从零编 umdk 全部源码 → 生成测试 → 跑 sim + no-sim 两套测试 → 汇总。
# 在 umdk 根目录执行：bash baseline/build_and_test.sh
#
# 步骤：
#   [build]  编 liburma → 编 liburma_sim → 编 gen_baseline + fixture
#   [gen]   跑模板 → 生成基准 + .cpp → out/
#   [sim]   编 + 跑 gtest（带 sim LD_PRELOAD，标记 [sim]）
#   [no-sim] 编 + 跑 gtest（不带 sim，直连真 urma，标记 [no-sim]）
#   [check]  回归比对

# 不用 set -e——中间步骤失败不退出，最后汇总

BL=$(cd "$(dirname "$0")" && pwd)
SRC=$BL/../src
SIM=$BL/../sim/urma
UA_CORE=$SRC/build_noasan/urma/lib/urma/core
UA_COMMON=$SRC/build_noasan/urma/common
URMA_INC=$SRC/urma/lib/urma/core/include
URMA_COMMON_INC=$SRC/urma/common/include

# gtest 路径：默认搜标准位置，找不到用环境变量 GTEST_DIR 覆盖
# 用户设 GTEST_DIR=/home/zyc/umdk/tmp/googletest-1.17.0 即可
if [ -n "$GTEST_DIR" ]; then
    GTEST_INC=$GTEST_DIR/googletest/include
    GTEST_LIB=$GTEST_DIR/build/lib
elif [ -f /usr/local/lib64/libgtest.a ]; then
    GTEST_INC=/usr/local/include
    GTEST_LIB=/usr/local/lib64
elif [ -f /usr/lib64/libgtest.a ]; then
    GTEST_INC=/usr/include
    GTEST_LIB=/usr/lib64
else
    echo "✗ gtest 未找到。设环境变量：export GTEST_DIR=/path/to/googletest-x.x.x"
    echo "  （该路径下需有 include/gtest/gtest.h 和 build/lib/libgtest.a）"
    exit 1
fi

echo "╔══════════════════════════════════════════════╗"
echo "║  umdk 全量构建 + 测试（sim / no-sim）        ║"
echo "╚══════════════════════════════════════════════╝"

############################################
# [build] 编译全部
############################################
echo ""
echo "=== [build] 编译 ==="

# 1. 编 liburma（force rebuild）
echo "[1/5] 编 liburma（build_noasan）"
cd $SRC && rm -rf build_noasan && mkdir build_noasan && cd build_noasan
cmake .. -DBUILD_ALL=disable -DBUILD_URMA=enable -DBUILD_UDMA=disable \
         -DGTEST=disable -DASAN=disable -DCODE_COVERAGE=disable 2>&1 | tail -2
cmake --build . -j 2>&1 | tail -2
echo "  ✓ liburma.so"
cd $BL

# 2. 编 liburma_sim（force rebuild）
echo "[2/5] 编 liburma_sim.so"
cd $SIM && rm -rf build && mkdir build && cd build
cmake .. 2>&1 | tail -2
cmake --build . -j 2>&1 | tail -2
echo "  ✓ liburma_sim.so"
cd $BL

# 3. 编 gen_baseline
echo "[3/5] 编 gen_baseline"
gcc $BL/gen/gen_baseline.c -o /tmp/gen_baseline 2>&1
echo "  ✓ gen_baseline"

# 4. 编 fixture（C 版 .o 供模板用）
echo "[4/5] 编 bl_fixture（C）"
gcc -c -I$BL/gen -I$URMA_INC -I$URMA_COMMON_INC $BL/gen/bl_fixture.c -o /tmp/bl_fixture.o 2>&1
echo "  ✓ bl_fixture.o"

# 5. 确认 gtest 库在（用前面检测到的 GTEST_LIB）
echo "[5/5] 检查 gtest"
if [ ! -f "$GTEST_LIB/libgtest.a" ]; then
    echo "  ✗ gtest 库未找到（$GTEST_LIB/libgtest.a）"
    echo "  跳过 gtest 步骤，只做模板录制 + check"
    NO_GTEST=1
else
    echo "  ✓ gtest 就绪（$GTEST_LIB）"
    NO_GTEST=0
fi

############################################
# [gen] 跑模板 → 生成基准 + .cpp
############################################
echo ""
echo "=== [gen] 跑模板 → 生成基准 + .cpp → out/ ==="

export LD_LIBRARY_PATH=$UA_CORE:$UA_COMMON
export LD_PRELOAD=$SIM/build/liburma_sim.so
export URMA_SIM_CONFIG=$SIM/urma_sim_config.json
export URMA_SIM_HW=1

for tmpl in $BL/src/test_*.c; do
    name=$(basename "$tmpl" .c)
    echo -n "  $name: "
    gcc -I$BL/gen -I$URMA_INC -I$URMA_COMMON_INC \
        -L$SIM/build -lurma_sim -L$UA_CORE -lurma -L$UA_COMMON -lurma_common \
        -Wl,-rpath,$SIM/build -Wl,-rpath,$UA_CORE -Wl,-rpath,$UA_COMMON \
        $BL/gen/bl_fixture.c "$tmpl" -o /tmp/$name 2>&1
    case $name in
        test_inject_err)         URMA_SIM_INJECT_STATUS=5 timeout 30 /tmp/$name 2>/tmp/${name}_out.txt ;;
        test_inject_rnr)         URMA_SIM_INJECT_STATUS=4 timeout 30 /tmp/$name 2>/tmp/${name}_out.txt ;;
        test_inject_local_err)   URMA_SIM_INJECT_STATUS=1 timeout 30 /tmp/$name 2>/tmp/${name}_out.txt ;;
        *)                       timeout 30 /tmp/$name 2>/tmp/${name}_out.txt ;;
    esac
    bl=$(grep -c "^BASELINE|" /tmp/${name}_out.txt )
    /tmp/gen_baseline gen /tmp/${name}_out.txt $BL/out/${name}_expected.txt 
    /tmp/gen_baseline gen_test "$tmpl" $BL/out/${name}_expected.txt $BL/out/${name}.cpp 
    echo "BL=$bl"
    rm -f /tmp/$name
done

############################################
# [sim] 编 + 跑 gtest（带 sim）
############################################
echo ""
echo "=== [sim] gtest 测试（带仿真）==="

export LD_PRELOAD=$SIM/build/liburma_sim.so
export URMA_SIM_HW=1

spass=0
sfail=0
if [ "$NO_GTEST" != "1" ]; then
    for cpp in $BL/out/*.cpp; do
        name=$(basename "$cpp" .cpp)
        echo "  [sim] $name:"
        if g++ -std=c++17 -fpermissive -w \
            -I$GTEST_INC -I$BL/gen -I$URMA_INC -I$URMA_COMMON_INC \
            -L$SIM/build -lurma_sim -L$UA_CORE -lurma -L$UA_COMMON -lurma_common \
            -Wl,-rpath,$SIM/build -Wl,-rpath,$UA_CORE -Wl,-rpath,$UA_COMMON \
            $BL/gen/bl_fixture.cpp "$cpp" \
            -L$GTEST_LIB -lgtest_main -lgtest -lpthread \
            -o /tmp/${name}_sim 2>&1; then
            if timeout 30 /tmp/${name}_sim 2>&1 | grep -q "\[  PASSED  \]"; then
                echo "    PASS"
                spass=$((spass+1))
            else
                echo "    FAIL（运行失败，输出：）"
                timeout 30 /tmp/${name}_sim 2>&1 | tail -5 | sed 's/^/    /'
                sfail=$((sfail+1))
            fi
        else
            echo "    COMPILE FAIL（编译错误见上）"
            sfail=$((sfail+1))
        fi
        rm -f /tmp/${name}_sim
    done
else
    echo "  (跳过，无 gtest)"
fi

############################################
# [no-sim] 编 + 跑 gtest（不带 sim）
############################################
echo ""
echo "=== [no-sim] gtest 测试（直连真 urma，无仿真）==="

export LD_PRELOAD=""
export URMA_SIM_HW=""

npass=0
nfail=0
if [ "$NO_GTEST" != "1" ]; then
    for cpp in $BL/out/*.cpp; do
        name=$(basename "$cpp" .cpp)
        echo "  [no-sim] $name:"
        if g++ -std=c++17 -fpermissive -w \
            -I$GTEST_INC -I$BL/gen -I$URMA_INC -I$URMA_COMMON_INC \
            -L$UA_CORE -lurma -L$UA_COMMON -lurma_common \
            -Wl,-rpath,$UA_CORE -Wl,-rpath,$UA_COMMON \
            $BL/gen/bl_fixture.cpp "$cpp" \
            -L$GTEST_LIB -lgtest_main -lgtest -lpthread \
            -o /tmp/${name}_nosim 2>&1; then
            if timeout 30 /tmp/${name}_nosim 2>&1 | grep -q "\[  PASSED  \]"; then
                echo "    PASS"
                npass=$((npass+1))
            else
                echo "    FAIL（无真硬件正常，输出末尾：）"
                timeout 30 /tmp/${name}_nosim 2>&1 | tail -5 | sed 's/^/    /'
                nfail=$((nfail+1))
            fi
        else
            echo "    COMPILE FAIL（编译错误见上）"
            nfail=$((nfail+1))
        fi
        rm -f /tmp/${name}_nosim
    done
else
    echo "  (跳过，无 gtest)"
fi

############################################
# [check] 回归比对（带 sim）
############################################
echo ""
echo "=== [check] 回归比对 ==="

export LD_PRELOAD=$SIM/build/liburma_sim.so
export URMA_SIM_HW=1

cpass=0
cfail=0
for tmpl in $BL/src/test_*.c; do
    name=$(basename "$tmpl" .c)
    exp=$BL/out/${name}_expected.txt
    [ ! -f "$exp" ] && continue
    echo -n "  $name: "
    gcc -I$BL/gen -I$URMA_INC -I$URMA_COMMON_INC \
        -L$SIM/build -lurma_sim -L$UA_CORE -lurma -L$UA_COMMON -lurma_common \
        -Wl,-rpath,$SIM/build -Wl,-rpath,$UA_CORE -Wl,-rpath,$UA_COMMON \
        $BL/gen/bl_fixture.c "$tmpl" -o /tmp/${name}_chk 
    case $name in
        test_inject_err)         URMA_SIM_INJECT_STATUS=5 timeout 30 /tmp/${name}_chk 2>/tmp/${name}_chk_out.txt ;;
        test_inject_rnr)         URMA_SIM_INJECT_STATUS=4 timeout 30 /tmp/${name}_chk 2>/tmp/${name}_chk_out.txt ;;
        test_inject_local_err)   URMA_SIM_INJECT_STATUS=1 timeout 30 /tmp/${name}_chk 2>/tmp/${name}_chk_out.txt ;;
        *)                       timeout 30 /tmp/${name}_chk 2>/tmp/${name}_chk_out.txt ;;
    esac
    if /tmp/gen_baseline check /tmp/${name}_chk_out.txt "$exp" 2>&1 | grep -q "BASELINE PASS"; then
        echo "PASS"
        cpass=$((cpass+1))
    else
        echo "FAIL"
        cfail=$((cfail+1))
    fi
    rm -f /tmp/${name}_chk
done

############################################
# 汇总
############################################
echo ""
echo "╔══════════════════════════════════════════════╗"
echo "  [sim]    gtest:    $spass passed, $sfail failed"
echo "  [no-sim] gtest:    $npass passed, $nfail failed"
echo "  [check]  baseline: $cpass passed, $cfail failed"
echo "╚══════════════════════════════════════════════╝"
exit $((sfail + nfail + cfail))
