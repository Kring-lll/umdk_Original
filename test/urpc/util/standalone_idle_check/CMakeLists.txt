# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
#
# Standalone CMake for tool_credit_return_idle_check
# Verifies idle check triggers credit return with precise timing
#
# Build:
#   cd standalone_idle_check
#   mkdir build && cd build
#   cmake ..
#   make

cmake_minimum_required(VERSION 3.13)
project(tool_credit_return_idle_check C)

set(UMDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." CACHE PATH "UMDK root directory")

add_executable(${PROJECT_NAME}
    ${CMAKE_CURRENT_SOURCE_DIR}/../tool_credit_return_idle_check.c
    ${UMDK_ROOT}/src/urpc/util/urpc_util.c
    ${UMDK_ROOT}/src/urma/common/ub_get_clock.c
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${UMDK_ROOT}/src/urpc/util
    ${UMDK_ROOT}/src/urma/common/include
    ${UMDK_ROOT}/src/urpc/include
    ${UMDK_ROOT}/src/urpc/include/umq
    ${UMDK_ROOT}/src/urpc/include/umq/transport_layer
    ${UMDK_ROOT}/src/urpc/include/framework
)

target_compile_definitions(${PROJECT_NAME} PRIVATE _GNU_SOURCE)

find_package(OpenSSL REQUIRED)

target_link_libraries(${PROJECT_NAME}
    OpenSSL::Crypto
    pthread
)
