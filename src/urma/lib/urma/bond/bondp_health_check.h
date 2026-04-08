/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bond provider health check declarations
 */
#ifndef BONDP_HEALTH_CHECK_H
#define BONDP_HEALTH_CHECK_H

#include "bondp_types.h"

typedef enum bondp_health_event {
	BONDP_HEALTH_EVENT_TA_TIMEOUT = 0,
	BONDP_HEALTH_EVENT_MAX
} bondp_health_event_t;

typedef struct bondp_health_event_info {
	int local_idx;
	int target_idx;
	uint64_t user_ctx;
	uint32_t cr_status;
	bool old_ok;
	bool new_ok;
} bondp_health_event_info_t;

void bondp_health_check_global_ctx_init(bondp_global_context_t *ctx);
void bondp_health_check_ctx_init(bondp_context_t *bond_ctx);

int bondp_start_health_check_thread(void);
void bondp_stop_health_check_thread(void);

int bondp_create_health_check_ctx(bondp_context_t *bond_ctx);
void bondp_destroy_health_check_ctx(bondp_context_t *bond_ctx);
void bondp_notify_health_event(bondp_context_t *bdp_ctx, bondp_health_event_t event,
	const bondp_health_event_info_t *info);

#endif // BONDP_HEALTH_CHECK_H
