/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bond worker thread header
 */

#ifndef BONDP_WORKER_H
#define BONDP_WORKER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t bondp_worker_task_id_t;

typedef void (*bondp_worker_task_fn_t)(void *arg);
typedef void (*bondp_worker_event_fn_t)(int fd, void *arg);

int bondp_worker_create(void);

void bondp_worker_destroy(void);

int bondp_worker_schedule(uint64_t delay_ms, bondp_worker_task_fn_t fn, void *arg, bondp_worker_task_id_t *task_id);

int bondp_worker_cancel(bondp_worker_task_id_t task_id);

int bondp_worker_add_fd(int fd, bondp_worker_event_fn_t handler, void *arg);

int bondp_worker_del_fd(int fd);

#ifdef __cplusplus
}
#endif

#endif // BONDP_WORKER_H
