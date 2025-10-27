/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: Provide vlog module
 * Create: 2025-07-29
 */

#include <stdarg.h>

#include "util_vlog.h"

typedef struct util_vlog_level_def {
    const char *output_name;
    const char **alias_names;
} util_vlog_level_def_t;

static const char *g_emerg_alias_names_def[]   = { "emerg", "emergency", "0", NULL };
static const char *g_alert_alias_names_def[]   = { "alert", "1", NULL };
static const char *g_crit_alias_names_def[]   = { "crit", "critical", "2", NULL };
static const char *g_err_alias_names_def[]   = { "err", "error", "3", NULL };
static const char *g_warn_alias_names_def[]   = { "warn", "warning", "4", NULL };
static const char *g_notice_alias_names_def[]   = { "notice", "5", NULL };
static const char *g_info_alias_names_def[]   = { "info", "informational", "6", NULL };
static const char *g_debug_alias_names_def[]   = { "debug", "7", NULL };

static const util_vlog_level_def_t g_util_vlog_level_def[] = {
    { "EMERG", g_emerg_alias_names_def },
    { "ALERT", g_alert_alias_names_def },
    { "CRIT", g_crit_alias_names_def },
    { "ERR", g_err_alias_names_def },
    { "WARN", g_warn_alias_names_def },
    { "NOTICE", g_notice_alias_names_def },
    { "INFO", g_info_alias_names_def },
    { "DEBUG", g_debug_alias_names_def },
};

int util_vlog_create_context(util_vlog_level_t level, char *vlog_name, util_vlog_ctx_t *ctx)
{
    static util_vlog_ctx_t default_ctx = {
        .level = UTIL_VLOG_LEVEL_INFO,
        .vlog_name = "UTIL_VLOG",
    };
    if (level < 0 || level > UTIL_VLOG_LEVEL_MAX || ctx == NULL) {
        if (level < 0 || level > UTIL_VLOG_LEVEL_MAX) {
            UTIL_VLOG_ERR(&default_ctx, "Invalid vlog level(%d)\n", level);
        } else if (ctx == NULL) {
            UTIL_VLOG_ERR(&default_ctx, "Invalid vlog context input, which is NULL\n");
        }
        
        return -1;
    }

    ctx->level = level;
    if (vlog_name != NULL) {
        (void)strncpy(ctx->vlog_name, vlog_name, UTIL_VLOG_NAME_STR_LEN - 1);
    }

    return 0;
}

void util_vlog_output(
    util_vlog_ctx_t *ctx, util_vlog_level_t level, const char *function, int line, const char *format, ...)
{
    char log_msg[UTIL_VLOG_SIZE];
    int len = snprintf(log_msg, UTIL_VLOG_SIZE, "%s|%s[%d]|", ctx->vlog_name, function, line);
    if (len < 0) {
        return;
    }

    va_list va;
    va_start(va, format);
    len = vsnprintf(&log_msg[len], (size_t)(UTIL_VLOG_SIZE - len), format, va);
    va_end(va);
    if (len < 0) {
        return;
    }

    syslog(level, "%s", log_msg);
}

util_vlog_level_t util_vlog_level_converter_from_str(const char *str, util_vlog_level_t default_level)
{
    int array_size = (int)sizeof(g_util_vlog_level_def) / (int)sizeof(g_util_vlog_level_def[0]);
    for (int i = 0; i < array_size; ++i) {
        for (const char **name_def = g_util_vlog_level_def[i].alias_names; *name_def != NULL; ++name_def) {
            if (strcasecmp(str, *name_def) == 0) {
                return (util_vlog_level_t)i;
            }
        }
    }

    return default_level;
}

const char *util_vlog_level_converter_to_str(util_vlog_level_t level)
{
    return g_util_vlog_level_def[level].output_name;
}