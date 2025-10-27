# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2021-2025. All rights reserved.

if ((NOT BUILD_FOR_ANDROID) AND (NOT OHOS) AND (NOT BUILD_FOR_OH))
    add_subdirectory(urma_admin)
    add_subdirectory(urma_perftest)
    add_subdirectory(ubagg_cli)
endif ()
