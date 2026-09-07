#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
#
# Minimal simulation DT gate.
#
# Usage:
#   bash test/urma/dt/scripts/run_dt.sh
#   URMA_DT_SAVE_LOGS=1 bash test/urma/dt/scripts/run_dt.sh
#   URMA_DT_PRINT_LOGS=0 bash test/urma/dt/scripts/run_dt.sh
#   URMA_DT_LOG_TAIL_LINES=120 bash test/urma/dt/scripts/run_dt.sh
#   URMA_DT_CASES=/tmp/my_dt_cases.conf bash test/urma/dt/scripts/run_dt.sh
#   URMA_DT_SIM_MODE=hw bash test/urma/dt/scripts/run_dt.sh
#   URMA_DT_BUILD_ROOT=/tmp/umdk_dt_build bash test/urma/dt/scripts/run_dt.sh
#   URMA_DT_SIMULATOR_DIR=/tmp/simulator bash test/urma/dt/scripts/run_dt.sh

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DT_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
UMDK_ROOT=$(cd "$DT_ROOT/../../.." && pwd)
. "$SCRIPT_DIR/dt_common.sh"

usage()
{
    cat <<EOF
Usage:
  bash test/urma/dt/scripts/run_dt.sh

Common examples:
  bash test/urma/dt/scripts/run_dt.sh
  URMA_DT_SAVE_LOGS=1 bash test/urma/dt/scripts/run_dt.sh
  URMA_DT_PRINT_LOGS=0 bash test/urma/dt/scripts/run_dt.sh
  URMA_DT_LOG_TAIL_LINES=120 bash test/urma/dt/scripts/run_dt.sh
  URMA_DT_CASES=/tmp/my_dt_cases.conf bash test/urma/dt/scripts/run_dt.sh
  URMA_DT_SIM_MODE=hw bash test/urma/dt/scripts/run_dt.sh
  URMA_DT_BUILD_ROOT=/tmp/umdk_dt_build bash test/urma/dt/scripts/run_dt.sh
  URMA_DT_SIMULATOR_DIR=/tmp/simulator bash test/urma/dt/scripts/run_dt.sh

Environment:
  URMA_DT_CASES          Case manifest. Default: $DT_ROOT/cases/manifest.conf
                         Each line may be either "name" or "name|case.cpp|extra_env".
  URMA_DT_SIM_MODE       hw or provider. Default: hw
  URMA_DT_BUILD_ROOT     Build output root. Default: $UMDK_ROOT/build/dt
  URMA_DT_SRC_BUILD      Non-ASan UMDK build dir. Default: <build_root>/src_noasan
  URMA_DT_REBUILD        Remove and rebuild URMA_DT_SRC_BUILD when set to 1. Default: 0
  URMA_DT_SAVE_LOGS      Save logs to files when set to 1. Default: 0
  URMA_DT_LOG_DIR        Log directory when logs are saved. Default: <build_root>/logs
  URMA_DT_PRINT_LOGS     Print case stdout when set to 1. Default: 1
  URMA_DT_LOG_TAIL_LINES Printed stdout tail lines. 0 means full stdout. Default: 0
  URMA_DT_TIMEOUT        Per-case timeout in seconds. Default: 30
  URMA_DT_WORK_ROOT      Per-case work root. Default: /tmp/umdk_dt_work
  URMA_DT_FIXTURE_DIR    Fixture directory. Default: $DT_ROOT/fixture
  URMA_DT_SIMULATOR_DIR  Simulator source directory. Default: $DT_ROOT/simulator
  URMA_DT_PROVIDER_DIR   Provider directory for hw mode. Default: /usr/lib64/urma
  UA_CORE                liburma.so directory. Default: <src_build>/urma/lib/urma/core
  UA_COMMON              liburma_common.so directory. Default: <src_build>/urma/common
  UDMA_SO                Real udma provider. Default: <src_build>/urma/hw/udma/liburma-udma.so
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

CASES_FILE=${URMA_DT_CASES:-$DT_ROOT/cases/manifest.conf}

BUILD_ROOT=${URMA_DT_BUILD_ROOT:-$UMDK_ROOT/build/dt}
SRC_BUILD=${URMA_DT_SRC_BUILD:-$BUILD_ROOT/src_noasan}
UA_CORE=${UA_CORE:-$SRC_BUILD/urma/lib/urma/core}
UA_COMMON=${UA_COMMON:-$SRC_BUILD/urma/common}
UDMA_SO=${UDMA_SO:-$SRC_BUILD/urma/hw/udma/liburma-udma.so}
SIMULATOR_DIR=${URMA_DT_SIMULATOR_DIR:-$DT_ROOT/simulator}

SIM_BUILD=${SIM_BUILD:-$BUILD_ROOT/simulator}
SIM_SO=${SIM_SO:-$SIM_BUILD/liburma_sim.so}
SIM_CFG=${SIM_CFG:-$SIMULATOR_DIR/urma_sim_config.json}
CORE_FP_FILE=$BUILD_ROOT/.dt_core_fingerprint
SIM_FP_FILE=$BUILD_ROOT/.dt_sim_fingerprint
PROVIDER_DIR=${URMA_DT_PROVIDER_DIR:-/usr/lib64/urma}
PROVIDER_SHIM=
PROVIDER_DIR_CREATED=0
PROVIDER_SHIM_CREATED=0
REAL_PROVIDER_FILE=
REAL_PROVIDER_CREATED=0
SAVE_LOGS=${URMA_DT_SAVE_LOGS:-0}
if [ "$SAVE_LOGS" = "1" ]; then
    LOG_DIR=${URMA_DT_LOG_DIR:-$BUILD_ROOT/logs}
else
    LOG_DIR=$(mktemp -d "${TMPDIR:-/tmp}/umdk_dt_logs.XXXXXX")
fi
SIM_MODE=${URMA_DT_SIM_MODE:-hw}
mkdir -p "$LOG_DIR"
export URMA_DT_LOG_DIR=$LOG_DIR

cleanup_logs()
{
    if [ "$SAVE_LOGS" != "1" ]; then
        rm -rf "$LOG_DIR"
    fi
}

cleanup_provider_shim()
{
    if [ "$REAL_PROVIDER_CREATED" = "1" ] && [ -n "$REAL_PROVIDER_FILE" ]; then
        rm -f "$REAL_PROVIDER_FILE"
    fi
    if [ "$PROVIDER_SHIM_CREATED" = "1" ] && [ -n "$PROVIDER_SHIM" ]; then
        rm -f "$PROVIDER_SHIM"
    fi
    if [ "$PROVIDER_DIR_CREATED" = "1" ] && [ -n "$PROVIDER_DIR" ]; then
        rmdir "$PROVIDER_DIR" 2>/dev/null || true
    fi
}

cleanup_dt()
{
    cleanup_provider_shim
    cleanup_logs
}
trap cleanup_dt EXIT

dump_log_tail()
{
    local title=$1
    local path=$2

    if [ ! -s "$path" ]; then
        return
    fi

    echo "[DT] ---- $title: $path ----"
    tail -n "${URMA_DT_LOG_TAIL_LINES:-120}" "$path"
}

preflight_hw_mode()
{
    local arch

    arch=$(uname -m)
    case "$arch" in
        aarch64|arm64)
            ;;
        *)
            echo "[DT] hw mode requires an aarch64 environment, current arch: $arch"
            echo "[DT] use the openEuler aarch64 container when running real udma provider DT"
            return 1
            ;;
    esac

    if [ ! -f /usr/include/ummu_api.h ]; then
        echo "[DT] missing /usr/include/ummu_api.h"
        echo "[DT] install libummu-devel in the container before running hw mode"
        return 1
    fi
}

