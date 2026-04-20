# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2021-2025. All rights reserved.

add_executable(urma_sample
    urma_sample.c
)

add_executable(uoip_example
    uoip_example.c
)

set_target_properties(urma_sample
    PROPERTIES
        C_STANDARD 99
)

set_target_properties(uoip_example
    PROPERTIES
        C_STANDARD 99
)

target_link_libraries(urma_sample urma pthread)
target_link_libraries(uoip_example urma pthread)

install(TARGETS urma_sample
    DESTINATION /usr/bin
)

install(TARGETS uoip_example
    DESTINATION /usr/bin
)
install(FILES
    ${CMAKE_SOURCE_DIR}/urma/examples/README.md
    DESTINATION /usr/share/doc/umdk-examples/urma_example
)