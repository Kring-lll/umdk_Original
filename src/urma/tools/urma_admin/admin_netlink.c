/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2025. All rights reserved.
 * Description: netlink header file for urma_admin
 * Author: Yan Fangfang
 * Create: 2023-12-07
 * Note:
 * History: 2023-12-07   create file
 */

#include <errno.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/genl.h>
#include <netlink/netlink.h>
#include <netlink/socket.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "urma_cmd.h"
#include "urma_types.h"

#include "admin_cmd.h"

#include "admin_netlink.h"

static struct nl_sock *sock = NULL;
static int genl_id = 0;

static struct nl_sock *sock_ubagg = NULL;
static int genl_id_ubagg = 0;

typedef struct genl_info {
    genl_family_t genl_family;
    struct nl_sock **sock_ptr;
    int *genl_id_ptr;
    const char *family_name;
} genl_info_t;

static const genl_info_t genl_info_table[GENL_FAMILY_COUNT] = {
    { UBCORE_GENL, &sock,       &genl_id,       UBCORE_GENL_FAMILY_NAME },
    { UBAGG_GENL,  &sock_ubagg, &genl_id_ubagg, UBAGG_GENL_FAMILY_NAME  },
};

static struct nl_sock *get_nl_sock_by_family(genl_family_t family)
{
    if (family >= GENL_FAMILY_COUNT || family < 0) {
        printf("Invalid family for sock\n");
        return NULL;
    }
    printf("Get sock for genl_family=%d\n", family);
    return *genl_info_table[family].sock_ptr;
}

static int get_genl_id_by_family(genl_family_t family)
{
    if (family >= GENL_FAMILY_COUNT || family < 0) {
        printf("Invalid family for genl_id\n");
        return -1;
    }
    printf("Get genl_id for genl_family=%d\n", family);
    return *genl_info_table[family].genl_id_ptr;
}

static int init_sock_and_genl_id(void);

static int admin_nl_recv_msg_inner(int (*cb)(struct nl_msg *msg, void *arg),
    void *arg, int silent_errno, genl_family_t family)
{
    int ret;

    ret = init_sock_and_genl_id();
    if (ret != 0) {
        return ret;
    }

    struct nl_sock *target_sock = get_nl_sock_by_family(family);
    if (target_sock == NULL) {
        printf("Invalid sock\n");
        return -EINVAL;
    }

    ret = nl_socket_modify_cb(target_sock, NL_CB_MSG_IN, NL_CB_CUSTOM, cb, arg);
    if (ret < 0) {
        printf("Failed to set netlink callback, ret:%d\n", ret);
        return ret;
    }

    ret = nl_recvmsgs_default(target_sock);
    if (ret < 0) {
        if (!(silent_errno != 0 && ret == silent_errno)) {
            printf("Failed to recv netlink msg, ret:%d\n", ret);
        }
        return ret;
    }

    ret = nl_socket_modify_cb(target_sock, NL_CB_MSG_IN, NL_CB_CUSTOM, NULL, arg);
    if (ret < 0) {
        printf("Failed to reset netlink callback, ret:%d\n", ret);
        return ret;
    }

    return 0;
}

static int init_sock_and_genl_id(void)
{
    if (sock != NULL && sock_ubagg != NULL) {
        return 0;
    }

    int ret;
    struct nl_sock *new_socks[GENL_FAMILY_COUNT] = { NULL };
    int i;

    for (i = 0; i < GENL_FAMILY_COUNT; ++i) {
        const genl_info_t *info = &genl_info_table[i];
        if (*info->sock_ptr != NULL) {
            continue;
        }

        new_socks[i] = nl_socket_alloc();
        if (new_socks[i] == NULL) {
            printf("Failed to allocate netlink socket for %s\n", info->family_name);
            ret = -ENOMEM;
            goto cleanup;
        }

        ret = genl_ctrl_resolve(new_socks[i], info->family_name);
        if (ret < 0) {
            printf("Resolving of \"%s\" failed, ret:%d\n", info->family_name, ret);
            goto cleanup;
        }

        *info->genl_id_ptr = ret;
        *info->sock_ptr = new_socks[i];
    }
    return 0;

cleanup:
    for (i = GENL_FAMILY_COUNT - 1; i >= 0; --i) {
        if (new_socks[i] != NULL) {
            nl_close(new_socks[i]);
            nl_socket_free(new_socks[i]);
        }
    }
    return ret;
}

int admin_nl_send_msg(struct nl_msg *msg, genl_family_t family)
{
    int ret;

    ret = init_sock_and_genl_id();
    if (ret != 0) {
        return ret;
    }

    struct nl_sock *target_sock = get_nl_sock_by_family(family);
    if (target_sock == NULL) {
        printf("Invalid sock\n");
        return -EINVAL;
    }

    ret = nl_send_auto(target_sock, msg);
    if (ret < 0) {
        printf("Failed to send netlink msg, ret:%d\n", ret);
        return ret;
    }
    return 0;
}

int admin_nl_recv_msg(int (*cb)(struct nl_msg *msg, void *arg), void *arg, genl_family_t family)
{
    return admin_nl_recv_msg_inner(cb, arg, 0, family);
}

int admin_nl_send_recv_msg(struct nl_msg *msg, int (*cb)(struct nl_msg *msg, void *arg),
    void *arg, genl_family_t family)
{
    int ret;

    ret = admin_nl_send_msg(msg, family);
    if (ret != 0) {
        return ret;
    }

    ret = admin_nl_recv_msg(cb, arg, family);
    if (ret != 0) {
        return ret;
    }

    return 0;
}

static int nl_default_cb(struct nl_msg *msg, void *arg)
{
    (void)msg;
    (void)arg;
    return NL_OK;
}

int admin_nl_send_recv_msg_default(struct nl_msg *msg, genl_family_t family)
{
    return admin_nl_send_recv_msg(msg, nl_default_cb, NULL, family);
}

int admin_nl_send_recv_msg_default_silent_errno(struct nl_msg *msg,
    int silent_errno, genl_family_t family)
{
    int ret;

    ret = admin_nl_send_msg(msg, family);
    if (ret != 0) {
        return ret;
    }
    return admin_nl_recv_msg_inner(nl_default_cb, NULL, silent_errno, family);
}

int admin_nl_send_recv_msg_default_silent_notfound(struct nl_msg *msg, genl_family_t family)
{
    return admin_nl_send_recv_msg_default_silent_errno(msg, -NLE_OBJ_NOTFOUND, family);
}

struct nl_msg *admin_nl_alloc_msg(uint8_t cmd, int flags, genl_family_t family)
{
    int ret;

    ret = init_sock_and_genl_id();
    if (ret != 0) {
        return NULL;
    }

    struct nl_msg *msg = nlmsg_alloc();
    if (msg == NULL) {
        printf("Failed to allocate netlink message\n");
        return NULL;
    }

    int target_genl_id = get_genl_id_by_family(family);
    if (target_genl_id <= 0) {
        printf("Invalid genl_sock\n");
        return NULL;
    }

    void *msg_hdr = genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, target_genl_id, 0, flags, cmd, GENL_FAMILY_VERSION);
    if (msg_hdr == NULL) {
        printf("Failed to put genl header\n");
        nlmsg_free(msg);
        return NULL;
    }

    return msg;
};

void admin_nl_free_msg(struct nl_msg *msg)
{
    nlmsg_free(msg);
};
