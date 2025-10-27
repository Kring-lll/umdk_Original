# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2021-2025. All rights reserved.

add_executable(ubagg_cli
    ubagg_cli.c
)

set_target_properties(ubagg_cli
    PROPERTIES
        C_STANDARD 99
)

target_link_libraries(ubagg_cli
    PRIVATE
        urma urma_ubagg pthread
)

install(TARGETS ubagg_cli
    DESTINATION /usr/bin
)
