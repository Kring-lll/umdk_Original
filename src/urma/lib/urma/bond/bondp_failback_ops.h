/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bond provider link recovery declarations
 */

#ifndef BONDP_LINK_RECOVERY_H
#define BONDP_LINK_RECOVERY_H

#include "bondp_failback.h"
#include "bondp_netlink.h"

int bondp_failback_rebuild_local_pjetty(bondp_failback_task_t *task);
int bondp_failback_send_req_to_node(bondp_failback_task_t *task, uint32_t node_idx);

// 对端处理 REQ: unimport 旧 ptjetty, 用 pjetty_i_new_id 重新 import, 发送 RESP
int bondp_failback_handle_req(const bondp_failback_msg_t *msg);

int bondp_failback_handle_node_notify(bondp_failback_task_t *task, uint32_t node_idx);
void bondp_failback_destroy_old_jetty(bondp_failback_task_t *task);

#endif // BONDP_LINK_RECOVERY_H
