#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# Description: Unified coverage collection and threshold check script

set -e

SCRIPT_PATH=$(cd $(dirname $0);pwd)
PROJECT_ROOT=$(cd $SCRIPT_PATH/..;pwd)
COVERAGE_CONF="$SCRIPT_PATH/coverage.conf"

LINE_THRESHOLD=70
BRANCH_THRESHOLD=60
OUTPUT_DIR="$PROJECT_ROOT/test/coverage_reports"

parse_config() {
    if [ -f "$COVERAGE_CONF" ]; then
        LINE_THRESHOLD=$(grep "line_coverage_threshold" "$COVERAGE_CONF" | cut -d'=' -f2 | tr -d ' ' || echo "70")
        BRANCH_THRESHOLD=$(grep "branch_coverage_threshold" "$COVERAGE_CONF" | cut -d'=' -f2 | tr -d ' ' || echo "60")
        OUTPUT_DIR=$(grep "output_dir" "$COVERAGE_CONF" | cut -d'=' -f2 | tr -d ' ' || echo "$PROJECT_ROOT/test/coverage_reports")
    fi
}

collect_coverage() {
    local module=$1
    local build_dir="$PROJECT_ROOT/src/build"
    
    echo "Collecting coverage for module: $module"
    
    if [ -d "$build_dir" ]; then
        cd "$build_dir"
        lcov --capture --directory . --output-file "${module}_coverage.info" \
            --rc lcov_branch_coverage=1 \
            --ignore-errors source || true
        
        lcov --remove "${module}_coverage.info" \
            '*test*' '*mock*' '*example*' '*third_party*' '*c++/*' '*gtest/*' \
            --output-file "${module}_filtered.info" \
            --rc lcov_branch_coverage=1 || true
    fi
}

check_threshold() {
    local info_file=$1
    local module=$2
    
    if [ ! -f "$info_file" ]; then
        echo "Warning: No coverage info file for $module"
        return 0
    fi
    
    local line_cov=$(lcov --summary "$info_file" 2>&1 | grep "lines" | grep -oP '\d+\.\d+' | head -1 || echo "0")
    local branch_cov=$(lcov --summary "$info_file" 2>&1 | grep "branches" | grep -oP '\d+\.\d+' | head -1 || echo "0")
    
    echo "Module: $module"
    echo "  Line coverage: ${line_cov}% (threshold: ${LINE_THRESHOLD}%)"
    echo "  Branch coverage: ${branch_cov}% (threshold: ${BRANCH_THRESHOLD}%)"
    
    local pass=true
    
    if [ "$(echo "$line_cov < $LINE_THRESHOLD" | bc -l 2>/dev/null || echo "0")" -eq 1 ]; then
        echo "  FAIL: Line coverage below threshold!"
        pass=false
    fi
    
    if [ "$(echo "$branch_cov < $BRANCH_THRESHOLD" | bc -l 2>/dev/null || echo "0")" -eq 1 ]; then
        echo "  FAIL: Branch coverage below threshold!"
        pass=false
    fi
    
    if [ "$pass" = true ]; then
        echo "  PASS: Coverage meets threshold"
        return 0
    else
        return 1
    fi
}

generate_report() {
    mkdir -p "$OUTPUT_DIR"
    
    cd "$PROJECT_ROOT/src/build"
    
    if ls *_filtered.info 1>/dev/null 2>&1; then
        lcov --add-tracefile *_filtered.info --output-file total_coverage.info \
            --rc lcov_branch_coverage=1 || true
        
        genhtml total_coverage.info --output-directory "$OUTPUT_DIR/html" \
            --rc lcov_branch_coverage=1 \
            --title "UMDK Coverage Report" || true
        
        echo "Coverage report generated at: $OUTPUT_DIR/html"
    fi
}

main() {
    parse_config
    
    echo "========================================"
    echo "UMDK Coverage Collection and Check"
    echo "========================================"
    echo "Line threshold: ${LINE_THRESHOLD}%"
    echo "Branch threshold: ${BRANCH_THRESHOLD}%"
    echo ""
    
    local modules="urpc urma dlock"
    local overall_pass=true
    
    for module in $modules; do
        collect_coverage "$module"
        
        if [ -f "$PROJECT_ROOT/src/build/${module}_filtered.info" ]; then
            if ! check_threshold "$PROJECT_ROOT/src/build/${module}_filtered.info" "$module"; then
                overall_pass=false
            fi
        fi
    done
    
    generate_report
    
    echo ""
    echo "========================================"
    if [ "$overall_pass" = true ]; then
        echo "Coverage check: PASSED"
        exit 0
    else
        echo "Coverage check: FAILED"
        exit 1
    fi
}

main "$@"