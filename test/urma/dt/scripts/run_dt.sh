#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DT_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
UMDK_ROOT=$(cd "$DT_ROOT/../../.." && pwd)

usage() { echo "Usage: bash test/urma/dt/scripts/run_dt.sh"; echo "See test/urma/dt/docs/dt_layout_plan.md."; }
case "${1:-}" in -h|--help) usage; exit 0 ;; "") ;; *) usage >&2; exit 2 ;; esac

CASES_FILE=${UMDK_DT_CASES:-$DT_ROOT/cases/manifest.conf}
UA_CORE=${UA_CORE:-$UMDK_ROOT/src/build_noasan/urma/lib/urma/core}
UA_COMMON=${UA_COMMON:-$UMDK_ROOT/src/build_noasan/urma/common}
SIMULATOR_DIR=${UMDK_DT_SIMULATOR_DIR:-$DT_ROOT/simulator}
BUILD_ROOT=${UMDK_DT_BUILD_ROOT:-$UMDK_ROOT/build/dt}
SIM_BUILD=${SIM_BUILD:-$BUILD_ROOT/simulator}
SIM_SO=${SIM_SO:-$SIM_BUILD/liburma_sim.so}
PROVIDER_DIR=
PROVIDER_SHIM=
PROVIDER_DIR_CREATED=0
PROVIDER_SHIM_CREATED=0
SAVE_LOGS=${UMDK_DT_SAVE_LOGS:-0}
SIM_MODE=${UMDK_DT_SIM_MODE:-provider}

if [ "$SAVE_LOGS" = "1" ]; then
    LOG_DIR=${UMDK_DT_LOG_DIR:-$BUILD_ROOT/logs}
else
    LOG_DIR=$(mktemp -d "${TMPDIR:-/tmp}/umdk_dt_logs.XXXXXX")
fi
mkdir -p "$LOG_DIR"
export UMDK_DT_LOG_DIR=$LOG_DIR

cleanup_dt()
{
    [ "$PROVIDER_SHIM_CREATED" = "1" ] && [ -n "$PROVIDER_SHIM" ] && rm -f "$PROVIDER_SHIM"
    [ "$PROVIDER_DIR_CREATED" = "1" ] && [ -n "$PROVIDER_DIR" ] && rmdir "$PROVIDER_DIR" 2>/dev/null || true
    [ "$SAVE_LOGS" = "1" ] || rm -rf "$LOG_DIR"
}
trap cleanup_dt EXIT

dump_log_tail()
{
    local title=$1 path=$2
    [ -s "$path" ] || return
    echo "[DT] ---- $title: $path ----"
    tail -n "${UMDK_DT_LOG_TAIL_LINES:-120}" "$path"
}

fail_step()
{
    local step=$1
    echo "[DT] $step failed, see $LOG_DIR/$step.*.log"
    dump_log_tail "$step cmake log" "$LOG_DIR/$step.cmake.log"
    dump_log_tail "$step build log" "$LOG_DIR/$step.build.log"
    exit 1
}

build_noasan_if_needed()
{
    [ -f "$UA_CORE/liburma.so" ] && [ -f "$UA_COMMON/liburma_common.so" ] && return 0
    echo "[DT] build src/build_noasan"
    cmake -S "$UMDK_ROOT/src" -B "$UMDK_ROOT/src/build_noasan" \
        -DBUILD_ALL=disable -DBUILD_URMA=enable -DBUILD_UDMA=disable \
        -DGTEST=disable -DASAN=disable -DCODE_COVERAGE=disable \
        >"$LOG_DIR/build_noasan.cmake.log" 2>&1 || return 1
    cmake --build "$UMDK_ROOT/src/build_noasan" -j >"$LOG_DIR/build_noasan.build.log" 2>&1
}

build_sim()
{
    echo "[DT] build simulator"
    cmake -S "$SIMULATOR_DIR" -B "$SIM_BUILD" \
        -DURMA_BUILD_DIR="$UMDK_ROOT/src/build_noasan" \
        >"$LOG_DIR/sim.cmake.log" 2>&1 || return 1
    cmake --build "$SIM_BUILD" -j >"$LOG_DIR/sim.build.log" 2>&1
}

prepare_provider_shim()
{
    PROVIDER_DIR="$UA_CORE/urma"
    PROVIDER_SHIM="$PROVIDER_DIR/liburma_sim.so"
    if [ ! -d "$PROVIDER_DIR" ]; then PROVIDER_DIR_CREATED=1; mkdir -p "$PROVIDER_DIR" || return 1; fi
    if [ -e "$PROVIDER_SHIM" ] || [ -L "$PROVIDER_SHIM" ]; then
        echo "[DT] provider shim already exists: $PROVIDER_SHIM"
        echo "[DT] not overwriting existing provider; use a clean UA_CORE or remove it explicitly"
        return 1
    fi
    ln -s "$SIM_SO" "$PROVIDER_SHIM" || return 1
    PROVIDER_SHIM_CREATED=1
}

echo "[DT] mode=$SIM_MODE"
[ -f "$CASES_FILE" ] || { echo "[DT] missing cases file: $CASES_FILE"; exit 1; }
if [ ! -f "$SIMULATOR_DIR/CMakeLists.txt" ]; then
    echo "[DT] missing simulator source: $SIMULATOR_DIR"
    echo "[DT] provide simulator via UMDK_DT_SIMULATOR_DIR or add it under test/urma/dt/simulator"
    exit 1
fi

build_noasan_if_needed || fail_step build_noasan
build_sim || fail_step sim
[ -f "$SIM_SO" ] || { echo "[DT] missing simulator: $SIM_SO"; exit 1; }
prepare_provider_shim || { echo "[DT] failed to prepare simulator provider shim under $UA_CORE/urma"; exit 1; }

pass=0
fail=0
while IFS='|' read -r name cpp extra_env; do
    case "$name" in ""|\#*) continue ;; esac
    if "$SCRIPT_DIR/run_dt_one.sh" "$name" "$cpp" "${extra_env:-}"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
    fi
done < "$CASES_FILE"

echo "[DT] summary: $pass passed, $fail failed"
echo "pass=$pass fail=$fail" > "$LOG_DIR/summary.status"
[ $fail -eq 0 ]
