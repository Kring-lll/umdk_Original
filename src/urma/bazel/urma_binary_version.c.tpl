/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bazel-generated executable version metadata and option handling
 * Author: URMA Team
 * Create: 2026-07-18
 */

#include <stdio.h>
#include <string.h>

#if defined(__GNUC__)
#define URMA_VERSION_SECTION __attribute__((used, visibility("hidden"), section(".urma.version")))
#else
#define URMA_VERSION_SECTION
#endif

/* Immutable marker used by the Bazel artifact consistency checker. */
const char @SYMBOL@[] URMA_VERSION_SECTION =
    "URMA_META_V1|type=binary|component=@COMPONENT@|version=@VERSION@|"
    "soversion=@SOVERSION@|soname=-|origin=@ORIGIN@\n";

/* These names are defined by GNU ld's --wrap option. */
int __real_main(int argc, char *argv[]);

int __wrap_main(int argc, char *argv[])
{
    int ret;

    if (argc == 2 && argv != NULL && argv[1] != NULL &&
        (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)) {
        ret = printf("@COMPONENT@ version @VERSION@ (soversion @SOVERSION@)\n");
        return ret < 0 ? 1 : 0;
    }

    return __real_main(argc, argv);
}
