/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bond provider link recovery declarations
 */

#ifndef BONDP_LINK_RECOVERY_H
#define BONDP_LINK_RECOVERY_H

#include "bondp_types.h"

static int bondp_rebuild_local_pjetty(bondp_failover_ctx_t *task);
static int bondp_send_failover_req(bondp_failover_ctx_t *task);

#endif // BONDP_LINK_RECOVERY_H