build_src_if_needed()
{
    local udma_flag=disable
    local build_target=
    local core_fp=""
    local need_rebuild=0

    if [ "$SIM_MODE" = "hw" ]; then
        udma_flag=enable
        build_target="--target BETA_UDMA_SO"
    fi

    core_fp=$(dt_source_fingerprint "$UMDK_ROOT/src/urma" "$UMDK_ROOT/src/CMakeLists.txt")

    if [ "${URMA_DT_REBUILD:-0}" = "1" ]; then
        echo "[DT] rebuild requested, removing $SRC_BUILD"
        rm -rf "$SRC_BUILD"
        need_rebuild=1
    fi

    if ! dt_fingerprint_matches "$CORE_FP_FILE" "$core_fp"; then
        echo "[DT] source fingerprint changed/missing, rebuilding $SRC_BUILD"
        need_rebuild=1
    fi

    if [ $need_rebuild -eq 0 ] && [ -f "$UA_CORE/liburma.so" ] && [ -f "$UA_COMMON/liburma_common.so" ] &&
        { [ "$SIM_MODE" != "hw" ] || [ -f "$UDMA_SO" ]; }; then
        return 0
    fi

    if [ $need_rebuild -eq 1 ] && [ -d "$SRC_BUILD" ]; then
        rm -rf "$SRC_BUILD"
    fi

    echo "[DT] build UMDK non-ASan tree: $SRC_BUILD (BUILD_UDMA=$udma_flag)"
    cmake -S "$UMDK_ROOT/src" -B "$SRC_BUILD" \
        -DBUILD_ALL=disable -DBUILD_URMA=enable -DBUILD_UDMA="$udma_flag" \
        -DGTEST=disable -DASAN=disable -DCODE_COVERAGE=disable \
        >"$LOG_DIR/build_noasan.cmake.log" 2>&1 || return 1

    # shellcheck disable=SC2086
    cmake --build "$SRC_BUILD" $build_target -j \
        >"$LOG_DIR/build_noasan.build.log" 2>&1 || return 1

    dt_fingerprint_save "$CORE_FP_FILE" "$core_fp"
    return 0
}

