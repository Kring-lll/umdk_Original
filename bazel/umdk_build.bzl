# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.

def _shell_quote(s):
    return "'" + str(s).replace("'", "'\"'\"'") + "'"

def _dirname(path):
    i = path.rfind("/")
    return path[:i] if i > 0 else "."

def _umdk_cmake_impl(ctx):
    cmake_lists = ctx.file.cmake_lists

    # Directory TreeArtifacts filled via `cp` are unreliable with `run_shell` on some setups.
    # Build-only mode emits one `.tar.gz` of the CMake build directory (unpack to inspect libs/binaries).
    if ctx.attr.run_install:
        out = ctx.actions.declare_directory(ctx.attr.install_dir_name)
        out_path = out.path
        finish = """
export DESTDIR="{out}"
cmake --install "$BUILD_DIR"
""".format(out = out_path)
    else:
        out = ctx.actions.declare_file(ctx.attr.install_dir_name + ".tar.gz")
        out_path = out.path
        finish = """
test -f "$BUILD_DIR/CMakeCache.txt" || {{ echo "umdk_cmake: missing CMake build dir" >&2; exit 1; }}
(tar -C "$BUILD_DIR" -cf - . | gzip -nc > "{out}")
test -s "{out}" || {{ echo "umdk_cmake: failed to write build archive" >&2; exit 1; }}
""".format(out = out_path)

    src_dir = _dirname(cmake_lists.path)
    repo_root = _dirname(src_dir)
    ws_tag = ctx.attr.install_dir_name.replace("/", "_")

    fragments = []
    for k in sorted(ctx.attr.cache_entries.keys()):
        v = ctx.attr.cache_entries[k]
        fragments.append("-D" + k + "=" + _shell_quote(v))
    cmake_d_flags = " ".join(fragments)

    date_cmd = "date -u +%Y-%m-%d-%H:%M:%S"
    if ctx.attr.build_date:
        date_cmd = "echo " + _shell_quote(ctx.attr.build_date)

    setup_install_root = ""
    if ctx.attr.run_install:
        setup_install_root = """
mkdir -p "{out}"
chmod u+w "{out}" 2>/dev/null || true
find "{out}" -mindepth 1 -delete 2>/dev/null || true
""".format(out = out_path)

    cmd = """
set -euo pipefail
REPO_ROOT={repo_root}
SRC_TREE="$REPO_ROOT/"{src_subdir}
BUILD_DIR="$PWD/umdk_cmake_build_{ws_tag}"
SRC_RW="$PWD/umdk_src_rw_{ws_tag}"
rm -rf "$BUILD_DIR" "$SRC_RW"
mkdir -p "$SRC_RW"
{setup_install_root}
cp -a "$SRC_TREE/." "$SRC_RW/"

BUILD_DATE=$( {date_cmd} )
if [ -n "${{BUILD_DATE_OVERRIDE:-}}" ]; then
  BUILD_DATE="$BUILD_DATE_OVERRIDE"
fi

cmake -S "$SRC_RW" -B "$BUILD_DIR" {cache_flags} \\
  -DBUILD_DATE="$BUILD_DATE"
NJ="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
cmake --build "$BUILD_DIR" -- -j"$NJ"

{finish}
""".format(
        repo_root = _shell_quote(repo_root),
        src_subdir = ctx.attr.src_subdir,
        ws_tag = ws_tag,
        setup_install_root = setup_install_root,
        cache_flags = cmake_d_flags,
        date_cmd = date_cmd,
        finish = finish,
    )

    progress = (
        "UMDK CMake configure/build/install (DESTDIR)"
        if ctx.attr.run_install
        else "UMDK CMake configure/build (tar.gz, no install)"
    )

    ctx.actions.run_shell(
        outputs = [out],
        inputs = depset(direct = [cmake_lists] + ctx.files.extra_inputs),
        command = cmd,
        progress_message = progress,
        use_default_shell_env = True,
        execution_requirements = {
            "no-sandbox": "1",
        },
    )
    return [DefaultInfo(files = depset([out]))]

umdk_cmake = rule(
    implementation = _umdk_cmake_impl,
    attrs = {
        "cmake_lists": attr.label(allow_single_file = True, mandatory = True),
        "src_subdir": attr.string(
            default = "src",
            doc = "Source subtree under the repo root (same layout as `cmake ./src/` in umdk.spec).",
        ),
        "extra_inputs": attr.label_list(
            allow_files = True,
            default = [],
            doc = "Optional extra files that should invalidate this build when changed.",
        ),
        "cache_entries": attr.string_dict(mandatory = True),
        "install_dir_name": attr.string(default = "umdk_install"),
        "build_date": attr.string(default = ""),
        "run_install": attr.bool(
            default = False,
            doc = (
                "If True, run `cmake --install` with DESTDIR under `install_dir_name/` (FHS tree). " +
                "If False (default), only configure + build; output is `<install_dir_name>.tar.gz` " +
                "(gzip tarball of the CMake build directory)."
            ),
        ),
    },
)
