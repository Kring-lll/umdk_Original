"""Rules that generate and validate Bazel-built URMA artifact versions."""

load("@rules_cc//cc:defs.bzl", "cc_library")

def _metadata_symbol(version_info):
    return "g_urma_version_metadata_" + version_info.component.replace("-", "_")

def _version_source_impl(ctx):
    output = ctx.actions.declare_file(ctx.label.name + ".c")
    ctx.actions.expand_template(
        template = ctx.file.template,
        output = output,
        substitutions = {
            "@COMPONENT@": ctx.attr.component,
            "@ORIGIN@": ctx.attr.origin,
            "@VERSION@": ctx.attr.version,
            "@SOVERSION@": ctx.attr.soversion,
            "@SONAME@": ctx.attr.soname,
            "@SYMBOL@": ctx.attr.symbol,
        },
    )
    return DefaultInfo(files = depset([output]))

_version_source = rule(
    implementation = _version_source_impl,
    attrs = {
        "component": attr.string(mandatory = True),
        "origin": attr.string(mandatory = True),
        "version": attr.string(mandatory = True),
        "soversion": attr.string(mandatory = True),
        "soname": attr.string(mandatory = True),
        "symbol": attr.string(mandatory = True),
        "template": attr.label(allow_single_file = True, mandatory = True),
    },
)

def _version_library(name, version_info, template, visibility):
    source_target = name + "_source"
    _version_source(
        name = source_target,
        component = version_info.component,
        origin = version_info.origin,
        version = version_info.version,
        soversion = version_info.soversion,
        soname = version_info.soname,
        symbol = _metadata_symbol(version_info),
        template = template,
        visibility = visibility,
    )
    cc_library(
        name = name,
        srcs = [":" + source_target],
        alwayslink = True,
        linkstatic = True,
        visibility = visibility,
    )

def urma_version_note(name, version_info, visibility = None):
    """Create an always-linked object containing shared-object metadata."""
    vis = visibility if visibility != None else ["//visibility:public"]
    _version_library(name, version_info, "//bazel:urma_so_version.c.tpl", vis)

def urma_binary_version(name, version_info, visibility = None):
    """Create an executable main wrapper that prints Bazel version metadata."""
    vis = visibility if visibility != None else ["//visibility:public"]
    _version_library(name, version_info, "//bazel:urma_binary_version.c.tpl", vis)

def urma_version_linkopts(version_info):
    """Keep hidden shared-object metadata when linker section GC is enabled."""
    return ["-Wl,--undefined=" + _metadata_symbol(version_info)]

def urma_binary_version_linkopts(version_info):
    """Route main through the wrapper and retain hidden executable metadata."""
    return [
        "-Wl,--wrap=main",
        "-Wl,--undefined=" + _metadata_symbol(version_info),
    ]

def _version_header_impl(ctx):
    output = ctx.actions.declare_file("versioned/include/urma_version.h")
    ctx.actions.expand_template(
        template = ctx.file.template,
        output = output,
        substitutions = {
            "@COMPONENT@": ctx.attr.component,
            "@ORIGIN@": ctx.attr.origin,
            "@VERSION@": ctx.attr.version,
            "@SOVERSION@": ctx.attr.soversion,
        },
    )
    return DefaultInfo(files = depset([output]))

_version_header = rule(
    implementation = _version_header_impl,
    attrs = {
        "component": attr.string(mandatory = True),
        "origin": attr.string(mandatory = True),
        "version": attr.string(mandatory = True),
        "soversion": attr.string(mandatory = True),
        "template": attr.label(allow_single_file = True, mandatory = True),
    },
)

def urma_version_header(name, version_info, visibility = None):
    """Generate the public Bazel artifact version header."""
    vis = visibility if visibility != None else ["//visibility:public"]
    _version_header(
        name = name,
        component = version_info.component,
        origin = version_info.origin,
        version = version_info.version,
        soversion = version_info.soversion,
        template = "//bazel:urma_version.h.tpl",
        visibility = vis,
    )

