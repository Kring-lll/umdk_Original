/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: shared log helpers for URMA tools
 * Author: Codex
 * Create: 2026-06-17
 * Note:
 * History: 2026-06-17 Create file
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdarg.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <syslog.h>
#include <unistd.h>

#include "urma_tools_log.h"

#define MAX_LOG_LEN           1024
#define MAX_LOG_TAG_LEN       64
#define URMA_LOG_TAG          "URMA"
#define DEFAULT_TOOLS_LOG_TAG "urma_tools"
#define VLOG_LEVEL_AUDIT      LOG_INFO

static urma_tools_vlog_level_t vlog_level = VLOG_LEVEL_INFO;
static char tools_log_tag[MAX_LOG_TAG_LEN] = DEFAULT_TOOLS_LOG_TAG;

void vlog_set_tag(const char *tag)
{
    if (tag == NULL || tag[0] == '\0') {
        (void)snprintf(tools_log_tag, sizeof(tools_log_tag), "%s", DEFAULT_TOOLS_LOG_TAG);
        return;
    }

    (void)snprintf(tools_log_tag, sizeof(tools_log_tag), "%s", tag);
}

void vlog_set_level(urma_tools_vlog_level_t level)
{
    vlog_level = level;
}

urma_tools_vlog_level_t vlog_get_level(void)
{
    return vlog_level;
}

void vlog_print(FILE *stream, urma_tools_vlog_level_t level, const char *fmt, ...)
{
    if (vlog_level < level) {
        return;
    }

    va_list va;
    va_start(va, fmt);
    (void)vfprintf(stream, fmt, va);
    va_end(va);
}

void vlog_audit(const char *fmt, ...)
{
    static __thread long tid = -1;
    if (tid == -1) {
        tid = syscall(__NR_gettid);
    }

    /* URMA|tool_tag|tid|format */
    char logmsg[MAX_LOG_LEN + 1] = {0};
    int ret = snprintf(logmsg, sizeof(logmsg), "%s|%s|%ld|",
                       URMA_LOG_TAG, tools_log_tag, tid);
    if (ret <= 0 || ret >= (int)sizeof(logmsg)) {
        return;
    }

    size_t offset = (size_t)ret;
    va_list va;
    va_start(va, fmt);
    (void)vsnprintf(logmsg + offset, sizeof(logmsg) - offset, fmt, va);
    va_end(va);

    syslog(VLOG_LEVEL_AUDIT, "%s", logmsg);
}
