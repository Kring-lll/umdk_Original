#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Generate the same version artifacts as CMake, without requiring CMake.
set -euo pipefail
cmake_file=$1
header_template=$2
note_template=$3
output_dir=$4
version=$(sed -n 's/^set(PROJECT_VERSION "\([0-9.]*\)")$/\1/p' "$cmake_file")
if [[ ! "$version" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]; then
    echo "Invalid URMA version in $cmake_file" >&2
    exit 1
fi
major=${BASH_REMATCH[1]}
minor=${BASH_REMATCH[2]}
patch=${BASH_REMATCH[3]}
if (( major > 4294967295 || minor > 65535 || patch > 65535 )); then
    echo "URMA version exceeds the 32/16/16-bit encoding" >&2
    exit 1
fi
code=$(( (major << 32) | (minor << 16) | patch ))
mkdir -p "$output_dir"
sed -e "s/@PROJECT_VERSION_MAJOR@/$major/g" \
    -e "s/@PROJECT_VERSION_MINOR@/$minor/g" \
    -e "s/@PROJECT_VERSION_PATCH@/$patch/g" \
    -e "s/@PROJECT_VERSION@/$version/g" \
    "$header_template" > "$output_dir/urma_version.h"
sed "s/@URMA_VERSION_CODE@/$code/g" "$note_template" > "$output_dir/urma_version_note.S"
for lib in urma urma_common urma_ubagg tpsa; do
    printf -- '-soname lib%s.so.%s\n' "$lib" "$major" > "$output_dir/$lib.ldflags"
done
