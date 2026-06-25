# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2021-2025. All rights reserved.

add_executable(urma_perftest
    urma_perftest.c
    perftest_log.c
    perftest_parameters.c
    perftest_communication.c
    perftest_resources.c
    perftest_run_test.c
)

set_target_properties(urma_perftest
    PROPERTIES
        C_STANDARD 99
)

target_include_directories(urma_perftest
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../../lib/urma/core/include
        ${CMAKE_CURRENT_LIST_DIR}/../../lib/urma/bond/include
        ${CMAKE_CURRENT_LIST_DIR}/../../common/include
)

target_link_libraries(urma_perftest
    PRIVATE
        urma pthread m
)

install(TARGETS urma_perftest
    DESTINATION /usr/bin
)
