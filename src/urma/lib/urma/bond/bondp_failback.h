/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bond provider failback netlink helpers.
 */
#ifndef BONDP_FAILBACK_H
#define BONDP_FAILBACK_H

#include <netlink/msg.h>

#include "bondp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void bondp_fb_handle_notify_nl_msg(struct nlattr *attrs[]);
void bondp_fb_handle_done_nl_msg(struct nlattr *attrs[]);

int bondp_fb_add_task(bondp_context_t *bond_ctx, uint32_t vjetty_id, uint32_t pjetty_idx);

/* Schedule async reclamation of a detached, failed backup pjetty: removes its id mapping
 * and deletes the jetty off the datapath path. Safe to call with send_lock held. */
int bondp_fb_schedule_delete_pjetty(bondp_context_t *bond_ctx, urma_jetty_t *dead_jetty);

/* Schedule async recreation of a backup pjetty on a slot whose backup was detached (e.g. after a
 * double fault), restoring the hot-standby capability. Safe to call with send_lock held. */
int bondp_fb_schedule_rebuild_backup(bondp_context_t *bond_ctx, uint32_t vjetty_id, uint32_t pjetty_idx);

int bondp_fb_init(bondp_context_t *bond_ctx);
void bondp_fb_uninit(bondp_context_t *bond_ctx);

#ifdef __cplusplus
}
#endif

#endif // BONDP_FAILBACK_H
