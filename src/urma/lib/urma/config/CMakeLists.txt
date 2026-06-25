# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2021-2025. All rights reserved.

install(FILES "${CMAKE_CURRENT_LIST_DIR}/urma.conf"
    DESTINATION /etc/rsyslog.d/
    PERMISSIONS OWNER_WRITE OWNER_READ)
install(FILES "${CMAKE_CURRENT_LIST_DIR}/urma"
    DESTINATION /etc/logrotate.d/
    PERMISSIONS OWNER_WRITE OWNER_READ)
