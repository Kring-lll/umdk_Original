#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# Description: Environment check and validation script for UMDK testing

set -e

SCRIPT_PATH=$(cd $(dirname $0);pwd)
PROJECT_ROOT=$(cd $SCRIPT_PATH/..;pwd)

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

print_header() {
    echo ""
    echo "========================================"
    echo "UMDK Environment Check"
    echo "========================================"
}

check_command() {
    local cmd=$1
    local package=$2
    
    if command -v "$cmd" &>/dev/null; then
        echo -e "${GREEN}[OK]${NC} $cmd is installed"
        return 0
    else
        echo -e "${RED}[MISSING]${NC} $cmd is not installed (package: $package)"
        return 1
    fi
}

check_library() {
    local lib=$1
    local header=$2
    
    if [ -f "/usr/include/$header" ] || [ -f "/usr/local/include/$header" ]; then
        echo -e "${GREEN}[OK]${NC} Library $lib headers found"
        return 0
    else
        echo -e "${YELLOW}[WARNING]${NC} Library $lib headers not found"
        return 1
    fi
}

check_build_tools() {
    echo ""
    echo "--- Build Tools ---"
    
    check_command cmake "cmake"
    check_command make "make"
    check_command gcc "gcc"
    check_command g++ "g++"
}

check_test_frameworks() {
    echo ""
    echo "--- Test Frameworks ---"
    
    check_library gtest "gtest/gtest.h"
    check_library mockcpp "mockcpp/mockcpp.hpp"
    
    if [ -f "/usr/lib/libgtest.a" ] || [ -f "/usr/local/lib/libgtest.a" ]; then
        echo -e "${GREEN}[OK]${NC} GTest library found"
    else
        echo -e "${YELLOW}[WARNING]${NC} GTest library not found"
    fi
}

check_coverage_tools() {
    echo ""
    echo "--- Coverage Tools ---"
    
    check_command lcov "lcov"
    check_command genhtml "genhtml"
}

check_python_deps() {
    echo ""
    echo "--- Python Dependencies ---"
    
    check_command python3 "python3"
    check_command pip3 "python3-pip"
    
    if python3 -c "import pytest" 2>/dev/null; then
        echo -e "${GREEN}[OK]${NC} pytest is installed"
    else
        echo -e "${YELLOW}[WARNING]${NC} pytest is not installed"
    fi
    
    if python3 -c "import yaml" 2>/dev/null; then
        echo -e "${GREEN}[OK]${NC} PyYAML is installed"
    else
        echo -e "${YELLOW}[WARNING]${NC} PyYAML is not installed"
    fi
}

check_optional_deps() {
    echo ""
    echo "--- Optional Dependencies ---"
    
    check_library openssl "openssl/ssl.h"
    check_library glib "glib-2.0/glib.h"
    
    if command -v torchrun &>/dev/null; then
        echo -e "${GREEN}[OK]${NC} torchrun is available (for CAM tests)"
    else
        echo -e "${YELLOW}[INFO]${NC} torchrun not available (CAM tests will be limited)"
    fi
}

check_project_structure() {
    echo ""
    echo "--- Project Structure ---"
    
    if [ -d "$PROJECT_ROOT/src" ]; then
        echo -e "${GREEN}[OK]${NC} src directory exists"
    else
        echo -e "${RED}[ERROR]${NC} src directory missing"
    fi
    
    if [ -d "$PROJECT_ROOT/test" ]; then
        echo -e "${GREEN}[OK]${NC} test directory exists"
    else
        echo -e "${RED}[ERROR]${NC} test directory missing"
    fi
    
    if [ -f "$PROJECT_ROOT/src/CMakeLists.txt" ]; then
        echo -e "${GREEN}[OK]${NC} Main CMakeLists.txt exists"
    else
        echo -e "${RED}[ERROR]${NC} Main CMakeLists.txt missing"
    fi
}

check_test_env_config() {
    echo ""
    echo "--- Integration Test Environment ---"
    
    local env_file="/etc/ubus_ci/test_env.yaml"
    
    if [ -f "$env_file" ]; then
        echo -e "${GREEN}[OK]${NC} Test environment config found: $env_file"
        
        if python3 -c "import yaml; yaml.safe_load(open('$env_file'))" 2>/dev/null; then
            echo -e "${GREEN}[OK]${NC} YAML format is valid"
        else
            echo -e "${RED}[ERROR]${NC} YAML format is invalid"
        fi
    else
        echo -e "${YELLOW}[INFO]${NC} Integration test config not found at $env_file"
        echo "         Integration tests require this configuration file"
    fi
}

generate_install_script() {
    echo ""
    echo "--- Missing Dependencies ---"
    echo "To install missing dependencies, run:"
    echo ""
    echo "  # Ubuntu/Debian:"
    echo "  sudo apt-get install -y cmake build-essential g++ libgtest-dev libssl-dev lcov python3 python3-pip libglib2.0-dev"
    echo "  pip install pytest pytest-timeout pytest-html PyYAML fabric paramiko func_timeout"
    echo ""
    echo "  # CentOS/RHEL/openEuler:"
    echo "  yum install -y cmake gcc gcc-c++ openssl-devel glib2-devel python3-devel python3-pip lcov"
    echo "  pip install pytest pytest-timeout pytest-html PyYAML fabric paramiko func_timeout"
}

print_summary() {
    echo ""
    echo "========================================"
    echo "Environment Check Complete"
    echo "========================================"
}

main() {
    print_header
    
    check_build_tools
    check_test_frameworks
    check_coverage_tools
    check_python_deps
    check_optional_deps
    check_project_structure
    check_test_env_config
    
    generate_install_script
    print_summary
}

main "$@"