def _versioned_artifact_impl(ctx):
    source = ctx.file.src
    output_dir = "versioned/provider" if ctx.attr.kind == "provider" else "versioned/lib"
    real_file = ctx.actions.declare_file(output_dir + "/" + ctx.attr.file_name)
    outputs = [real_file]
    arguments = [source.path, real_file.path]

    if ctx.attr.versioned_links:
        soname_link = ctx.actions.declare_file(output_dir + "/" + ctx.attr.soname)
        unversioned_link = ctx.actions.declare_file(output_dir + "/" + ctx.attr.link_name)
        outputs.extend([soname_link, unversioned_link])
        arguments.extend([soname_link.path, unversioned_link.path])

    command = """
set -eu
src="$1"
real_file="$2"
mkdir -p "$(dirname "${real_file}")"
cp "${src}" "${real_file}"
chmod 0755 "${real_file}"
if [ "$#" -eq 4 ]; then
    soname_link="$3"
    unversioned_link="$4"
    ln -s "$(basename "${real_file}")" "${soname_link}"
    ln -s "$(basename "${soname_link}")" "${unversioned_link}"
fi
"""
    ctx.actions.run_shell(
        inputs = [source],
        outputs = outputs,
        arguments = arguments,
        command = command,
        mnemonic = "UrmaVersionedSharedObject",
        progress_message = "Versioning %{label}",
    )
    return DefaultInfo(files = depset(outputs))

_versioned_artifact = rule(
    implementation = _versioned_artifact_impl,
    attrs = {
        "src": attr.label(allow_single_file = True, mandatory = True),
        "file_name": attr.string(mandatory = True),
        "soname": attr.string(mandatory = True),
        "link_name": attr.string(mandatory = True),
        "kind": attr.string(values = ["library", "provider"], mandatory = True),
        "versioned_links": attr.bool(mandatory = True),
    },
)

def urma_versioned_artifact(name, src, version_info, visibility = None):
    """Create the installable real SO and relative symlinks when applicable."""
    vis = visibility if visibility != None else ["//visibility:public"]
    _versioned_artifact(
        name = name,
        src = src,
        file_name = version_info.file_name,
        soname = version_info.soname,
        link_name = version_info.link_name,
        kind = version_info.kind,
        versioned_links = version_info.versioned_links,
        visibility = vis,
    )

def _version_manifest_impl(ctx):
    json_output = ctx.actions.declare_file("versioned/metadata/urma_versions.json")
    tsv_output = ctx.actions.declare_file("versioned/metadata/urma_versions.tsv")
    ctx.actions.write(json_output, ctx.attr.json_content)
    ctx.actions.write(tsv_output, ctx.attr.tsv_content)
    return DefaultInfo(files = depset([json_output, tsv_output]))

_version_manifest = rule(
    implementation = _version_manifest_impl,
    attrs = {
        "json_content": attr.string(mandatory = True),
        "tsv_content": attr.string(mandatory = True),
    },
)

def _json_string(value):
    return "\"" + value.replace("\\", "\\\\").replace("\"", "\\\"") + "\""

def _install_path(entry):
    install_dir = entry.install_dir[1:] if entry.install_dir.startswith("/") else entry.install_dir
    return install_dir + "/" + entry.file_name

def urma_version_manifest(name, entries, bazel_version, package_version, visibility = None):
    """Generate JSON and TSV manifests for every versioned Bazel artifact."""
    json_lines = [
        "{",
        "  \"schema_version\": 1,",
        "  \"package_version\": " + _json_string(package_version) + ",",
        "  \"bazel_version\": " + _json_string(bazel_version) + ",",
        "  \"artifacts\": [",
    ]
    tsv_lines = ["type\tcomponent\tpath\tversion\tsoversion\tsoname\torigin"]

    for index, entry in enumerate(entries):
        path = _install_path(entry)
        suffix = "," if index + 1 < len(entries) else ""
        json_lines.extend([
            "    {",
            "      \"type\": " + _json_string(entry.artifact_type) + ",",
            "      \"component\": " + _json_string(entry.component) + ",",
            "      \"path\": " + _json_string(path) + ",",
            "      \"version\": " + _json_string(entry.version) + ",",
            "      \"soversion\": " + _json_string(entry.soversion) + ",",
            "      \"soname\": " + _json_string(entry.soname) + ",",
            "      \"origin\": " + _json_string(entry.origin),
            "    }" + suffix,
        ])
        tsv_lines.append("\t".join([
            entry.artifact_type,
            entry.component,
            path,
            entry.version,
            entry.soversion,
            entry.soname,
            entry.origin,
        ]))

    json_lines.extend(["  ]", "}", ""])
    tsv_lines.append("")
    vis = visibility if visibility != None else ["//visibility:public"]
    _version_manifest(
        name = name,
        json_content = "\n".join(json_lines),
        tsv_content = "\n".join(tsv_lines),
        visibility = vis,
    )
