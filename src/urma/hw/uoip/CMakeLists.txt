# SPDX-License-Identifier: MIT
# Copyright (c) 2025. All rights reserved.
# Description: CMake build rules for liburma-uoip.so

# Collect all .c files in this directory
aux_source_directory(${CMAKE_CURRENT_LIST_DIR} PROVIDER_UOIP_SRCS)

add_library(BETA_UOIP_SO SHARED ${PROVIDER_UOIP_SRCS})

# Headers from liburma core
target_include_directories(BETA_UOIP_SO PRIVATE
    ${CMAKE_SOURCE_DIR}/urma/lib/urma/core
    ${CMAKE_SOURCE_DIR}/urma/lib/urma/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Link against liburma (for urma_register_provider_ops etc.)
target_link_libraries(BETA_UOIP_SO PRIVATE urma)
target_link_libraries(BETA_UOIP_SO PRIVATE urma_common)

# Output library name follows the liburma-<driver> convention so that
# liburma's driver loader (scans for liburma-*.so) picks it up.
set(UOIP_NAME urma-uoip)
set_target_properties(BETA_UOIP_SO PROPERTIES OUTPUT_NAME ${UOIP_NAME})

# Compiler flags mirrored from the udma driver
set(CMAKE_C_FLAGS
    "-Wall -Wextra -O2 -Wfloat-equal -fno-common -std=gnu99 \
     -Werror=implicit-function-declaration \
     -Wl,-z,now -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
     -Wuninitialized -Wno-error -Wundef -Wunused -Wvla \
     -Wempty-body -Wimplicit-fallthrough=3 -Wtype-limits \
     -Wshift-overflow=2 -Wmissing-prototypes -Wstrict-prototypes")

include(GNUInstallDirs)
install(TARGETS BETA_UOIP_SO
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/urma OPTIONAL)
