/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Timing wheel internal header
 */

#ifndef BONDP_TIMEWHEEL_H
#define BONDP_TIMEWHEEL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TW_DEFAULT_TICK_MS (10U)
#define TW_DEFAULT_SLOT_NUM (512U)

#define TW_OK (0)
#define TW_SYS_ERROR (-1)
#define TW_NOT_FOUND_ERROR (-2)
#define TW_ALREADY_CANCELED_ERROR (-3)
#define TW_ALREADY_FIRED_ERROR (-4)
#define TW_ALREADY_RUNNING_ERROR (-5)
#define TW_NO_MEMORY_ERROR (-12)
#define TW_INVALID_PARAM_ERROR (-22)
#define TW_STOPPING_ERROR (-108)

typedef uint64_t tw_task_id_t;

typedef void (*tw_task_fn_t)(void *arg);

typedef struct tw tw_t;

typedef struct tw_cfg {
    uint32_t tick_ms;
    uint32_t slot_num;
} tw_cfg_t;

tw_t *tw_create(const tw_cfg_t *cfg);

void tw_destroy(tw_t *tw);

int tw_schedule(tw_t *tw, uint64_t delay_ms, tw_task_fn_t fn, void *arg, tw_task_id_t *task_id);

int tw_cancel(tw_t *tw, tw_task_id_t task_id);

uint32_t tw_get_tick_ms(const tw_t *tw);

void tw_advance(tw_t *tw, uint64_t tick_cnt);

#ifdef __cplusplus
}
#endif

#endif // BONDP_TIMEWHEEL_H
