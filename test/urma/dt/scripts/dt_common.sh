#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
#

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

dt_fingerprint_save()
{
    local file=$1
    local hash=$2
    mkdir -p "$(dirname "$file")"
    printf '%s\n' "$hash" >"$file"
}

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
