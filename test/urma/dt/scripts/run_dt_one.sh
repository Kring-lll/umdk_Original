#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#
# Build and run one generated DT case under the URMA simulator.
#
# Usage:
#   bash test/urma/dt/scripts/run_dt_one.sh <case_name> <generated_cpp> [extra_runtime_env]
#   bash test/urma/dt/scripts/run_dt_one.sh test_read test/urma/dt/cases/test_read/case.cpp

set -uo pipefail

usage()
{
    cat <<EOF
Usage:
  bash test/urma/dt/scripts/run_dt_one.sh <case_name> <generated_cpp> [extra_runtime_env]

Examples:
  bash test/urma/dt/scripts/run_dt_one.sh test_read test/urma/dt/cases/test_read/case.cpp
  bash test/urma/dt/scripts/run_dt_one.sh test_rnr test/urma/dt/cases/test_rnr/case.cpp URMA_SIM_INJECT_STATUS=4

Environment:
  UMDK_DT_SIM_MODE       provider or hw. Default: provider
  UMDK_DT_BUILD_ROOT     Build output root. Default: <repo>/build/dt
  UMDK_DT_SAVE_LOGS      Save logs to files when set to 1. Default: 0
  UMDK_DT_LOG_DIR        Log directory. Usually exported by run_dt.sh
  UMDK_DT_PRINT_LOGS     Print case stdout when set to 1. Default: 1
  UMDK_DT_LOG_TAIL_LINES Printed stdout tail lines. 0 means full stdout. Default: 0
  UMDK_DT_TIMEOUT        Per-case timeout in seconds. Default: 30
  UMDK_DT_WORK_ROOT      Per-case work root. Default: /tmp/umdk_dt_work
  UMDK_DT_FIXTURE_DIR    Fixture directory. Default: <dt_root>/fixture
  UMDK_DT_SIMULATOR_DIR  Simulator source directory. Default: <dt_root>/simulator
  UA_CORE                liburma.so directory. Default: <repo>/src/build_noasan/urma/lib/urma/core
  UA_COMMON              liburma_common.so directory. Default: <repo>/src/build_noasan/urma/common
  SIM_BUILD              Simulator build directory. Default: <build_root>/simulator
  SIM_SO                 Simulator shared library. Default: <sim_build>/liburma_sim.so
  SIM_CFG                Simulator config json. Default: <simulator_dir>/urma_sim_config.json
EOF
}

if [ $# -gt 0 ]; then
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
    esac
fi

if [ $# -lt 2 ]; then
    usage >&2
    exit 2
fi

CASE_NAME=$1
CASE_CPP=$2
EXTRA_RUNTIME_ENV=${3:-}

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DT_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
UMDK_ROOT=$(cd "$DT_ROOT/../../.." && pwd)

UA_CORE=${UA_CORE:-$UMDK_ROOT/src/build_noasan/urma/lib/urma/core}
UA_COMMON=${UA_COMMON:-$UMDK_ROOT/src/build_noasan/urma/common}
URMA_INC=${URMA_INC:-$UMDK_ROOT/src/urma/lib/urma/core/include}
URMA_COMMON_INC=${URMA_COMMON_INC:-$UMDK_ROOT/src/urma/common/include}
SIMULATOR_DIR=${UMDK_DT_SIMULATOR_DIR:-$DT_ROOT/simulator}
BUILD_ROOT=${UMDK_DT_BUILD_ROOT:-$UMDK_ROOT/build/dt}
SIM_BUILD=${SIM_BUILD:-$BUILD_ROOT/simulator}
SIM_SO=${SIM_SO:-$SIM_BUILD/liburma_sim.so}
SIM_CFG=${SIM_CFG:-$SIMULATOR_DIR/urma_sim_config.json}
FIXTURE_DIR=${UMDK_DT_FIXTURE_DIR:-$DT_ROOT/fixture}

BIN_DIR=$BUILD_ROOT/bin
LOG_DIR=${UMDK_DT_LOG_DIR:-$BUILD_ROOT/logs}
WORK_PARENT=${UMDK_DT_WORK_ROOT:-/tmp/umdk_dt_work}
TIMEOUT_SEC=${UMDK_DT_TIMEOUT:-30}
SIM_MODE=${UMDK_DT_SIM_MODE:-provider}
LOG_TAIL_LINES=${UMDK_DT_LOG_TAIL_LINES:-0}
PRINT_LOGS=${UMDK_DT_PRINT_LOGS:-1}
SAVE_LOGS=${UMDK_DT_SAVE_LOGS:-0}

case "$SIM_MODE" in
    provider)
        SIM_HW=0
        ;;
    hw)
        SIM_HW=1
        ;;
    *)
        echo "unknown UMDK_DT_SIM_MODE=$SIM_MODE, expected provider or hw" >&2
        exit 2
        ;;
esac

mkdir -p "$BIN_DIR" "$LOG_DIR" "$WORK_PARENT"

