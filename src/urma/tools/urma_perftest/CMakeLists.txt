# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2021-2025. All rights reserved.

add_executable(urma_perftest
    urma_perftest.c
    perftest_parameters.c
    perftest_communication.c
    perftest_resources.c
    perftest_run_test.c
)

set_target_properties(urma_perftest
    PROPERTIES
        C_STANDARD 99
)

target_link_libraries(urma_perftest
    PRIVATE
        urma pthread m
)

target_include_directories(urma_perftest
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src/urma/lib/urma/bond/include
)

install(TARGETS urma_perftest
    DESTINATION /usr/bin
)
