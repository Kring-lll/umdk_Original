# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2021-2025. All rights reserved.

if("${URMA_INSTALL}" STREQUAL "enable")
    install(FILES "${CMAKE_SOURCE_DIR}/urma/tools/urma_admin/config/urma_admin.conf"
        DESTINATION /etc/rsyslog.d/
        PERMISSIONS OWNER_WRITE OWNER_READ)
endif()