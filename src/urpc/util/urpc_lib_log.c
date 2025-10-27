/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: urpc lib log
 * Create: 2024-3-12
 */

#include <stdarg.h>
#include <stdio.h>
#include <pthread.h>
#include <syslog.h>

#include "urma_api.h"
#include "urpc_framework_errno.h"
#include "urpc_util.h"
#include "urpc_lib_log.h"

#define URPC_LIB_LOG_SIZE 1024

static void urpc_log_default_print(int level, char *log_msg);

static urpc_log_config_t g_urpc_log_config = {
    .func = urpc_log_default_print,
    .level = URPC_LOG_LEVEL_INFO,
    .rate_limited.interval_ms = URPC_LOG_PRINT_PERIOD_MS,
    .rate_limited.num = URPC_LOG_PRINT_TIMES,
};
pthread_mutex_t g_urpc_log_lock = PTHREAD_MUTEX_INITIALIZER;

static bool next_print_cycle(uint64_t *last_time, uint32_t time_interval)
{
    uint64_t now_time = urpc_get_cpu_cycles();
    if (((now_time - *last_time) * MS_PER_SEC / urpc_get_cpu_hz()) >= time_interval) {
        *last_time = now_time;
        return true;
    }
    return false;
}

static void urpc_log_default_print(int level, char *log_msg)
{
    syslog(level, "%s", log_msg);
}

bool urpc_log_drop(urpc_log_level_t level)
{
    if (level > g_urpc_log_config.level) {
        return true;
    }
    return false;
}

void urpc_log(urpc_log_level_t level, const char *function, int line, const char *format, ...)
{
    char log_msg[URPC_LIB_LOG_SIZE] = {0};
    const char urpc_log_name[] = "URPC_LOG";
    int len = snprintf(log_msg, URPC_LIB_LOG_SIZE, "%s|%s[%d]|", urpc_log_name, function, line);
    if (len < 0) {
        return;
    }

    va_list va;
    va_start(va, format);
    len = vsnprintf(&log_msg[len], (size_t)(URPC_LIB_LOG_SIZE - len), format, va);
    va_end(va);
    if (len < 0) {
        return;
    }

    g_urpc_log_config.func((int)level, log_msg);
}

bool urpc_log_limit(uint32_t *print_count, uint64_t *last_time)
{
    if (g_urpc_log_config.rate_limited.interval_ms == 0) {
        return true;
    }

    if (next_print_cycle(last_time, g_urpc_log_config.rate_limited.interval_ms)) {
        *print_count = 0;
    }

    if (*print_count < g_urpc_log_config.rate_limited.num) {
        *print_count += 1;
        return true;
    }

    return false;
}

int urpc_log_config_set(urpc_log_config_t *config)
{
    if (config == NULL) {
        URPC_LIB_LOG_ERR("invalid configure\n");
        return -URPC_ERR_EINVAL;
    }

    if ((config->log_flag & URPC_LOG_FLAG_LEVEL) &&
        (config->level < URPC_LOG_LEVEL_ERR || config->level >= URPC_LOG_LEVEL_MAX)) {
        URPC_LIB_LOG_ERR("invalid log level %d\n", config->level);
        return URPC_FAIL;
    }

    (void)pthread_mutex_lock(&g_urpc_log_lock);
    if (config->log_flag & URPC_LOG_FLAG_FUNC) {
        if (config->func == NULL) {
            g_urpc_log_config.func = urpc_log_default_print;
            urma_unregister_log_func();
            URPC_LIB_LOG_INFO("set log configuration successful, log output function: default\n");
        } else {
            g_urpc_log_config.func = config->func;
            urma_register_log_func(config->func);
            URPC_LIB_LOG_INFO("set log configuration successful, log output function: user defined\n");
        }
    }

    if (config->log_flag & URPC_LOG_FLAG_LEVEL) {
        g_urpc_log_config.level = config->level;
        urma_log_set_level((urma_vlog_level_t)config->level);
        URPC_LIB_LOG_INFO("set log configuration successful, log level: %d\n", config->level);
    }

    if ((config->log_flag & URPC_LOG_FLAG_RATE_LIMITED)) {
        g_urpc_log_config.rate_limited.interval_ms = config->rate_limited.interval_ms;
        g_urpc_log_config.rate_limited.num = config->rate_limited.num;
        URPC_LIB_LOG_INFO("set log configuration successful, limited interval(ms): %u, limited num: %u\n",
            config->rate_limited.interval_ms, config->rate_limited.num);
    }
    (void)pthread_mutex_unlock(&g_urpc_log_lock);

    return URPC_SUCCESS;
}

int urpc_log_config_get(urpc_log_config_t *config)
{
    if (config == NULL) {
        URPC_LIB_LOG_ERR("invalid parameter\n");
        return -URPC_ERR_EINVAL;
    }

    (void)pthread_mutex_lock(&g_urpc_log_lock);
    *config = g_urpc_log_config;
    if (config->func == urpc_log_default_print) {
        config->func = NULL;
    }
    (void)pthread_mutex_unlock(&g_urpc_log_lock);

    return URPC_SUCCESS;
}