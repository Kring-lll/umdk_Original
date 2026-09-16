# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2021-2025. All rights reserved.

# Library/note version; keep aligned with the public macros in urma_types.h.
set(PROJECT_VERSION "0.1.0")
string(REPLACE "." ";" URMA_VERSION_PARTS "${PROJECT_VERSION}")
list(GET URMA_VERSION_PARTS 0 PROJECT_VERSION_MAJOR)
list(GET URMA_VERSION_PARTS 1 PROJECT_VERSION_MINOR)
list(GET URMA_VERSION_PARTS 2 PROJECT_VERSION_PATCH)
math(EXPR URMA_VERSION_CODE
    "(${PROJECT_VERSION_MAJOR} << 32) | (${PROJECT_VERSION_MINOR} << 16) | ${PROJECT_VERSION_PATCH}")
set(URMA_VERSION_NOTE "${CMAKE_CURRENT_BINARY_DIR}/urma_version_note.S")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/urma_version_note.S.in"
    "${URMA_VERSION_NOTE}" @ONLY)

enable_language(ASM)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(URMA_PERF_USE_CNTVCT_DEFAULT ON)
else()
    set(URMA_PERF_USE_CNTVCT_DEFAULT OFF)
endif()
option(URMA_PERF_USE_CNTVCT "Use ARM CNTVCT_EL0 register for perf timestamp instead of clock_gettime (AArch64 only)" ${URMA_PERF_USE_CNTVCT_DEFAULT})

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} URMA_DIR_SRCS)

# Historical adapters are shared-only; their headers are not installed.
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/compat URMA_COMPAT_SRCS)

add_library(urma SHARED
    ${URMA_COMPAT_SRCS}
    ${URMA_VERSION_NOTE}
    ${URMA_DIR_SRCS}
)

add_library(urma_static STATIC
    ${URMA_DIR_SRCS}
)

target_compile_definitions(urma PRIVATE URMA_BUILD_SHARED=1)

set_target_properties(urma
    PROPERTIES
        C_STANDARD 99
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
)
set_target_properties(urma_static
    PROPERTIES
        C_STANDARD 99
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
)

target_include_directories(urma
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(urma_static
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Stable ABI symbol node; do not derive its name from the package version.
set(URMA_SYMBOL_VERSION_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/liburma.map")
set_property(TARGET urma APPEND_STRING PROPERTY LINK_FLAGS
    " -Wl,--version-script,${URMA_SYMBOL_VERSION_SCRIPT} -Wl,-z,now")
set_property(TARGET urma APPEND PROPERTY LINK_DEPENDS "${URMA_SYMBOL_VERSION_SCRIPT}")
# Consumers of this target resolve PLT relocations at startup.
# LINK_FLAGS above retains compatibility with CMake 3.12.
set_property(TARGET urma APPEND PROPERTY INTERFACE_LINK_LIBRARIES "-Wl,-z,now")

target_link_libraries(urma urma_common -ldl -lrt)
target_link_libraries(urma_static urma_common -ldl -lrt)

if(URMA_PERF_USE_CNTVCT)
    target_compile_definitions(urma PRIVATE URMA_PERF_USE_CNTVCT)
    target_compile_definitions(urma_static PRIVATE URMA_PERF_USE_CNTVCT)
endif()

install(TARGETS urma
    DESTINATION /usr/lib64
)
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/urma_api.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/urma_cmd.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/urma_opcode.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/urma_provider.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/urma_types_str.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/urma_types.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/urma_perf.h
    DESTINATION /usr/include/ub/umdk/urma
)
