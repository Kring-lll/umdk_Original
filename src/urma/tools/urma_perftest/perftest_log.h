/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: urma_perftest log head file
 * Author: OpenAI Codex
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 Create file
 */

#ifndef PERFTEST_LOG_H
#define PERFTEST_LOG_H

#include <stdarg.h>
#include <stdio.h>

typedef enum perftest_vlog_level {
    VLOG_LEVEL_QUIET = 0,
    VLOG_LEVEL_NORMAL = 1,
    VLOG_LEVEL_VERBOSE = 2,
    VLOG_LEVEL_VVERBOSE = 3,
} perftest_vlog_level_t;

void verbose_set_level(perftest_vlog_level_t level);
void verbose_print(FILE *stream, perftest_vlog_level_t level, const char *fmt, ...);

#define LOG_QUIET(...)    verbose_print(stdout, VLOG_LEVEL_QUIET, __VA_ARGS__)
#define LOG_NORMAL(...)   verbose_print(stdout, VLOG_LEVEL_NORMAL, __VA_ARGS__)
#define LOG_VERBOSE(...)  verbose_print(stdout, VLOG_LEVEL_VERBOSE, __VA_ARGS__)
#define LOG_VVERBOSE(...) verbose_print(stdout, VLOG_LEVEL_VVERBOSE, __VA_ARGS__)
#define LOG_ERROR(...)    verbose_print(stderr, VLOG_LEVEL_QUIET, __VA_ARGS__)

#endif
