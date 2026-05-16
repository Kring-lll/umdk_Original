/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 * Description: urma log head file
 * Author: Yan Fangfang, Qian Guoxin
 * Create: 2020-9-28
 * Note:
 * History: 2020-9-28 delare urma log API
 */

#ifndef URMA_LOG_H
#define URMA_LOG_H

#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>
#include <urma_types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_FORMAT_IDX 4 /* index of 'format' of urma_log */
#define LOG_VA_ARG_IDX 5 /* index of variable argument of urma_log */

/* Rate limit configuration */
#define URMA_LOG_RL_WINDOW_SEC      5       /* Rate limit time window (seconds) */
#define URMA_LOG_RL_LIMIT           100     /* Max logs per window */

/* Rate limit state flags */
#define URMA_LOG_RL_INITIALIZED     0x01

/* Rate limit state structure - per log point */
typedef struct urma_log_rl_state {
    pthread_spinlock_t lock;         /* Spinlock to protect window reset */
    atomic_uint        n_left;       /* Remaining quota in window (atomic) */
    atomic_uint        missed;       /* Number of suppressed logs (atomic) */
    time_t             begin;        /* Window start time */
    uint32_t           flags;        /* State flags */
} urma_log_rl_state_t;

/* Static initialization macro */
#define URMA_LOG_RL_STATE_INIT(name) { \
    .lock = PTHREAD_SPINLOCK_INITIALIZER, \
    .n_left = ATOMIC_VAR_INIT(URMA_LOG_RL_LIMIT), \
    .missed = ATOMIC_VAR_INIT(0), \
    .flags = 0 \
}

/* Define rate limit state variable */
#define DEFINE_URMA_LOG_RL_STATE(name) \
    static urma_log_rl_state_t name = URMA_LOG_RL_STATE_INIT(name)

int urma_log_init(void);
void urma_getenv_log_level(void);
void urma_getenv_log_separator(void);
bool urma_log_drop(urma_vlog_level_t level);
void __attribute__((format(printf, LOG_FORMAT_IDX, LOG_VA_ARG_IDX)))
urma_log(const char *function, int line, urma_vlog_level_t level, const char *format, ...);
const char *urma_get_level_print(urma_vlog_level_t level);
urma_vlog_level_t urma_log_get_level_from_string(const char *level_string);

/**
 * @brief Check if log should be rate limited
 * @param rs Rate limit state pointer
 * @return true: log can be recorded
 *         false: log should be suppressed
 */
bool urma_log_rl_check(urma_log_rl_state_t *rs);

#define URMA_LOG(l, ...)                                                                                               \
    if (!urma_log_drop(URMA_VLOG_LEVEL_##l)) {                                                                         \
        urma_log(__func__, __LINE__, URMA_VLOG_LEVEL_##l, __VA_ARGS__);                                                \
    }

#define URMA_LOG_INFO(...) URMA_LOG(INFO, __VA_ARGS__)

#define URMA_LOG_ERR(...) URMA_LOG(ERR, __VA_ARGS__)

#define URMA_LOG_WARN(...) URMA_LOG(WARNING, __VA_ARGS__)

#define URMA_LOG_DEBUG(...) URMA_LOG(DEBUG, __VA_ARGS__)

/* Internal rate limit log macro */
#define URMA_LOG_RL(l, rs, ...) \
    do { \
        if (!urma_log_drop(URMA_VLOG_LEVEL_##l)) { \
            if (urma_log_rl_check(rs)) { \
                urma_log(__func__, __LINE__, URMA_VLOG_LEVEL_##l, __VA_ARGS__); \
            } \
        } \
    } while (0)

/* Rate limit log macros - each call defines independent rate limit state */
#define URMA_LOG_ERR_RL(...) \
    do { \
        DEFINE_URMA_LOG_RL_STATE(_rs); \
        URMA_LOG_RL(ERR, &_rs, __VA_ARGS__); \
    } while (0)

#define URMA_LOG_WARN_RL(...) \
    do { \
        DEFINE_URMA_LOG_RL_STATE(_rs); \
        URMA_LOG_RL(WARNING, &_rs, __VA_ARGS__); \
    } while (0)

#define URMA_LOG_INFO_RL(...) \
    do { \
        DEFINE_URMA_LOG_RL_STATE(_rs); \
        URMA_LOG_RL(INFO, &_rs, __VA_ARGS__); \
    } while (0)

#define URMA_LOG_DEBUG_RL(...) \
    do { \
        DEFINE_URMA_LOG_RL_STATE(_rs); \
        URMA_LOG_RL(DEBUG, &_rs, __VA_ARGS__); \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif
