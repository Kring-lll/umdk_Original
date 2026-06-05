"""Versioned external dependency configuration for URMA Bazel builds."""

LIBUMMU_REMOTE = "https://gitcode.com/openeuler/libummu.git"
LIBUMMU_COMMIT = "d28a6b1096f1214f9d3185b41537cde06d5e6233"
LIBUMMU_VERSION = "1.0.3"
LIBUMMU_ABI_VERSION = "1"

# Keep these explicit to match libummu.spec/CMake NEEDED entries.
LIBUMMU_RUNTIME_LINKOPTS = [
    "-lstdc++",
    "-lm",
    "-lgcc_s",
    "-lc",
]

# glibc's aarch64 dynamic loader exists only on aarch64 systems. Keep this out
# of the common linkopts so x86_64 and other architectures can build //:ummu.
LIBUMMU_AARCH64_RUNTIME_LINKOPTS = [
    "-l:ld-linux-aarch64.so.1",
]
