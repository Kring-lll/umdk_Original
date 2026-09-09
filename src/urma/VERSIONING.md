# URMA version contract

The version source is src/urma/CMakeLists.txt, currently 1.0.0.
The installed urma_version.h, included by urma_api.h, exposes
URMA_API_VERSION_MAJOR, URMA_API_VERSION_MINOR, URMA_API_VERSION_PATCH,
URMA_API_VERSION_NUMBER and URMA_API_VERSION_ENCODE(major, minor, patch).
The numeric layout is unsigned 64-bit, with 32/16/16-bit fields.

## Source compatibility

Consumers opt into requirements before the first inclusion:

    #define URMA_API_REQUIRED_MAJOR 1
    #define URMA_API_MIN_VERSION URMA_API_VERSION_ENCODE(1, 0, 0)
    #include <urma_api.h>

A different major or insufficient version causes #error. Compiler type
checking still applies. Consumers can also write their own checks:

    #include <urma_api.h>
    #if URMA_API_VERSION < URMA_API_VERSION_ENCODE(3, 2, 0)
    #error "URMA API >= 3.2 is required"
    #endif

The second example intentionally fails with version 1.0.0.

## Binary compatibility

liburma.so.1.0.0 has SONAME liburma.so.1. Its existing exports belong to
the stable URMA_1.0 symbol node. Newly linked consumers record that node
for referenced symbols. Retain existing nodes in compatible releases;
incompatible ABI changes require a new SONAME major. The node now has an explicit source baseline; compare it with the release
ELF symbol table before publishing.

Missing SONAMEs and unavailable required symbol versions are diagnosed by
the dynamic linker. Missing function symbols can otherwise fail on first
use with lazy binding. CMake consumers linking the urma target inherit
-Wl,-z,now; other consumers must link their executable with -Wl,-z,now
or run with LD_BIND_NOW=1 to require startup resolution. A library's own
NOW flag does not enforce immediate binding for all executable relocations.
dlopen users should use RTLD_NOW.

Symbol versioning is not retroactive for old unversioned binaries. It
does not prove type/layout compatibility or reject every unversioned
replacement library: the dynamic linker can allow unversioned fallbacks.

.note.urma.version in liburma and liburma_common is diagnostic metadata,
not an automatic loader compatibility check.

On a Linux build, verify with:

    readelf -d liburma.so.1.0.0
    readelf --version-info liburma.so.1.0.0
    readelf --dyn-syms --wide liburma.so.1.0.0
    readelf -n liburma.so.1.0.0

Expect SONAME liburma.so.1, symbol node URMA_1.0, and a URMA note with an
8-byte native-endian descriptor containing 0x0000000100000000.


## Backward-compatible interface evolution

Only current public headers are installed. Historical layouts and adapters
belong in lib/urma/core/compat and are not installed. Compatibility C files
are compiled into the shared target only. Static linking exposes the current
API and does not promise compatibility with old binaries.

For compatible 1.1.0 keep SONAME major 1 and the URMA_1.0 node. Unchanged
symbols stay in their original node; new symbols go into URMA_1.1 inheriting
URMA_1.0. Do not move all symbols to the latest node. ABI-changed functions
retain foo@URMA_1.0; foo@@URMA_1.1 is the new default. Two single-@ aliases
do not establish the default version for new links.

Illustrative future interface (not a released URMA API):

    /* Current public header */
    struct foo_args { int value; unsigned int flags; };
    int foo(const struct foo_args *args);

    /* Private historical layout */
    struct foo_args_v1 { int value; };

The implementation includes errno.h, the current header, the private layout
and compat/urma_symver.h:

    static int foo_current(const struct foo_args *args)
    {
        if (!args || args->flags != 0)
            return -EINVAL;
        return args->value;
    }

    #ifdef URMA_BUILD_SHARED
    int urma_compat_foo_v1(const struct foo_args_v1 *old)
    {
        struct foo_args current;
        if (!old)
            return -EINVAL;
        current.value = old->value;
        current.flags = 0; /* Preserve the historical behavior. */
        return foo_current(&current);
    }
    int urma_compat_foo_current(const struct foo_args *args)
    {
        return foo_current(args);
    }
    URMA_SYMVER_COMPAT(urma_compat_foo_v1, foo, "URMA_1.0");
    URMA_SYMVER_DEFAULT(urma_compat_foo_current, foo, "URMA_1.1");
    #else
    int foo(const struct foo_args *args)
    {
        return foo_current(args);
    }
    #endif

Each alias must be in the same translation unit as its implementation.
For this example the future map would contain:

    URMA_1.0 {
        global: foo; /* Also retain every existing baseline symbol. */
        local: *;
    };
    URMA_1.1 {
        global: foo;
    } URMA_1.0;

Never cast historical structs to current layouts. Translate fields, defaults,
errors, ownership and lifetime explicitly. Callbacks and opaque state may
require private handle adapters. Preserve historical size, alignment and
offsets on each supported architecture. Inline code and direct field accesses
in old binaries cannot be repaired by symbol aliases. If historical semantics
cannot be preserved, do not claim backward compatibility.

Before release, run an unchanged old-library client with the replacement
library, test old semantics, and also test a new-header client. Inspect
readelf --dyn-syms --wide and --version-info for old @ and current @@ entries
and verify that urma_compat_* implementation names are hidden.
Current 1.0.0 adds no artificial historical interface or 1.1 node.
