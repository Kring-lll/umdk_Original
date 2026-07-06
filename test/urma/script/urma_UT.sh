#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
#
set -e

SCRIPT_PATH=$(cd "$(dirname "$0")"; pwd)
source "$SCRIPT_PATH/urma_ut_common.sh"

parse_urma_ut_args "$@"
validate_urma_ut_phase "$URMA_UT_PHASE"

if [ "$URMA_UT_PHASE" = "all" ]; then
    run_urma_ut_all
else
    run_urma_ut_phase "$URMA_UT_PHASE"
fi
