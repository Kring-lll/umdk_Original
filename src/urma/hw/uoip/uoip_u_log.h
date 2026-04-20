/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2025. All rights reserved.
 * Description: uoip (URMA over IP) provider — logging header
 */

#ifndef UOIP_U_LOG_H
#define UOIP_U_LOG_H

#include <stdio.h>
#include <syslog.h>

/* Log to stderr; tag every line so they are easy to grep */
#define UOIP_LOG(level, fmt, ...)                                          \
    do {                                                                    \
        fprintf(stderr, "UOIP|" level "|%s:%d| " fmt,                     \
                __func__, __LINE__, ##__VA_ARGS__);                        \
        syslog(LOG_INFO, "UOIP|" level "|%s:%d| " fmt,                    \
               __func__, __LINE__, ##__VA_ARGS__);                         \
    } while (0)

#define UOIP_LOG_ERR(fmt, ...)  UOIP_LOG("ERR",  fmt, ##__VA_ARGS__)
#define UOIP_LOG_WARN(fmt, ...) UOIP_LOG("WARN", fmt, ##__VA_ARGS__)
#define UOIP_LOG_INFO(fmt, ...) UOIP_LOG("INFO", fmt, ##__VA_ARGS__)
#define UOIP_LOG_DBG(fmt, ...)  /* disabled by default */

#endif /* UOIP_U_LOG_H */