build_sim()
{
    local sim_fp=""
    local core_fp=""
    local need_rebuild=0

    sim_fp=$(dt_source_fingerprint "$SIMULATOR_DIR")
    core_fp=$(dt_source_fingerprint "$UMDK_ROOT/src/urma" "$UMDK_ROOT/src/CMakeLists.txt")

    if [ "${URMA_DT_REBUILD:-0}" = "1" ]; then
        rm -rf "$SIM_BUILD"
        need_rebuild=1
    fi

    if ! dt_fingerprint_matches "$SIM_FP_FILE" "$sim_fp" ||
       ! dt_fingerprint_matches "$CORE_FP_FILE" "$core_fp"; then
        echo "[DT] simulator source fingerprint changed/missing, rebuilding $SIM_BUILD"
        need_rebuild=1
    fi

    if [ $need_rebuild -eq 0 ] && [ -f "$SIM_SO" ]; then
        return 0
    fi

    if [ $need_rebuild -eq 1 ] && [ -d "$SIM_BUILD" ]; then
        rm -rf "$SIM_BUILD"
    fi

    echo "[DT] build simulator"
    cmake -S "$SIMULATOR_DIR" -B "$SIM_BUILD" \
        -DURMA_BUILD_DIR="$SRC_BUILD" \
        -DURMA_SRC_DIR="$UMDK_ROOT/src/urma" \
        >"$LOG_DIR/sim.cmake.log" 2>&1 || return 1
    cmake --build "$SIM_BUILD" -j >"$LOG_DIR/sim.build.log" 2>&1 || return 1

    dt_fingerprint_save "$SIM_FP_FILE" "$sim_fp"
    return 0
}

prepare_sim_provider_shim()
{
    PROVIDER_DIR="$UA_CORE/urma"
    PROVIDER_SHIM="$PROVIDER_DIR/liburma_sim.so"

    if [ ! -d "$PROVIDER_DIR" ]; then
        PROVIDER_DIR_CREATED=1
        mkdir -p "$PROVIDER_DIR" || return 1
    fi

    if [ -e "$PROVIDER_SHIM" ] || [ -L "$PROVIDER_SHIM" ]; then
        echo "[DT] provider shim already exists: $PROVIDER_SHIM"
        echo "[DT] not overwriting existing provider; use a clean UA_CORE or remove it explicitly"
        return 1
    fi

    ln -s "$SIM_SO" "$PROVIDER_SHIM" || return 1
    PROVIDER_SHIM_CREATED=1
}

