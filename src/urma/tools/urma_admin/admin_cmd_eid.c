/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: eid sub-command source file for urma_admin
 * Author: Wang Hang
 * Create: 2025-12-26
 * Note:
 * History: 2025-12-26   create file
 */

#include <stdio.h>

#include "admin_cmd.h"

static int cmd_eid_usage(admin_config_t *cfg)
{
    printf("Usage: urma_admin eid add [ DEV ] [ EID_IDX ] [ EID ] --ns [ NETNS ] --mode [ EID_MODE ]\n"
           "       urma_admin eid del [ DEV ] [ EID_IDX ]\n"
           "       urma_admin eid set [ DEV ] [ EID_IDX ] ns [ NETNS ]\n"
           "       urma_admin eid set [ DEV ] [ EID_IDX ] mode { static | dynamic }\n"
           "where  NETNS := /proc/$pid/ns/net\n");
    return 0;
}

static int cmd_eid_add(admin_config_t *cfg)
{
    int ret;
    if ((ret = pop_arg_dev(cfg)) != 0) {
        return ret;
    }
    if ((ret = pop_arg_eid_idx(cfg)) != 0) {
        return ret;
    }
    if ((ret = pop_arg_eid(cfg)) != 0) {
        return ret;
    }

    printf("TODO add eid %s %u\n", cfg->dev_name, cfg->idx);
    return 0;
}

static int cmd_eid_del(admin_config_t *cfg)
{
    int ret;
    if ((ret = pop_arg_dev(cfg)) != 0) {
        return ret;
    }
    if ((ret = pop_arg_eid_idx(cfg)) != 0) {
        return ret;
    }

    printf("TODO del eid %s %u\n", cfg->dev_name, cfg->idx);
    return 0;
}

static int cmd_eid_set_mode(admin_config_t *cfg)
{
    printf("TODO set eid mode %s %u\n", cfg->dev_name, cfg->idx);
    return 0;
}

static int cmd_eid_set_ns(admin_config_t *cfg)
{
    int ret;
    if ((ret = pop_arg_ns(cfg)) != 0) {
        return ret;
    }

    int ns_fd = admin_get_ns_fd(cfg->ns);
    if (ns_fd < 0) {
        (void)printf("Failed to get ns fd, ns %s.\n", cfg->ns);
        return ns_fd;
    }

    struct nl_msg *msg = admin_nl_alloc_msg(URMA_CORE_SET_DEV_EID_NS, 0);
    if (msg == NULL) {
        ret = -ENOMEM;
        goto close_ns_fd;
    }

    admin_core_cmd_update_eid_t arg = {0};
    (void)memcpy(arg.in.dev_name, cfg->dev_name, URMA_ADMIN_MAX_DEV_NAME);
    arg.in.eid_index = cfg->idx;
    arg.in.ns_fd = ns_fd;

    admin_nl_put_string(msg, UBCORE_ATTR_DEV_NAME, cfg->dev_name);
    admin_nl_put_u32(msg, UBCORE_ATTR_NS_FD, ns_fd);
    admin_nl_put_u32(msg, UBCORE_HDR_ARGS_LEN, sizeof(admin_core_cmd_update_eid_t));
    admin_nl_put_u64(msg, UBCORE_HDR_ARGS_ADDR, (uint64_t)&arg);
    ret = admin_nl_send_recv_msg_default(msg);
    admin_nl_free_msg(msg);

close_ns_fd:
    (void)close(ns_fd);
    return ret;
}

static int cmd_eid_set(admin_config_t *cfg)
{
    int ret;
    if ((ret = pop_arg_dev(cfg)) != 0) {
        return ret;
    }
    if ((ret = pop_arg_eid_idx(cfg)) != 0) {
        return ret;
    }

    static const admin_cmd_t cmds[] = {
        {NULL, cmd_eid_usage},      //
        {"mode", cmd_eid_set_mode}, //
        {"ns", cmd_eid_set_ns},     //
        {0},                        //
    };
    return exec_cmd(cfg, cmds);
}

int admin_cmd_eid(admin_config_t *cfg)
{
    static const admin_cmd_t cmds[] = {
        {NULL, cmd_eid_usage}, //
        {"add", cmd_eid_add},  //
        {"del", cmd_eid_del},  //
        {"set", cmd_eid_set},  //
        {0},                   //
    };
    return exec_cmd(cfg, cmds);
}
