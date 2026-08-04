#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
#
# DT 构建来源指纹公共函数。
#
# 目的：保证 DT 门禁用到的 liburma.so / liburma-udma.so / liburma_sim.so
# 一定由当前 umdk 仓库源码编译，而不是复用过期的构建产物。
# 思路：对参与构建的源码树计算内容指纹（sha256），存到构建目录；
# 源码发生变化（或指纹缺失）时强制重新配置并编译，编译失败即报错。

# 计算源码指纹。
# 用法：dt_source_fingerprint <path>...
# 支持传目录或单个文件；目录内排除 build*/CMakeFiles 及编译产物。
dt_source_fingerprint()
{
    local tmp
    local p
    tmp=$(mktemp "${TMPDIR:-/tmp}/dt_fp.XXXXXX") || return 1
    for p in "$@"; do
        if [ -f "$p" ]; then
            sha256sum "$p" >>"$tmp" 2>/dev/null
        elif [ -d "$p" ]; then
            find "$p" -type f \
                ! -path '*/build*/*' \
                ! -path '*/CMakeFiles/*' \
                ! -name '*.o' \
                ! -name '*.a' \
                ! -name '*.so' \
                ! -name '*.so.*' \
                -print0 2>/dev/null |
                sort -z |
                xargs -0 -r sha256sum >>"$tmp" 2>/dev/null
        fi
    done
    sort "$tmp" 2>/dev/null | sha256sum | awk '{print $1}'
    rm -f "$tmp"
}

# 保存指纹到构建目录。
dt_fingerprint_save()
{
    local file=$1
    local hash=$2
    mkdir -p "$(dirname "$file")"
    printf '%s\n' "$hash" >"$file"
}

# 指纹文件存在且与当前一致返回 0，否则返回 1。
dt_fingerprint_matches()
{
    local file=$1
    local hash=$2
    local stored=""
    if [ ! -f "$file" ]; then
        return 1
    fi
    read -r stored <"$file" || return 1
    [ "$stored" = "$hash" ]
}