prepare_real_provider()
{
    REAL_PROVIDER_FILE="$PROVIDER_DIR/liburma-udma.so"

    if [ ! -f "$UDMA_SO" ]; then
        echo "[DT] missing real udma provider build output: $UDMA_SO"
        return 1
    fi

    if [ ! -d "$PROVIDER_DIR" ]; then
        PROVIDER_DIR_CREATED=1
        mkdir -p "$PROVIDER_DIR" || return 1
    fi

    if [ -e "$REAL_PROVIDER_FILE" ] || [ -L "$REAL_PROVIDER_FILE" ]; then
        if [ "$(readlink -f "$REAL_PROVIDER_FILE" 2>/dev/null)" = "$(readlink -f "$UDMA_SO")" ]; then
            echo "[DT] real provider already staged: $REAL_PROVIDER_FILE"
            return 0
        fi

        echo "[DT] real provider already exists: $REAL_PROVIDER_FILE"
        echo "[DT] skip provider staging (managed by build_dt_env.sh)"
        return 0
    fi

    ln -s "$UDMA_SO" "$REAL_PROVIDER_FILE" || return 1
    REAL_PROVIDER_CREATED=1
    echo "[DT] staged real provider: $REAL_PROVIDER_FILE -> $UDMA_SO"
}

echo "[DT] mode=$SIM_MODE"

if [ ! -f "$CASES_FILE" ]; then
    echo "[DT] missing cases file: $CASES_FILE"
    exit 1
fi

if [ ! -f "$SIMULATOR_DIR/CMakeLists.txt" ]; then
    echo "[DT] missing simulator source: $SIMULATOR_DIR"
    echo "[DT] provide simulator via URMA_DT_SIMULATOR_DIR or add it under test/urma/dt/simulator"
    exit 1
fi

case "$SIM_MODE" in
    hw)
        if ! preflight_hw_mode; then
            exit 1
        fi
        ;;
    provider)
        ;;
    *)
        echo "[DT] unknown URMA_DT_SIM_MODE=$SIM_MODE, expected hw or provider"
        exit 2
        ;;
esac

if ! build_src_if_needed; then
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

if [ "$SIM_MODE" = "hw" ]; then
    if ! prepare_real_provider; then
        echo "[DT] failed to prepare real udma provider under $PROVIDER_DIR"
        exit 1
    fi
else
    if ! prepare_sim_provider_shim; then
        echo "[DT] failed to prepare simulator provider shim under $UA_CORE/urma"
        exit 1
    fi
fi

echo "[DT] ua_core=$UA_CORE"
echo "[DT] ua_common=$UA_COMMON"
echo "[DT] simulator=$SIM_SO"
if [ "$SIM_MODE" = "hw" ]; then
    echo "[DT] real_provider=$REAL_PROVIDER_FILE"
else
    echo "[DT] sim_provider_shim=$PROVIDER_SHIM"
fi

export URMA_DT_SIM_MODE="$SIM_MODE"
export URMA_DT_BUILD_ROOT="$BUILD_ROOT"
export URMA_DT_SRC_BUILD="$SRC_BUILD"
export URMA_DT_SIMULATOR_DIR="$SIMULATOR_DIR"
export URMA_DT_REAL_PROVIDER="$REAL_PROVIDER_FILE"
export UA_CORE UA_COMMON UDMA_SO SIM_BUILD SIM_SO SIM_CFG

pass=0
fail=0

while IFS='|' read -r name cpp extra_env; do
    case "$name" in
        ""|\#*) continue ;;
    esac
    if [ -z "${cpp:-}" ]; then
        cpp="test/urma/dt/cases/${name}.cpp"
    fi
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
