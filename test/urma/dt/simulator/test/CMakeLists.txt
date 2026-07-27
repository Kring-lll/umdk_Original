# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#
# urma_sim 单元测试。仿 umdk/test/urma/ut 的 gtest 风格（纯 C 自实现断言宏，容器无 gtest 库故不依赖 GTest）。
# 测 sim 的纯函数（urma_sim_mmap_decode_offset 等），链接 liburma_sim.so 调真导出函数。
#
# 开启：cmake .. -DBUILD_SIM_UT=ON （默认 OFF，不影响 sim 正常构建）
# 跑：ctest 或直接 ./urma_sim_mmap_decode_ut

cmake_minimum_required(VERSION 3.13)

# urma_sim_mmap_decode_offset 单元测试
add_executable(urma_sim_mmap_decode_ut
    test_mmap_decode_ut.c
)
target_include_directories(urma_sim_mmap_decode_ut PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/..           # urma_sim_hw.h
    ${URMA_CORE_INCLUDE_DIR}
    ${URMA_UDMA_KERNEL_HEADERS}
)
target_compile_options(urma_sim_mmap_decode_ut PRIVATE -Wall -Werror -g -O2 -D_GNU_SOURCE)
target_link_libraries(urma_sim_mmap_decode_ut PRIVATE urma_sim)

add_test(NAME urma_sim_mmap_decode_ut COMMAND urma_sim_mmap_decode_ut)
