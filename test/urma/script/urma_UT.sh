#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
#
set -e

SCRIPT_PATH=$(cd "$(dirname "$0")"; pwd)
SRC_PATH=$(cd "$SCRIPT_PATH/../../../src"; pwd)
TEST_PATH=$(cd "$SCRIPT_PATH/.."; pwd)
SRC_BUILD_PATH="$SRC_PATH/build"
REPORTS_PATH="$TEST_PATH/reports"
DEPENDENCIES_PATH="$TEST_PATH/dependencies"

function check_phase1_coverage()
{
    local phase_info="$REPORTS_PATH/phase1.info"
    local summary
    local line_rate
    local function_rate

    # Phase1 gates only deterministic unit-testable files. Hardware, provider, sysfs and netlink paths
    # are intentionally kept out of this denominator until they have stable seams.
    lcov --extract "$REPORTS_PATH/filtered.info" \
        '*/common/ub_bitmap.c' \
        '*/common/ub_hash.c' \
        '*/common/ub_hmap.c' \
        '*/common/ub_dstring.c' \
        '*/common/ub_util.c' \
        '*/lib/urma/core/ubagg_stats.c' \
        '*/lib/urma/core/urma_cmd_tlv.c' \
        '*/lib/urma/bond/bondp_wr_buf.c' \
        '*/lib/urma/bond/utils/bondp_slide_window.c' \
        '*/lib/urma/bond/bondp_datapath_convert.c' \
        '*/lib/urma/bond/bondp_datapath_schedule.c' \
        --output-file "$phase_info" --branch-coverage

    summary=$(lcov --summary "$phase_info" --branch-coverage)
    echo "$summary"
    line_rate=$(echo "$summary" | awk -F '[:%]' '/lines\.\.\./ {gsub(/ /, "", $2); print $2}')
    function_rate=$(echo "$summary" | awk -F '[:%]' '/functions\.\.\./ {gsub(/ /, "", $2); print $2}')

    awk -v line_rate="$line_rate" -v function_rate="$function_rate" 'BEGIN {
        if (function_rate + 0 < 100 || line_rate + 0 < 80) {
            exit 1;
        }
    }' || {
        echo "ERROR: phase1 coverage gate failed: functions=${function_rate}%, lines=${line_rate}%"
        return 1
    }
}

function start_test()
{
    echo "$SCRIPT_PATH"
    mkdir -p "$SRC_BUILD_PATH"
    cd "$SRC_BUILD_PATH"
    cmake .. -DBUILD_ALL=disable -DBUILD_URMA=enable -DBUILD_UDMA=disable \
        -DGTEST=enable -DCODE_COVERAGE=enable -DASAN=enable
    make -j

    if [ -d "$REPORTS_PATH" ]; then
        rm -r "$REPORTS_PATH"
    fi
    mkdir -p "$REPORTS_PATH"

    if [ -d "$DEPENDENCIES_PATH" ]; then
        rm -r "$DEPENDENCIES_PATH"
    fi
    mkdir -p "$DEPENDENCIES_PATH"
    find . -name '*.so*' -exec cp {} "$DEPENDENCIES_PATH" \;

    cd "$TEST_PATH"
    mkdir -p build
    cd build
    cmake .. -D URMA_SRC_DIR="$SRC_PATH/urma" -D URMA_BUILD_DIR="$SRC_BUILD_PATH"
    make -j
    export LD_LIBRARY_PATH="$DEPENDENCIES_PATH:${LD_LIBRARY_PATH:-}"
    ctest --output-on-failure --output-junit "$REPORTS_PATH/report.xml"

    if command -v lcov >/dev/null 2>&1 && command -v genhtml >/dev/null 2>&1; then
        cd "$SRC_BUILD_PATH"
        lcov --capture --directory . --output-file "$REPORTS_PATH/coverage.info" --branch-coverage
        lcov --remove "$REPORTS_PATH/coverage.info" '*c++/*' '*test/urma/*' '*gtest/*' '*mockcpp/*' \
            --output-file "$REPORTS_PATH/filtered.info" --branch-coverage --ignore-errors unused,unused
        check_phase1_coverage
        genhtml "$REPORTS_PATH/filtered.info" --output-directory "$REPORTS_PATH/lcov_report" \
            --branch-coverage
    else
        echo "WARN: lcov/genhtml not found, skip coverage html report"
    fi

    echo -e "\033[32mSUCCESS\033[0m"
    date

    exit 0
}

start_test
