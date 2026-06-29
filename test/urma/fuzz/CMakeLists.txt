# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.

add_executable(urma_api_fuzz
    urma_api_fuzz.c
)
target_link_libraries(urma_api_fuzz PRIVATE urma_test_common ${URMA_SO} ${URMA_COMMON_SO}
    nl-genl-3 nl-3 dl rt pthread)

add_executable(uvs_api_fuzz
    uvs_api_fuzz.c
)
target_link_libraries(uvs_api_fuzz PRIVATE urma_test_common ${URMA_TPSA_SO} ${URMA_COMMON_SO}
    nl-genl-3 nl-3 dl rt pthread)

add_custom_target(urma_fuzz
    DEPENDS urma_api_fuzz uvs_api_fuzz
)

add_test(NAME urma_api_fuzz COMMAND urma_api_fuzz)
set_tests_properties(urma_api_fuzz PROPERTIES LABELS "phase_fuzz;phase_fuzz_urma")

add_test(NAME uvs_api_fuzz COMMAND uvs_api_fuzz)
set_tests_properties(uvs_api_fuzz PROPERTIES LABELS "phase_fuzz;phase_fuzz_uvs")
