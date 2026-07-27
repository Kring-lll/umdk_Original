#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#
# URMA 设备仿真 - 构建 + 运行环境准备脚本。
#
# 用法：
#   bash urma_sim_setup.sh           # 构建 liburma_sim.so
#   bash urma_sim_setup.sh --run CMD # 构建 + 用 preload 运行 CMD
#
# 示例：
#   bash urma_sim_setup.sh --run urma_admin show -d udma_sim0
#
# 环境变量：
#   URMA_SIM_CONFIG  指向自定义 config.json（默认用内置或同目录 urma_sim_config.json）

set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
BUILD_DIR="$SCRIPT_DIR/build"
SIM_SO="$BUILD_DIR/liburma_sim.so"
DEFAULT_CONFIG="$SCRIPT_DIR/urma_sim_config.json"

# urma_admin 非asan构建产物路径（src/build_noasan）。若不存在提示先构建。
URMA_SRC_BUILD="${URMA_SRC_BUILD:-/umdk/src/build_noasan}"
URMA_ADMIN="$URMA_SRC_BUILD/urma/tools/urma_admin/urma_admin"

function build_sim() {
    echo "[1/2] 构建 liburma_sim.so"
    mkdir -p "$BUILD_DIR"
    (cd "$BUILD_DIR" && cmake .. >/dev/null 2>&1 && cmake --build . -j >/dev/null 2>&1)
    echo "    -> $SIM_SO"
}

function print_env() {
    echo "[2/2] 运行环境"
    echo "    export LD_LIBRARY_PATH=$URMA_SRC_BUILD/urma/lib/urma/core:$URMA_SRC_BUILD/urma/common:\${LD_LIBRARY_PATH:-}"
    echo "    export LD_PRELOAD=$SIM_SO"
    echo "    export URMA_SIM_CONFIG=${URMA_SIM_CONFIG:-$DEFAULT_CONFIG}"
}

# 构造 preload 环境变量串，供 source 或直接用
function export_sim_env() {
    export LD_LIBRARY_PATH="$URMA_SRC_BUILD/urma/lib/urma/core:$URMA_SRC_BUILD/urma/common:${LD_LIBRARY_PATH:-}"
    export LD_PRELOAD="$SIM_SO"
    export URMA_SIM_CONFIG="${URMA_SIM_CONFIG:-$DEFAULT_CONFIG}"
}

if [ ! -f "$SIM_SO" ]; then
    build_sim
fi

if [ "${1:-}" = "--run" ]; then
    shift
    export_sim_env
    echo ">>> 运行: $*"
    "$@"
else
    print_env
    echo
    echo "提示：执行 'source $(basename "$0") --env' 或手动 export 上述变量后，"
    echo "      直接运行 urma_admin / 自定义程序即可看到虚拟设备。"
    echo "      例: $URMA_ADMIN show -d udma_sim0"
fi
