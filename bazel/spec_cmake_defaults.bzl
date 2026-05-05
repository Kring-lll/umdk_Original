# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
"""CMake cache aligned with `umdk/umdk.spec` (%%build).

Use `cache_entries_for_spec(...)` for selectable builds; `cache_entries()` is the full default
(same as RPM with no extra %%with flags and full %%build_all).
"""

def _enable(on):
    return "enable" if on else "disable"

def cache_entries():
    """Full-tree build matching default `umdk.spec` (no selective %%with urma/urpc/ums/dlock)."""
    return cache_entries_for_spec()

def cache_entries_for_spec(
        with_asan = False,
        with_tsan = False,
        with_gcov = False,
        with_release_enable = False,
        with_test = False,
        with_dfx_tool = False,
        with_urma = False,
        with_urpc = False,
        with_ums = False,
        with_dlock = False,
        without_udma = False,
        enable_udma_st64b = True,
        kernel_release = None,
        kernel_path = None):
    """Compute CMake `-D` flags like `umdk.spec` %%build.

    Mirrors spec macros:
      - Selective builds: any of `with_urma`, `with_urpc`, `with_ums`, `with_dlock` forces
        %%build_all-style behavior off (`-DBUILD_ALL=disable`), same as spec lines 28–31.
      - `BUILD_URMA` / `BUILD_URPC` follow %%if %%{build_all} || %%{with urma} etc.
      - `BUILD_UMS` / `BUILD_DLOCK` only when the corresponding %%with is set (full install does not pass them).
      - Sanitizers / coverage / test / release map to ASAN, TSAN, CODE_COVERAGE, URMA_TEST, RELEASE_ENABLE.
      - `without_udma` → `-DBUILD_UDMA=disable`; `enable_udma_st64b` → `-DUDMA_ST64B=enable` when True.

    `kernel_release` / `kernel_path` should match `-DKERNEL_RELEASE` / `-DKERNEL_PATH` when set (optional %%defined kernel_version in spec).
    Pass strings or omit (None) to let CMake discover headers via FindKernelHeaders.cmake.
    """
    selective = with_urma or with_urpc or with_ums or with_dlock
    build_all_on = not selective

    build_urma = build_all_on or with_urma or with_urpc
    build_urpc = build_all_on or with_urpc

    out = {
        "CMAKE_INSTALL_PREFIX": "/usr",
        "BUILD_ALL": _enable(build_all_on),
        "BUILD_URMA": _enable(build_urma),
        "BUILD_URPC": _enable(build_urpc),
    }

    if with_ums:
        out["BUILD_UMS"] = "enable"

    if with_dlock:
        out["BUILD_DLOCK"] = "enable"

    if with_asan:
        out["ASAN"] = "enable"

    if with_tsan:
        out["TSAN"] = "enable"

    if with_gcov:
        out["CODE_COVERAGE"] = "enable"

    if with_test:
        out["URMA_TEST"] = "enable"
        out["RELEASE_ENABLE"] = "disable"
    elif with_release_enable:
        out["RELEASE_ENABLE"] = "enable"

    if with_dfx_tool:
        out["DFX_TOOL"] = "enable"

    if enable_udma_st64b:
        out["UDMA_ST64B"] = "enable"
    else:
        out["UDMA_ST64B"] = "disable"

    if without_udma:
        out["BUILD_UDMA"] = "disable"

    if kernel_release:
        out["KERNEL_RELEASE"] = kernel_release

    if kernel_path:
        out["KERNEL_PATH"] = kernel_path

    return out
