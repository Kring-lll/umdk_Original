# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.

add_library(urma_tools_common STATIC
    urma_tools_log.c
)

set_target_properties(urma_tools_common
    PROPERTIES
        C_STANDARD 99
)

target_include_directories(urma_tools_common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
