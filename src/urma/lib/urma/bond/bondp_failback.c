/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bond provider failback netlink helpers.
 */

#include <netlink/genl/genl.h>
#include <netlink/msg.h>
#include <stdint.h>
#include <string.h>

#include "bondp_netlink.h"
#include "urma_log.h"
#include "urma_types.h"

#include "bondp_failback.h"

typedef urma_jetty_id_t bondp_failback_jetty_id_t;

typedef struct bondp_failback_task {
    uint32_t request_id;
    uint32_t peer_node_id;
    bondp_failback_jetty_id_t vjetty_id;
    uint32_t pjetty_idx;
    bondp_failback_jetty_id_t new_pjetty_id;
} bondp_failback_task_t;

typedef struct bondp_failback_result {
    uint32_t request_id;
    uint32_t peer_node_id;
    bondp_failback_jetty_id_t vjetty_id;
    uint32_t pjetty_idx;
    bondp_failback_jetty_id_t new_pjetty_id;
    int32_t result;
} bondp_failback_result_t;

static void bondp_failback_handle_notify(const bondp_failback_task_t *task)
{
    (void)task;
}

static void bondp_failback_handle_done(const bondp_failback_result_t *result)
{
    (void)result;
}

void bondp_failback_handle_notify_nl_msg(struct nlattr *attrs[])
{
    if (attrs[BONDP_NL_ATTR_PAYLOAD] == NULL) {
        URMA_LOG_WARN("Missing failback notify netlink payload.\n");
        return;
    }

    void *payload = nla_data(attrs[BONDP_NL_ATTR_PAYLOAD]);
    int payload_len = nla_len(attrs[BONDP_NL_ATTR_PAYLOAD]);
    if (payload == NULL || payload_len != (int)sizeof(bondp_failback_task_t)) {
        URMA_LOG_WARN("Invalid failback notify payload len=%d\n", payload_len);
        return;
    }

    bondp_failback_task_t task = {0};
    (void)memcpy(&task, payload, sizeof(task));
    bondp_failback_handle_notify(&task);
}

void bondp_failback_handle_done_nl_msg(struct nlattr *attrs[])
{
    if (attrs[BONDP_NL_ATTR_PAYLOAD] == NULL) {
        URMA_LOG_WARN("Missing failback done netlink payload.\n");
        return;
    }

    void *payload = nla_data(attrs[BONDP_NL_ATTR_PAYLOAD]);
    int payload_len = nla_len(attrs[BONDP_NL_ATTR_PAYLOAD]);
    if (payload == NULL || payload_len != (int)sizeof(bondp_failback_result_t)) {
        URMA_LOG_WARN("Invalid failback done payload len=%d\n", payload_len);
        return;
    }

    bondp_failback_result_t result = {0};
    (void)memcpy(&result, payload, sizeof(result));
    bondp_failback_handle_done(&result);
}
