/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: urma_perftest log implementation file
 * Author: OpenAI Codex
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 Create file
 */

#include <stdarg.h>
#include <stdio.h>

#include "perftest_log.h"

static int g_verbose_level = VLOG_LEVEL_NORMAL;

void verbose_set_level(perftest_vlog_level_t level)
{
    g_verbose_level = level;
}

void verbose_print(FILE *stream, perftest_vlog_level_t level, const char *fmt, ...)
{
    if (g_verbose_level < level) {
        return;
    }

    va_list va;
    va_start(va, fmt);
    (void)vfprintf(stream, fmt, va);
    va_end(va);
}
