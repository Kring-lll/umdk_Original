/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bazel-generated shared-object version metadata
 * Author: URMA Team
 * Create: 2026-07-18
 */

#if defined(__GNUC__)
#define URMA_VERSION_SECTION __attribute__((used, visibility("hidden"), section(".urma.version")))
#else
#define URMA_VERSION_SECTION
#endif

/* Hidden link-time anchor retained only to expose immutable SO metadata. */
const char @SYMBOL@[] URMA_VERSION_SECTION =
    "URMA_META_V1|type=so|component=@COMPONENT@|version=@VERSION@|"
    "soversion=@SOVERSION@|soname=@SONAME@|origin=@ORIGIN@\n";