if [[ "$CASE_CPP" != /* ]]; then
    CASE_CPP="$UMDK_ROOT/$CASE_CPP"
fi

if [ ! -f "$CASE_CPP" ]; then
    echo "[DT] missing case source: $CASE_CPP"
    exit 1
fi

if [ ! -f "$FIXTURE_DIR/bl_fixture.cpp" ]; then
    echo "[DT] missing fixture source: $FIXTURE_DIR/bl_fixture.cpp"
    exit 1
fi

if [ ! -f "$SIM_SO" ]; then
    echo "[DT] missing simulator library: $SIM_SO"
    echo "[DT] run run_dt.sh first or provide SIM_SO explicitly"
    exit 1
fi

if [ ! -f "$SIM_CFG" ]; then
    echo "[DT] missing simulator config: $SIM_CFG"
    echo "[DT] provide SIM_CFG explicitly or add simulator config under UMDK_DT_SIMULATOR_DIR"
    exit 1
fi

EXE=$BIN_DIR/${CASE_NAME}_run
COMPILE_LOG=$LOG_DIR/${CASE_NAME}.compile.log
STDOUT_LOG=$LOG_DIR/${CASE_NAME}.stdout
STDERR_LOG=$LOG_DIR/${CASE_NAME}.stderr
XML_LOG=$LOG_DIR/${CASE_NAME}.xml
STATUS_LOG=$LOG_DIR/${CASE_NAME}.status
WORKDIR_LOG=$LOG_DIR/${CASE_NAME}.workdir

rm -f "$EXE" "$COMPILE_LOG" "$STDOUT_LOG" "$STDERR_LOG" "$XML_LOG" "$STATUS_LOG" "$WORKDIR_LOG"

dump_log_tail()
{
    local title=$1
    local path=$2

    if [ ! -s "$path" ]; then
        return
    fi

    echo "[DT] ---- $title: $path ----"
    if [ "$LOG_TAIL_LINES" = "0" ]; then
        cat "$path"
    else
        tail -n "$LOG_TAIL_LINES" "$path"
    fi
}

dump_case_logs()
{
    if [ "$PRINT_LOGS" != "1" ]; then
        return
    fi

    dump_log_tail "stdout" "$STDOUT_LOG"
}

cleanup_case_logs()
{
    if [ "$SAVE_LOGS" = "1" ]; then
        return
    fi

    rm -f "$COMPILE_LOG" "$STDOUT_LOG" "$STDERR_LOG" "$XML_LOG" "$STATUS_LOG" "$WORKDIR_LOG"
}

echo "[DT] compile $CASE_NAME"
if ! env -u LD_PRELOAD -u URMA_SIM_CONFIG -u URMA_SIM_HW -u URMA_SIM_WORKDIR -u URMA_SIM_IPC_DIR \
    g++ -std=c++17 -fpermissive -Wno-error \
        -I/usr/local/include -I"$FIXTURE_DIR" -I"$URMA_INC" -I"$URMA_COMMON_INC" \
        "$FIXTURE_DIR/bl_fixture.cpp" "$CASE_CPP" \
        -L"$SIM_BUILD" -lurma_sim \
        -L"$UA_CORE" -lurma -L"$UA_COMMON" -lurma_common \
        -Wl,-rpath,"$SIM_BUILD" -Wl,-rpath,"$UA_CORE" -Wl,-rpath,"$UA_COMMON" \
        -L/usr/local/lib64 -lgtest_main -lgtest -lpthread \
        -o "$EXE" >"$COMPILE_LOG" 2>&1; then
    echo "COMPILE_FAIL" > "$STATUS_LOG"
    echo "[DT] $CASE_NAME COMPILE_FAIL, see $COMPILE_LOG"
    dump_log_tail "compile log" "$COMPILE_LOG"
    cleanup_case_logs
    exit 1
fi

RUN_ROOT=$(mktemp -d "$WORK_PARENT/${CASE_NAME}.XXXXXX")
mkdir -p "$RUN_ROOT/ipc"

read -r -a EXTRA_ENV_ARRAY <<< "$EXTRA_RUNTIME_ENV"

echo "[DT] run $CASE_NAME mode=$SIM_MODE"
env \
    LD_LIBRARY_PATH="$UA_CORE:$UA_COMMON:${LD_LIBRARY_PATH:-}" \
    LD_PRELOAD="$SIM_SO" \
    URMA_SIM_CONFIG="$SIM_CFG" \
    URMA_SIM_HW="$SIM_HW" \
    URMA_SIM_WORKDIR="$RUN_ROOT" \
    URMA_SIM_IPC_DIR="$RUN_ROOT/ipc" \
    "${EXTRA_ENV_ARRAY[@]}" \
    timeout "$TIMEOUT_SEC" "$EXE" --gtest_output=xml:"$XML_LOG" \
    >"$STDOUT_LOG" 2>"$STDERR_LOG"
RC=$?

if [ $RC -eq 0 ]; then
    if grep -qE "\\[setup\\].*(num_devs=0|dev idx|fail)" "$STDERR_LOG"; then
        echo "INFRA_FAIL" > "$STATUS_LOG"
        echo "[DT] $CASE_NAME INFRA_FAIL, setup did not reach a usable device, workdir: $RUN_ROOT"
        dump_case_logs
        echo "$RUN_ROOT" > "$WORKDIR_LOG"
        cleanup_case_logs
        exit 1
    fi
    echo "PASS" > "$STATUS_LOG"
    if [ "${KEEP_DT_WORKDIR:-0}" != "1" ]; then
        rm -rf "$RUN_ROOT"
    else
        echo "$RUN_ROOT" > "$WORKDIR_LOG"
    fi
    dump_case_logs
    echo "[DT] $CASE_NAME PASS"
    cleanup_case_logs
    exit 0
fi

if [ $RC -eq 124 ]; then
    echo "TIMEOUT" > "$STATUS_LOG"
    echo "[DT] $CASE_NAME TIMEOUT, workdir: $RUN_ROOT"
else
    echo "FAIL" > "$STATUS_LOG"
    echo "[DT] $CASE_NAME FAIL rc=$RC, workdir: $RUN_ROOT"
fi
dump_case_logs
echo "$RUN_ROOT" > "$WORKDIR_LOG"
cleanup_case_logs
exit $RC
