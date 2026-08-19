/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: SNC (Supernode Network Controller) service
 * Create: 2026-08-20
 */
#ifndef URMA_PING_LOG_H
#define URMA_PING_LOG_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#define LOG_LEVEL_DEBUG 0
#define LOG_LEVEL_INFO  1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_ERROR 3

#define LOG_DEBUG(fmt, ...) SncLogWrite(LOG_LEVEL_DEBUG, "[URMA_PING] " fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  SncLogWrite(LOG_LEVEL_INFO,  "[URMA_PING] " fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  SncLogWrite(LOG_LEVEL_WARN,  "[URMA_PING] " fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) SncLogWrite(LOG_LEVEL_ERROR, "[URMA_PING] " fmt, ##__VA_ARGS__)

int SncLogInit(void);
void SncLogDeinit(void);
void SncLogWrite(int level, const char *fmt, ...);

#endif