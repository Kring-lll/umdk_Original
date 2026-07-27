#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#
# Minimal simulation DT gate.
#
# Usage:
#   bash test/urma/dt/scripts/run_dt.sh
#   UMDK_DT_SAVE_LOGS=1 bash test/urma/dt/scripts/run_dt.sh
#   UMDK_DT_PRINT_LOGS=0 bash test/urma/dt/scripts/run_dt.sh
#   UMDK_DT_LOG_TAIL_LINES=120 bash test/urma/dt/scripts/run_dt.sh
#   UMDK_DT_CASES=/tmp/my_dt_cases.conf bash test/urma/dt/scripts/run_dt.sh
#   UMDK_DT_SIM_MODE=hw bash test/urma/dt/scripts/run_dt.sh
#   UMDK_DT_SIMULATOR_DIR=/tmp/simulator bash test/urma/dt/scripts/run_dt.sh

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DT_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
UMDK_ROOT=$(cd "$DT_ROOT/../../.." && pwd)

usage()
{
    cat <<EOF
Usage:
  bash test/urma/dt/scripts/run_dt.sh

Common examples:
  bash test/urma/dt/scripts/run_dt.sh
  UMDK_DT_SAVE_LOGS=1 bash test/urma/dt/scripts/run_dt.sh
  UMDK_DT_PRINT_LOGS=0 bash test/urma/dt/scripts/run_dt.sh
  UMDK_DT_LOG_TAIL_LINES=120 bash test/urma/dt/scripts/run_dt.sh
  UMDK_DT_CASES=/tmp/my_dt_cases.conf bash test/urma/dt/scripts/run_dt.sh
  UMDK_DT_SIM_MODE=hw bash test/urma/dt/scripts/run_dt.sh
  UMDK_DT_SIMULATOR_DIR=/tmp/simulator bash test/urma/dt/scripts/run_dt.sh

Environment:
  UMDK_DT_CASES          Case manifest. Default: $DT_ROOT/cases/manifest.conf
  UMDK_DT_SIM_MODE       provider or hw. Default: provider
  UMDK_DT_BUILD_ROOT     Build output root. Default: $UMDK_ROOT/build/dt
  UMDK_DT_SAVE_LOGS      Save logs to files when set to 1. Default: 0
  UMDK_DT_LOG_DIR        Log directory when logs are saved. Default: <build_root>/logs
  UMDK_DT_PRINT_LOGS     Print case stdout when set to 1. Default: 1
  UMDK_DT_LOG_TAIL_LINES Printed stdout tail lines. 0 means full stdout. Default: 0
  UMDK_DT_TIMEOUT        Per-case timeout in seconds. Default: 30
  UMDK_DT_WORK_ROOT      Per-case work root. Default: /tmp/umdk_dt_work
  UMDK_DT_FIXTURE_DIR    Fixture directory. Default: $DT_ROOT/fixture
  UMDK_DT_SIMULATOR_DIR  Simulator source directory. Default: $DT_ROOT/simulator
  UA_CORE                liburma.so directory. Default: $UMDK_ROOT/src/build_noasan/urma/lib/urma/core
  UA_COMMON              liburma_common.so directory. Default: $UMDK_ROOT/src/build_noasan/urma/common
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
        *)
            usage >&2
            exit 2
            ;;
    esac
fi

CASES_FILE=${UMDK_DT_CASES:-$DT_ROOT/cases/manifest.conf}

UA_CORE=${UA_CORE:-$UMDK_ROOT/src/build_noasan/urma/lib/urma/core}
UA_COMMON=${UA_COMMON:-$UMDK_ROOT/src/build_noasan/urma/common}
SIMULATOR_DIR=${UMDK_DT_SIMULATOR_DIR:-$DT_ROOT/simulator}

BUILD_ROOT=${UMDK_DT_BUILD_ROOT:-$UMDK_ROOT/build/dt}
SIM_BUILD=${SIM_BUILD:-$BUILD_ROOT/simulator}
SIM_SO=${SIM_SO:-$SIM_BUILD/liburma_sim.so}
SAVE_LOGS=${UMDK_DT_SAVE_LOGS:-0}
if [ "$SAVE_LOGS" = "1" ]; then
    LOG_DIR=${UMDK_DT_LOG_DIR:-$BUILD_ROOT/logs}
