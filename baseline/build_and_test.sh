#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#
# Full pipeline: build all umdk sources -> generate tests -> run sim + no-sim suites -> summary.
# Run from the umdk root: bash baseline/build_and_test.sh
#
# Phases:
#   [build]  liburma -> liburma_sim -> gen_baseline + bl_fixture
#   [gen]    run templates -> record baseline -> gen_test -> out/*.cpp
#   [sim]    build + run gtest (liburma_sim LD_PRELOAD)
#   [no-sim] probe real device; build + run gtest (direct urma, no sim)
#   [check]  regression comparison

# No set -e: intermediate phase failures do not abort; final summary aggregates.

BL=$(cd "$(dirname "$0")" && pwd)
SRC=$BL/../src
SIM=$BL/../sim/urma
UA_CORE=$SRC/build_noasan/urma/lib/urma/core
UA_COMMON=$SRC/build_noasan/urma/common
URMA_INC=$SRC/urma/lib/urma/core/include
URMA_COMMON_INC=$SRC/urma/common/include

# gtest discovery: check standard locations first, override with GTEST_DIR.
# e.g. GTEST_DIR=/path/to/googletest-1.17.0 (expects include/ and build/lib/)
if [ -n "$GTEST_DIR" ]; then
    GTEST_INC=$GTEST_DIR/googletest/include
    GTEST_LIB=$GTEST_DIR/build/lib
elif [ -f /usr/local/lib64/libgtest.a ] || [ -f /usr/local/lib64/libgtest.so ]; then
    GTEST_INC=/usr/local/include
    GTEST_LIB=/usr/local/lib64
elif [ -f /usr/lib64/libgtest.a ] || [ -f /usr/lib64/libgtest.so ]; then
    GTEST_INC=/usr/include
    GTEST_LIB=/usr/lib64
else
    echo "fail: gtest not found. set GTEST_DIR=/path/to/googletest-x.x.x"
    echo "      (expects include/gtest/gtest.h and build/lib/libgtest.a or libgtest.so)"
    exit 1
fi

echo "╔══════════════════════════════════════════════════╗"
echo "║  umdk build + test (sim / no-sim / regression)  ║"
echo "╚══════════════════════════════════════════════════╝"

############################################
# [build] compile all
############################################
echo ""
echo "=== [build] compile liburma / liburma_sim / gen_baseline ==="

# 1. liburma (force rebuild)
echo "[1/5] liburma (build_noasan)"
cd $SRC && rm -rf build_noasan && mkdir build_noasan && cd build_noasan
cmake .. -DBUILD_ALL=disable -DBUILD_URMA=enable -DBUILD_UDMA=disable \
         -DGTEST=disable -DASAN=disable -DCODE_COVERAGE=disable 2>&1 | tail -2
cmake --build . -j 2>&1 | tail -2
echo "  ok: liburma.so"
cd $BL

# 2. liburma_sim (force rebuild)
echo "[2/5] liburma_sim.so"
cd $SIM && rm -rf build && mkdir build && cd build
cmake .. 2>&1 | tail -2
cmake --build . -j 2>&1 | tail -2
echo "  ok: liburma_sim.so"
cd $BL

# 3. gen_baseline tool
echo "[3/5] gen_baseline"
gcc $BL/gen/gen_baseline.c -o /tmp/gen_baseline 2>&1
echo "  ok: gen_baseline"

# 4. bl_fixture (C object, for templates)
echo "[4/5] bl_fixture (C)"
gcc -c -I$BL/gen -I$URMA_INC -I$URMA_COMMON_INC $BL/gen/bl_fixture.c -o /tmp/bl_fixture.o 2>&1
echo "  ok: bl_fixture.o"

# 5. gtest availability
echo "[5/5] gtest"
if [ ! -f "$GTEST_LIB/libgtest.a" ] && [ ! -f "$GTEST_LIB/libgtest.so" ]; then
    echo "  fail: gtest not found in $GTEST_LIB (need libgtest.a or libgtest.so)"
    echo "  skipping gtest phases, recording + check only"
    NO_GTEST=1
