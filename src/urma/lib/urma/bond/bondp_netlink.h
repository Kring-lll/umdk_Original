/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bond provider support netlink.
 */
#ifndef BONDP_NETLINK_H
#define BONDP_NETLINK_H

#define SEND_SWITCHBACK_REQ 0

/*
 * 逻辑内容 (对端可见):
 *   vjetty_id        - 发送端 vjetty ID, 对端用于匹配 bdp_tjetty->v_tjetty.id.id
 *   pjetty_i_idx     - 故障 pjetty 本端 idx, 对端用于定位 reimport 路径
 *   pjetty_i_new_id  - 新创建的 pjetty ID, 对端用于 reimport
 *   ctrl_type        - BONDP_FAILBACK_CTRL_REQ / RESP
 *   req_seq          - 序列号
 */
typedef struct bondp_failback_msg {
    struct {
        uint32_t vjetty_id;        // 发送端 vjetty ID (用于匹配 bdp_tjetty)
        uint32_t pjetty_i_idx;     // 发送端故障 pjetty idx (用于定位 reimport 路径)
        uint32_t pjetty_i_new_id;  // 发送端新 pjetty ID (REQ: 用于 reimport; RESP: 对端 pjetty ID)
        uint8_t ctrl_type;         // BONDP_FAILBACK_CTRL_REQ / RESP
        uint8_t req_seq;           // 序列号
        uint16_t reserved;
    } in;
} bondp_failback_msg_t;

int bondp_nl_init(void);
void bondp_nl_uninit(void);
int bondp_nl_get_fd(void);
int bondp_nl_send_failback_msg(const bondp_failback_msg_t *req);
int bondp_nl_recv_failback_msg(bondp_failback_msg_t *msg);

// 新接口: 直接发送 不指定？
int bondp_failback_ctrl_send(const bondp_failback_msg_t *req);
// 旧接口: 保留指定的方式
int bondp_fallback_ctrl_send_default(bondp_context_t *bdp_ctx, uint32_t vjetty_id,
    int local_idx, int target_idx, uint8_t ctrl_type, uint8_t req_seq, uint32_t payload);
#endif // BONDP_NETLINK_H
