#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#
# 示例：用 urma_sim 仿真 preload 运行 urma_admin 查询虚拟设备。
# 前提：已用 src/build_noasan 编出非 asan 的 urma_admin（见 README "构建"一节）。

set -e
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SIM_DIR="$SCRIPT_DIR/.."
URMA_SRC_BUILD="${URMA_SRC_BUILD:-/umdk/src/build_noasan}"
URMA_ADMIN="$URMA_SRC_BUILD/urma/tools/urma_admin/urma_admin"

export LD_LIBRARY_PATH="$URMA_SRC_BUILD/urma/lib/urma/core:$URMA_SRC_BUILD/urma/common:${LD_LIBRARY_PATH:-}"
export LD_PRELOAD="$SIM_DIR/build/liburma_sim.so"
export URMA_SIM_CONFIG="${URMA_SIM_CONFIG:-$SIM_DIR/urma_sim_config.json}"

echo "=== 设备名校验(show -d) ==="
"$URMA_ADMIN" show -d udma_sim0

echo
echo "=== 枚举(show) ==="
"$URMA_ADMIN" show