else
    echo "  ok: gtest in $GTEST_LIB"
    NO_GTEST=0
fi

############################################
# [gen] run templates -> record baseline -> gen_test -> out/
############################################
echo ""
echo "=== [gen] record baseline -> gen_test -> out/ ==="

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
# [sim] build + run gtest (with liburma_sim)
############################################
echo ""
echo "=== [sim] gtest (with liburma_sim LD_PRELOAD) ==="

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
                echo "    FAIL (run failure, tail):"
                timeout 30 /tmp/${name}_sim 2>&1 | tail -5 | sed 's/^/    /'
                sfail=$((sfail+1))
            fi
        else
            echo "    COMPILE FAIL"
            sfail=$((sfail+1))
        fi
        rm -f /tmp/${name}_sim
    done
else
    echo "  (skipped, no gtest)"
fi

############################################
# [no-sim] probe real device; build + run gtest (direct urma, no sim)
############################################
echo ""
echo "=== [no-sim] gtest (direct urma, no simulation) ==="

export LD_PRELOAD=""
export URMA_SIM_HW=""

npass=0
nskip=0
nfail=0
nprobe_failed=0
if [ "$NO_GTEST" != "1" ]; then
    # Probe for real hardware before running no-sim suite.
    # Without a device, urma_init fails and every TEST body returns early at
    # the `if (!c) goto cleanup` guard, so gtest reports PASSED with zero
    # assertions executed. We probe first and SKIP the whole suite instead of
    # emitting misleading PASS results.
    echo "  [probe] checking for real urma device..."
    cat > /tmp/probe_dev.cpp <<'EOF'
#include "bl_fixture.hpp"
int main(void) { return probe_device(); }
EOF
    if g++ -std=c++17 -fpermissive -w \
        -I$GTEST_INC -I$BL/gen -I$URMA_INC -I$URMA_COMMON_INC \
        -L$UA_CORE -lurma -L$UA_COMMON -lurma_common \
        -Wl,-rpath,$UA_CORE -Wl,-rpath,$UA_COMMON \
        $BL/gen/bl_fixture.cpp /tmp/probe_dev.cpp \
        -o /tmp/probe_dev 2>&1; then
        timeout 10 /tmp/probe_dev >/dev/null 2>&1
        nprobe_failed=$?
    else
        nprobe_failed=99   # probe compile failure -> treat as no device
    fi
    rm -f /tmp/probe_dev /tmp/probe_dev.cpp

    if [ "$nprobe_failed" != "0" ]; then
        echo "  [probe] no real device (rc=$nprobe_failed); skipping no-sim suite"
        for cpp in $BL/out/*.cpp; do
            name=$(basename "$cpp" .cpp)
            echo "  [no-sim] $name: SKIP"
            nskip=$((nskip+1))
        done
    else
        echo "  [probe] real device detected; running no-sim suite"
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
                    echo "    FAIL (tail):"
                    timeout 30 /tmp/${name}_nosim 2>&1 | tail -5 | sed 's/^/    /'
                    nfail=$((nfail+1))
                fi
            else
                echo "    COMPILE FAIL"
                nfail=$((nfail+1))
            fi
            rm -f /tmp/${name}_nosim
        done
    fi
else
    echo "  (skipped, no gtest)"
fi

############################################
# [check] regression comparison (with sim)
############################################
echo ""
echo "=== [check] regression comparison ==="

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
echo "╔════════════════════════════════════════════════════════════════╗"
echo "  [sim]    gtest:    $spass passed, $sfail failed"
if [ "$nprobe_failed" != "0" ] && [ "$NO_GTEST" != "1" ]; then
    echo "  [no-sim] gtest:    $npass passed, $nskip skipped, $nfail failed  (no real device)"
else
    echo "  [no-sim] gtest:    $npass passed, $nskip skipped, $nfail failed"
fi
echo "  [check]  baseline: $cpass passed, $cfail failed"
echo "╚════════════════════════════════════════════════════════════════╝"
exit $((sfail + nfail + cfail))