else
    LOG_DIR=$(mktemp -d "${TMPDIR:-/tmp}/umdk_dt_logs.XXXXXX")
fi
SIM_MODE=${UMDK_DT_SIM_MODE:-provider}
mkdir -p "$LOG_DIR"
export UMDK_DT_LOG_DIR=$LOG_DIR

cleanup_logs()
{
    if [ "$SAVE_LOGS" != "1" ]; then
        rm -rf "$LOG_DIR"
    fi
}
trap cleanup_logs EXIT

dump_log_tail()
{
    local title=$1
    local path=$2

    if [ ! -s "$path" ]; then
        return
    fi

    echo "[DT] ---- $title: $path ----"
    tail -n "${UMDK_DT_LOG_TAIL_LINES:-120}" "$path"
}

build_noasan_if_needed()
{
    if [ -f "$UA_CORE/liburma.so" ] && [ -f "$UA_COMMON/liburma_common.so" ]; then
        return 0
    fi

    echo "[DT] build src/build_noasan"
    cmake -S "$UMDK_ROOT/src" -B "$UMDK_ROOT/src/build_noasan" \
        -DBUILD_ALL=disable -DBUILD_URMA=enable -DBUILD_UDMA=disable \
        -DGTEST=disable -DASAN=disable -DCODE_COVERAGE=disable \
        >"$LOG_DIR/build_noasan.cmake.log" 2>&1 || return 1
    cmake --build "$UMDK_ROOT/src/build_noasan" -j \
        >"$LOG_DIR/build_noasan.build.log" 2>&1
}

build_sim()
{
    echo "[DT] build simulator"
    cmake -S "$SIMULATOR_DIR" -B "$SIM_BUILD" \
        -DURMA_BUILD_DIR="$UMDK_ROOT/src/build_noasan" \
        >"$LOG_DIR/sim.cmake.log" 2>&1 || return 1
    rm -f "$SIM_SO"
    cmake --build "$SIM_BUILD" -j >"$LOG_DIR/sim.build.log" 2>&1
}

prepare_provider_shim()
{
    local provider_dir="$UA_CORE/urma"

    mkdir -p "$provider_dir" || return 1
    ln -sfn "$SIM_SO" "$provider_dir/liburma_sim.so"
}

echo "[DT] mode=$SIM_MODE"

if [ ! -f "$CASES_FILE" ]; then
    echo "[DT] missing cases file: $CASES_FILE"
    exit 1
fi

if [ ! -f "$SIMULATOR_DIR/CMakeLists.txt" ]; then
    echo "[DT] missing simulator source: $SIMULATOR_DIR"
    echo "[DT] provide simulator via UMDK_DT_SIMULATOR_DIR or add it under test/urma/dt/simulator"
    exit 1
fi

if ! build_noasan_if_needed; then
    echo "[DT] build_noasan failed, see $LOG_DIR/build_noasan.*.log"
    dump_log_tail "build_noasan cmake log" "$LOG_DIR/build_noasan.cmake.log"
    dump_log_tail "build_noasan build log" "$LOG_DIR/build_noasan.build.log"
    exit 1
fi

if ! build_sim; then
    echo "[DT] sim build failed, see $LOG_DIR/sim.*.log"
    dump_log_tail "sim cmake log" "$LOG_DIR/sim.cmake.log"
    dump_log_tail "sim build log" "$LOG_DIR/sim.build.log"
    exit 1
fi

if [ ! -f "$SIM_SO" ]; then
    echo "[DT] missing simulator: $SIM_SO"
    exit 1
fi

if ! prepare_provider_shim; then
    echo "[DT] failed to prepare simulator provider shim under $UA_CORE/urma"
    exit 1
fi

pass=0
fail=0

while IFS='|' read -r name cpp extra_env; do
    case "$name" in
        ""|\#*) continue ;;
    esac
    if "$SCRIPT_DIR/run_dt_one.sh" "$name" "$cpp" "${extra_env:-}"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
    fi
done < "$CASES_FILE"

echo "[DT] summary: $pass passed, $fail failed"
echo "pass=$pass fail=$fail" > "$LOG_DIR/summary.status"

if [ $fail -ne 0 ]; then
    exit 1
fi
exit 0
