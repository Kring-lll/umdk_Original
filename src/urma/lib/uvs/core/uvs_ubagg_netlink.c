/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: uvs ubagg netlink source file
 */
#include "uvs_ubagg_netlink.h"

#include <errno.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/genl.h>
#include <netlink/msg.h>
#include <netlink/netlink.h>
#include <netlink/socket.h>
#include <stdlib.h>
#include <string.h>

#include "tpsa_log.h"

static void uvs_ubagg_nl_close_sock(struct nl_sock *sock)
{
    if (sock == NULL) {
        return;
    }

    nl_close(sock);
    nl_socket_free(sock);
}

static int uvs_ubagg_nl_open_sock(struct nl_sock **sock_out, int *genl_id)
{
    struct nl_sock *sock = NULL;
    int ret;

    if (sock_out == NULL || genl_id == NULL) {
        return -EINVAL;
    }

    sock = nl_socket_alloc();
    if (sock == NULL) {
        TPSA_LOG_ERR("Failed to allocate ubagg netlink socket.\n");
        return -ENOMEM;
    }

    ret = genl_connect(sock);
    if (ret < 0) {
        TPSA_LOG_ERR("Failed to connect ubagg netlink, ret=%d.\n", ret);
        uvs_ubagg_nl_close_sock(sock);
        return ret;
    }

    ret = genl_ctrl_resolve(sock, UVS_UBAGG_GENL_FAMILY_NAME);
    if (ret < 0) {
        TPSA_LOG_ERR("Failed to resolve ubagg netlink family, ret=%d.\n", ret);
        uvs_ubagg_nl_close_sock(sock);
        return ret;
    }

    *sock_out = sock;
    *genl_id = ret;
    return 0;
}

static struct nl_msg *uvs_ubagg_nl_alloc_msg(int genl_id)
{
    struct nl_msg *msg = nlmsg_alloc();

    if (msg == NULL) {
        TPSA_LOG_ERR("Failed to allocate ubagg netlink msg.\n");
        return NULL;
    }

    if (genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, genl_id, 0, 0,
                    UVS_UBAGG_NL_CMD_GET_TOPO,
                    UVS_UBAGG_GENL_FAMILY_VERSION) == NULL) {
        nlmsg_free(msg);
        TPSA_LOG_ERR("Failed to build ubagg topo netlink msg.\n");
        return NULL;
    }

    return msg;
}

static int uvs_ubagg_nl_get_topo_node(struct nl_sock *sock, int genl_id,
    uint32_t node_idx, uvs_ubagg_nl_topo_info_t *arg)
{
    struct nl_msg *msg = NULL;
    int ret;

    if (sock == NULL || arg == NULL || node_idx > (uint32_t)INT32_MAX) {
        return -EINVAL;
    }

    (void)memset(arg, 0, sizeof(*arg));
    arg->in.node_idx = (int)node_idx;

    msg = uvs_ubagg_nl_alloc_msg(genl_id);
    if (msg == NULL) {
        return -ENOMEM;
    }

    ret = nla_put_u64(msg, UVS_UBAGG_NL_HDR_ARGS_ADDR,
                      (uint64_t)(uintptr_t)arg);
    if (ret != 0) {
        TPSA_LOG_ERR("Failed to put ubagg topo args attr, ret=%d.\n", ret);
        nlmsg_free(msg);
        return ret;
    }

    ret = nl_send_auto(sock, msg);
    nlmsg_free(msg);
    if (ret < 0) {
        TPSA_LOG_ERR("Failed to send ubagg topo netlink msg, ret=%d.\n", ret);
        return ret;
    }

    ret = nl_recvmsgs_default(sock);
    if (ret != 0) {
        TPSA_LOG_ERR("Failed to recv ubagg topo netlink msg, ret=%d.\n", ret);
        return ret;
    }

    return 0;
}

static int uvs_ubagg_nl_topo_num_valid(uint32_t topo_num)
{
    if (topo_num == 0 || topo_num > MAX_NODE_NUM) {
        TPSA_LOG_ERR("Invalid topo num from ubagg: %u.\n", topo_num);
        return -EINVAL;
    }
    if (topo_num > SIZE_MAX / sizeof(struct urma_topo_node)) {
        TPSA_LOG_ERR("Topo size overflow, topo_num=%u.\n", topo_num);
        return -EOVERFLOW;
    }

    return 0;
}

static int uvs_ubagg_nl_alloc_topo_info(const uvs_ubagg_nl_topo_info_t *arg,
    struct urma_topo_node **topo, uint32_t *topo_num)
{
    uint32_t node_num;
    int ret;

    if (arg == NULL || topo == NULL || topo_num == NULL) {
        return -EINVAL;
    }

    node_num = arg->out.node_num;
    ret = uvs_ubagg_nl_topo_num_valid(node_num);
    if (ret != 0) {
        return ret;
    }

    *topo = calloc(node_num, sizeof(struct urma_topo_node));
    if (*topo == NULL) {
        return -ENOMEM;
    }

    (*topo)[0] = arg->out.topo_info;
    *topo_num = node_num;
    return 0;
}

static int uvs_ubagg_nl_fill_topo_info(struct nl_sock *sock, int genl_id,
    struct urma_topo_node *topo, uint32_t topo_num,
    uvs_ubagg_nl_topo_info_t *arg)
{
    uint32_t node_idx;
    int ret;

    if (topo == NULL || arg == NULL) {
        return -EINVAL;
    }

    for (node_idx = 1; node_idx < topo_num; node_idx++) {
        ret = uvs_ubagg_nl_get_topo_node(sock, genl_id, node_idx, arg);
        if (ret != 0) {
            return ret;
        }
        if (arg->out.node_num != topo_num) {
            TPSA_LOG_ERR("Topo num changed, old=%u, new=%u.\n", topo_num,
                         arg->out.node_num);
            return -EAGAIN;
        }
        topo[node_idx] = arg->out.topo_info;
    }

    return 0;
}

int uvs_ubagg_nl_get_topo_info(struct urma_topo_node **topo, uint32_t *topo_num)
{
    uvs_ubagg_nl_topo_info_t *arg = NULL;
    struct nl_sock *sock = NULL;
    int genl_id = 0;
    int ret;

    if (topo == NULL || topo_num == NULL) {
        return -EINVAL;
    }
    *topo = NULL;
    *topo_num = 0;

    arg = calloc(1, sizeof(*arg));
    if (arg == NULL) {
        return -ENOMEM;
    }

    ret = uvs_ubagg_nl_open_sock(&sock, &genl_id);
    if (ret != 0) {
        free(arg);
        return ret;
    }

    ret = uvs_ubagg_nl_get_topo_node(sock, genl_id, 0, arg);
    if (ret != 0) {
        uvs_ubagg_nl_close_sock(sock);
        free(arg);
        return ret;
    }

    ret = uvs_ubagg_nl_alloc_topo_info(arg, topo, topo_num);
    if (ret != 0) {
        uvs_ubagg_nl_close_sock(sock);
        free(arg);
        return ret;
    }

    ret = uvs_ubagg_nl_fill_topo_info(sock, genl_id, *topo, *topo_num, arg);
    if (ret != 0) {
        free(*topo);
        *topo = NULL;
        *topo_num = 0;
    }

    uvs_ubagg_nl_close_sock(sock);
    free(arg);
    return ret;
}
