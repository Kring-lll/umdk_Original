# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.

add_executable(urma_mesh_perftest
    urma_mesh_perftest.c
)

set_target_properties(urma_mesh_perftest
    PROPERTIES
        C_STANDARD 99
)

target_include_directories(urma_mesh_perftest
    PRIVATE
        ${CMAKE_SOURCE_DIR}/urma/lib/urma/core/include
        ${CMAKE_SOURCE_DIR}/urma/lib/urma/bond/include
        ${CMAKE_SOURCE_DIR}/urma/lib/uvs/core/include
        ${CMAKE_SOURCE_DIR}/urma/common/include
)

target_link_libraries(urma_mesh_perftest
    PRIVATE
        urma tpsa pthread m
)

install(TARGETS urma_mesh_perftest
    DESTINATION /usr/bin
)
