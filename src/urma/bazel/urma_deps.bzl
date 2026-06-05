"""Versioned external dependency configuration for URMA Bazel builds."""

LIBUMMU_REMOTE = "https://gitcode.com/openeuler/libummu.git"
LIBUMMU_COMMIT = "d28a6b1096f1214f9d3185b41537cde06d5e6233"
LIBUMMU_VERSION = "1.0.3"
LIBUMMU_ABI_VERSION = "1"

# Keep these explicit to match libummu.spec/CMake NEEDED entries. libummu.spec
# is aarch64-only today; if that changes, update this dependency config in one place.
LIBUMMU_RUNTIME_LINKOPTS = [
    "-Wl,--no-as-needed",
    "-lstdc++",
    "-lm",
    "-lgcc_s",
    "-lc",
    "-l:ld-linux-aarch64.so.1",
    "-Wl,--as-needed",
]
