/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2025. All rights reserved.
 * Description: netlink header file for urma_admin
 * Author: Yan Fangfang
 * Create: 2023-12-07
 * Note:
 * History: 2023-12-07   create file
 */

#ifndef ADMIN_NETLINK
#define ADMIN_NETLINK

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <netlink/msg.h>
#include "admin_cmd.h"

int admin_nl_send_msg(struct nl_msg *msg, genl_family_t family);
int admin_nl_recv_msg(int (*cb)(struct nl_msg *msg, void *arg), void *arg, genl_family_t family);
int admin_nl_send_recv_msg(struct nl_msg *msg, int (*cb)(struct nl_msg *msg, void *arg),
                           void *arg, genl_family_t family);
int admin_nl_send_recv_valid_msg(struct nl_msg *msg, int (*cb)(struct nl_msg *msg, void *arg),
                                 void *arg, genl_family_t family);
int admin_nl_send_recv_msg_default(struct nl_msg *msg, genl_family_t family);
int admin_nl_send_recv_msg_default_silent_errno(struct nl_msg *msg, int silent_errno, genl_family_t family);
int admin_nl_send_recv_msg_default_silent_notfound(struct nl_msg *msg, genl_family_t family);

struct nl_msg *admin_nl_alloc_msg(uint8_t cmd, int flags, genl_family_t family);
void admin_nl_free_msg(struct nl_msg *msg);

static inline int admin_nl_put_string(struct nl_msg *msg, int attr, const char *str)
{
    int ret = nla_put_string(msg, attr, str);
    if (ret != 0) {
        printf("Failed to put string attribute %d, ret: %d\n", attr, ret);
    }
    return ret;
}

static inline int admin_nl_put_u8(struct nl_msg *msg, int attr, uint8_t value)
{
    int ret = nla_put_u8(msg, attr, value);
    if (ret != 0) {
        printf("Failed to put u8 attribute %d, ret: %d\n", attr, ret);
    }
    return ret;
}

static inline int admin_nl_put_u16(struct nl_msg *msg, int attr, uint16_t value)
{
    int ret = nla_put_u16(msg, attr, value);
    if (ret != 0) {
        printf("Failed to put u16 attribute %d, ret: %d\n", attr, ret);
    }
    return ret;
}

static inline int admin_nl_put_u32(struct nl_msg *msg, int attr, uint32_t value)
{
    int ret = nla_put_u32(msg, attr, value);
    if (ret != 0) {
        printf("Failed to put u32 attribute %d, ret: %d\n", attr, ret);
    }
    return ret;
}

static inline int admin_nl_put_u64(struct nl_msg *msg, int attr, uint64_t value)
{
    int ret = nla_put_u64(msg, attr, value);
    if (ret != 0) {
        printf("Failed to put u64 attribute %d, ret: %d\n", attr, ret);
    }
    return ret;
}

static inline int admin_nl_put_binary(struct nl_msg *msg, int attr, const void *data, size_t len)
{
    int ret;

    if (len > INT_MAX) {
        printf("Binary attribute %d is too long, len: %zu\n", attr, len);
        return -EINVAL;
    }

    ret = nla_put(msg, attr, (int)len, data);
    if (ret != 0) {
        printf("Failed to put binary attribute %d, ret: %d\n", attr, ret);
    }
    return ret;
}

#endif
