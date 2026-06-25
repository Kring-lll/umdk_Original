# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2021-2025. All rights reserved.


set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(CROSS_COMPILE)
    set(CMAKE_C_COMPILER "${CROSS_COMPILE}")
    message(STATUS "CMAKE_C_COMPILER: ${CROSS_COMPILE}")
else()
    set(CMAKE_C_COMPILER "/usr/bin/gcc")
endif()

if(NOT "${BUILD_DEBUG}" STREQUAL "off")
    add_definitions(-DUNIT_TEST)
endif()

# Position independent code - enables -fPIE, -fPIC, and -pie automatically
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Common compile options
set(COMPILE_OPTIONS_PUBLIC
    -Wall
    -Werror
    -Wformat
    -Wfloat-equal
    -Wtrampolines
    -g
    -O2
    -fno-strict-aliasing
    -fstack-protector-strong
    -D_FORTIFY_SOURCE=2
)

# Link options
set(LINK_OPTIONS_PUBLIC
    -rdynamic
    -Wl,-z,noexecstack,-z,relro,-z,now
)

if(CROSS_COMPILE_LINK)
    list(APPEND LINK_OPTIONS_PUBLIC ${CROSS_COMPILE_LINK})
    message(STATUS "CROSS_COMPILE_LINK: ${CROSS_COMPILE_LINK}")
endif()

# GTEST ENABLE
if("${GTEST}" STREQUAL "enable")
# mockcpp can only run on -O0 mode, because it need to change assembly code,
# so we cannot do compiler optimization.
    set(COMPILE_OPTIONS_PUBLIC
        -g
        -Wall
        -Werror
        -O0
        -DUT
        -fPIC
    )
endif()

# PERF_CYCLE ENABLE
if("${PERF_CYCLE}" STREQUAL "enable")
    list(APPEND COMPILE_OPTIONS_PUBLIC -DPERF_CYCLE_FLAG)
endif()

# ASAN ENABLE
if("${ASAN}" STREQUAL "enable")
    list(APPEND COMPILE_OPTIONS_PUBLIC
        -fsanitize=address
        -fsanitize-recover=address
        -fsanitize=leak
        -fno-omit-frame-pointer
    )
endif()

# CODE_COVERAGE ENABLE
if("${CODE_COVERAGE}" STREQUAL "enable")
    list(APPEND COMPILE_OPTIONS_PUBLIC
        --coverage
        -fprofile-arcs
        -ftest-coverage
    )
    list(APPEND LINK_OPTIONS_PUBLIC --coverage)
endif()

# Architecture-specific compile options
set(COMPILE_OPTIONS_ARM64
    -march=armv8-a+crc
    -DUB_ARCH_ARM64
)
set(COMPILE_OPTIONS_x86_64
    -msse4.2
    -DUB_ARCH_X86_64
)
if("${X86_CROSS_COMPILATION}" STREQUAL "enable")
    set(COMPILE_OPTIONS_x86_64 -DUB_ARCH_X86_64)
    message(STATUS "x86 cross compilation, disabling msse4.2!")
endif()

# Apply compile and link options based on architecture
if("${CMAKE_HOST_SYSTEM_PROCESSOR}" STREQUAL "aarch64")
    add_compile_options(${COMPILE_OPTIONS_PUBLIC} ${COMPILE_OPTIONS_ARM64})
else()
    add_compile_options(${COMPILE_OPTIONS_PUBLIC} ${COMPILE_OPTIONS_x86_64})
endif()

add_link_options(${LINK_OPTIONS_PUBLIC})

# C++ standard
set(CMAKE_CXX_STANDARD 11)

set(CMAKE_SKIP_RPATH TRUE)

message(STATUS "Compile options: ${COMPILE_OPTIONS_PUBLIC}")
enable_testing()

add_subdirectory(common)
add_subdirectory(lib)
add_subdirectory(examples)
add_subdirectory(tools)
if(NOT "${BUILD_UDMA}" STREQUAL "disable")
    add_subdirectory(hw)
endif()

# uninstall target
if(NOT TARGET uninstall)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
        IMMEDIATE @ONLY)

    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)
endif()
