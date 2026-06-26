# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.

cmake_minimum_required(VERSION 3.13)

project(test_util)

add_compile_definitions(URPC_ASAN)

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/ TEST_SOURCE_DIR)

add_executable(${PROJECT_NAME} ${TEST_SOURCE_DIR} ${TEST_C_FILE})

target_include_directories(${PROJECT_NAME} PRIVATE
    /usr/include/ub/umdk/urma
    ${URPC_ROOT_DIR}/include/framework
    ${URPC_ROOT_DIR}/util/
    ${URPC_ROOT_DIR}/framework
)

if(ENABLE_ASAN)
    target_compile_options(${PROJECT_NAME} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(${PROJECT_NAME} PRIVATE -fsanitize=address)
endif()

target_link_libraries(${PROJECT_NAME}
    urpc_framework
    GTest::gtest
    GTest::gtest_main
    mockcpp
    urma
    OpenSSL::SSL
    pthread
    ${CMAKE_THREAD_LIBS_INIT}
)

add_executable(tool_cpu_cycle_cross_thread tool_cpu_cycle_cross_thread.c)

target_include_directories(tool_cpu_cycle_cross_thread PRIVATE
    ${URPC_ROOT_DIR}/util/
)

target_link_libraries(tool_cpu_cycle_cross_thread
    urpc_framework
    pthread
    ${CMAKE_THREAD_LIBS_INIT}
)

add_executable(tool_credit_return_idle_check tool_credit_return_idle_check.c)

target_include_directories(tool_credit_return_idle_check PRIVATE
    ${URPC_ROOT_DIR}/util/
    ${URPC_ROOT_DIR}/include/framework
    /usr/include/ub/umdk/urma
    /usr/include/ub/umdk/umq
)

target_link_libraries(tool_credit_return_idle_check
    umq
    umq_buf
    urma
    urma_common
    urpc_framework
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
    m
    ${CMAKE_THREAD_LIBS_INIT}
)
