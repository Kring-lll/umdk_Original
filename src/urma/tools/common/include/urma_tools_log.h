/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: shared log helpers for URMA tools
 * Author: Codex
 * Create: 2026-06-17
 * Note:
 * History: 2026-06-17 Create file
 */

#ifndef URMA_TOOLS_LOG_H
#define URMA_TOOLS_LOG_H

#include <stdarg.h>
#include <stdio.h>

typedef enum urma_tools_vlog_level {
    VLOG_LEVEL_QUIET = 0,
    VLOG_LEVEL_INFO = 1,
    VLOG_LEVEL_VERBOSE = 2,
    VLOG_LEVEL_VVERBOSE = 3,
} urma_tools_vlog_level_t;

void vlog_set_tag(const char *tag);
void vlog_set_level(urma_tools_vlog_level_t level);
urma_tools_vlog_level_t vlog_get_level(void);
void vlog_print(FILE *stream, urma_tools_vlog_level_t level, const char *fmt, ...);
void vlog_audit(const char *fmt, ...);

#define VLOG_QUIET(...)    vlog_print(stdout, VLOG_LEVEL_QUIET, __VA_ARGS__)
#define VLOG_INFO(...)     vlog_print(stdout, VLOG_LEVEL_INFO, __VA_ARGS__)
#define VLOG_VERBOSE(...)  vlog_print(stdout, VLOG_LEVEL_VERBOSE, __VA_ARGS__)
#define VLOG_VVERBOSE(...) vlog_print(stdout, VLOG_LEVEL_VVERBOSE, __VA_ARGS__)
#define VLOG_ERROR(...)    vlog_print(stderr, VLOG_LEVEL_QUIET, __VA_ARGS__)
#define VLOG_AUDIT(...)    vlog_audit(__VA_ARGS__)

#endif
