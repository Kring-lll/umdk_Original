/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: SNC (Supernode Network Controller) service
 * Create: 2026-08-20
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include "urma_ping_log.h"

static FILE* g_logFile = NULL;
static pthread_mutex_t g_logMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_initMutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_isInited = false;

static const char* LevelToStr(int level)
{
    if (level == LOG_LEVEL_INFO) {
        return "INFO";
    } else {
        return "ERROR";
    }
}

int SncLogInit(void)
{
    pthread_mutex_lock(&g_initMutex);
    if (g_isInited) {
        pthread_mutex_unlock(&g_initMutex);
        return 0;
    }
    const char* logDir = getenv("SNC_LOG_DIR");
    if (logDir == NULL || strlen(logDir) == 0) {
        (void)fprintf(stderr, "SncLogInit: SNC_LOG_DIR is not set or empty\n");
        g_isInited = true;
        pthread_mutex_unlock(&g_initMutex);
        return 0;
    }
    size_t fullPathLen = strlen(logDir) + 32;
    char* logPath = (char*)malloc(fullPathLen);
    if (logPath == NULL) {
        (void)fprintf(stderr, "SncLogInit: failed to allocate memory for log path\n");
        pthread_mutex_unlock(&g_initMutex);
        return 0;
    }
    int snprintfRet = snprintf(logPath, fullPathLen, "%s/snc.log", logDir);
    if (snprintfRet < 0 || (size_t)snprintfRet >= fullPathLen) {
        (void)fprintf(stderr, "SncLogInit: log path snprintf failed or truncated (ret=%d)\n", snprintfRet);
        free(logPath);
        g_isInited = true;
        pthread_mutex_unlock(&g_initMutex);
        return 0;
    }
    g_logFile = fopen(logPath, "a");
    if (g_logFile == NULL) {
        (void)fprintf(stderr, "SncLogInit: failed to open log file %s\n", logPath);
        free(logPath);
        g_isInited = true;
        pthread_mutex_unlock(&g_initMutex);
        return 0;
    }
    free(logPath);
    g_isInited = true;
    pthread_mutex_unlock(&g_initMutex);
    return 0;
}

void SncLogDeinit(void)
{
    pthread_mutex_lock(&g_initMutex);
    if (!g_isInited) {
        pthread_mutex_unlock(&g_initMutex);
        return;
    }
    pthread_mutex_lock(&g_logMutex);
    if (g_logFile != NULL) {
        fclose(g_logFile);
        g_logFile = NULL;
    }
    g_isInited = false;
    pthread_mutex_unlock(&g_logMutex);
    pthread_mutex_unlock(&g_initMutex);
}

static void SncLogWriteInternal(int level, const char* fmt, va_list args)
{
    struct tm tmInfo;
    const int TIME_LEN = 32;
    time_t now = time(NULL);
    localtime_r(&now, &tmInfo);
    char timeStr[TIME_LEN];
    timeStr[0] = '\0';
    if (strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &tmInfo) == 0) {
        /* strftime returns 0 on failure or truncation; fall back to placeholder */
        if (snprintf(timeStr, sizeof(timeStr), "%s", "----") < 0) {
            timeStr[0] = '\0';
        }
    }
    pthread_mutex_lock(&g_logMutex);
    FILE* output = g_logFile != NULL ? g_logFile : stdout;
    int ret = fprintf(output, "[%s] [%s] ", timeStr, LevelToStr(level));
    if (ret >= 0) {
        ret = vfprintf(output, fmt, args);
    }
    if (ret >= 0) {
        ret = fprintf(output, "\n");
    }
    if (ret >= 0 && fflush(output) != 0) {
        ret = -1;
    }
    if (ret < 0 && output == g_logFile) {
        /* writing to the log file failed; close it so future logs fall back to stdout */
        (void)fclose(g_logFile);
        g_logFile = NULL;
    }
    pthread_mutex_unlock(&g_logMutex);
}

void SncLogWrite(int level, const char* fmt, ...)
{
    if (!g_isInited) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    SncLogWriteInternal(level, fmt, args);
    va_end(args);
}