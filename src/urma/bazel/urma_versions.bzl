"""Single source of truth for Bazel-built URMA artifact versions."""

BAZEL_REQUIRED_VERSION = "7.4.1"
URMA_VERSION = "0.0.3"
URMA_SOVERSION = "0"
URMA_PACKAGE_VERSION = URMA_VERSION

def _artifact_version_info(component, target, artifact_type, file_name, install_dir, soname = "-"):
    return struct(
        component = component,
        target = target,
        artifact_type = artifact_type,
        version = URMA_VERSION,
        soversion = URMA_SOVERSION,
        soname = soname,
        file_name = file_name,
        link_name = file_name,
        install_dir = install_dir,
        kind = artifact_type,
        origin = "urma",
        versioned_links = False,
    )

def _shared_object_version_info(component, target, link_name, install_dir, kind, versioned_links):
    soname = link_name + "." + URMA_SOVERSION if versioned_links else link_name
    file_name = link_name + "." + URMA_VERSION if versioned_links else link_name
    return struct(
        component = component,
        target = target,
        artifact_type = "so",
        version = URMA_VERSION,
        soversion = URMA_SOVERSION,
        soname = soname,
        file_name = file_name,
        link_name = link_name,
        install_dir = install_dir,
        kind = kind,
        origin = "urma",
        versioned_links = versioned_links,
    )

URMA_SO_VERSIONS = {
    "urma_common": _shared_object_version_info(
        "urma_common",
        "urma_common",
        "liburma_common.so",
        "/usr/lib64",
        "library",
        True,
    ),
    "urma": _shared_object_version_info(
        "urma",
        "urma",
        "liburma.so",
        "/usr/lib64",
        "library",
        True,
    ),
    "tpsa": _shared_object_version_info(
        "tpsa",
        "tpsa",
        "libtpsa.so",
        "/usr/lib64",
        "library",
        True,
    ),
    "urma_ubagg": _shared_object_version_info(
        "urma_ubagg",
        "urma_ubagg",
        "liburma_ubagg.so",
        "/usr/lib64/urma",
        "provider",
        True,
    ),
    "urma_udma": _shared_object_version_info(
        "urma_udma",
        "urma-udma",
        "liburma-udma.so",
        "/usr/lib64/urma",
        "provider",
        False,
    ),
}

URMA_BINARY_VERSIONS = {
    "urma_admin": _artifact_version_info("urma_admin", "urma_admin", "binary", "urma_admin", "/usr/bin"),
    "urma_perftest": _artifact_version_info(
        "urma_perftest",
        "urma_perftest",
        "binary",
        "urma_perftest",
        "/usr/bin",
    ),
    "urma_ping": _artifact_version_info("urma_ping", "urma_ping", "binary", "urma_ping", "/usr/bin"),
    "urma_sample": _artifact_version_info("urma_sample", "urma_sample", "binary", "urma_sample", "/usr/bin"),
    "urma_version": _artifact_version_info("urma_version", "urma_version", "binary", "urma_version", "/usr/bin"),
}

URMA_VERSION_HEADER = _artifact_version_info(
    "urma_header",
    "urma_version_header",
    "header",
    "urma_version.h",
    "/usr/include/ub/umdk/urma",
)

def libummu_version_info(version, soversion):
    """Build libummu version metadata from the pinned external dependency."""
    return struct(
        component = "libummu",
        target = "ummu",
        artifact_type = "so",
        version = version,
        soversion = soversion,
        soname = "libummu.so." + soversion,
        file_name = "libummu.so." + version,
        link_name = "libummu.so",
        install_dir = "/usr/lib64",
        kind = "library",
        origin = "external",
        versioned_links = True,
    )
