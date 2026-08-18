# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#
# URMA 设备仿真模块构建。产出 liburma_sim.so，配合 LD_PRELOAD 在无硬件环境下
# 让 urma_admin 枚举/查询虚拟设备。

cmake_minimum_required(VERSION 3.13)
project(urma_sim C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT DEFINED URMA_SRC_DIR)
    set(URMA_SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/../../src/urma)
endif()
if(NOT DEFINED URMA_BUILD_DIR)
    set(URMA_BUILD_DIR ${CMAKE_CURRENT_LIST_DIR}/../../src/build_noasan)
endif()
set(URMA_CORE_INCLUDE_DIR ${URMA_SRC_DIR}/lib/urma/core/include)
set(URMA_CORE_DIR ${URMA_SRC_DIR}/lib/urma/core)
set(URMA_COMMON_INCLUDE_DIR ${URMA_SRC_DIR}/common/include)
set(URMA_UDMA_DIR ${URMA_SRC_DIR}/hw/udma)
set(URMA_UDMA_KERNEL_HEADERS ${URMA_UDMA_DIR}/kernel_headers)

set(URMA_LIB_DIR ${URMA_BUILD_DIR}/urma/lib/urma/core)
set(URMA_COMMON_LIB_DIR ${URMA_BUILD_DIR}/urma/common)

add_library(urma_sim SHARED
    urma_sim_intercept.c
    urma_sim_cmd.c
    urma_sim_config.c
    urma_sim_res.c
    urma_sim_exec.c
    urma_sim_ummu.c
    urma_sim_ipc.c
)
target_include_directories(urma_sim PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${URMA_CORE_INCLUDE_DIR}
    ${URMA_CORE_DIR}
    ${URMA_COMMON_INCLUDE_DIR}
    ${URMA_UDMA_DIR}
    ${URMA_UDMA_KERNEL_HEADERS}
)
target_compile_options(urma_sim PRIVATE -Wall -Werror -g -O2 -fPIC -D_GNU_SOURCE)
target_link_options(urma_sim PRIVATE
    -Wl,--version-script,${CMAKE_CURRENT_LIST_DIR}/urma_sim.version)
target_link_libraries(urma_sim PRIVATE dl pthread atomic
    ${URMA_LIB_DIR}/liburma.so
    ${URMA_COMMON_LIB_DIR}/liburma_common.so)
target_link_directories(urma_sim PRIVATE ${URMA_LIB_DIR} ${URMA_COMMON_LIB_DIR})

set_target_properties(urma_sim PROPERTIES
    OUTPUT_NAME urma_sim
    PREFIX lib
)
