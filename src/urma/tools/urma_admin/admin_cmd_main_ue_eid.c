/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: main_ue_eid sub-command source file for urma_admin
 * Author: Weijie Li
 * Create: 2026-05-30
 * Note:
 * History: 2026-05-30   create file
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netlink/attr.h>
#include <netlink/genl/genl.h>

#include "admin_cmd.h"
#include "admin_netlink.h"

#define ADMIN_MAIN_UE_EID_BATCH_MAX 128U

typedef struct admin_main_ue_eid_lookup_ctx {
    urma_eid_t main_ue_eid;
    bool found;
    int ret;
} admin_main_ue_eid_lookup_ctx_t;

static int admin_main_ue_eid_pop_eid(admin_config_t *cfg, const char *name,
                                     urma_eid_t *eid)
{
    char *arg = pop_arg(cfg);
    int ret;

    if (arg == NULL) {
        printf("No %s specified.\n", name);
        return -EINVAL;
    }

    ret = admin_str_to_eid(arg, eid);
    if (ret != 0) {
        printf("Invalid %s: %s.\n", name, arg);
        return ret;
    }
    return 0;
}

static int admin_main_ue_eid_put_eid(struct nl_msg *msg, int attr,
                                     const urma_eid_t *eid)
{
    return admin_nl_put_binary(msg, attr, eid, sizeof(*eid));
}

static int admin_main_ue_eid_send_eid_cmd(uint8_t cmd, const urma_eid_t *eid)
{
    struct nl_msg *msg = admin_nl_alloc_msg(cmd, 0);
    int ret;

    if (msg == NULL) {
        return -ENOMEM;
    }

    ret = admin_main_ue_eid_put_eid(msg, UBCORE_ATTR_EID, eid);
    if (ret == 0) {
        ret = admin_nl_send_recv_msg_default(msg);
    }

    admin_nl_free_msg(msg);
    return ret;
}

static int admin_main_ue_eid_insert(const urma_eid_t *eid,
                                    const urma_eid_t *main_ue_eid)
{
    struct nl_msg *msg = admin_nl_alloc_msg(URMA_CORE_ADMIN_INSERT_MAIN_UE_EID, 0);
    int ret;

    if (msg == NULL) {
        return -ENOMEM;
    }

    ret = admin_main_ue_eid_put_eid(msg, UBCORE_ATTR_EID, eid);
    if (ret == 0) {
        ret = admin_main_ue_eid_put_eid(msg, UBCORE_ATTR_MAIN_UE_EID,
                                        main_ue_eid);
    }
    if (ret == 0) {
        ret = admin_nl_send_recv_msg_default(msg);
    }

    admin_nl_free_msg(msg);
    return ret;
}

static int admin_main_ue_eid_lookup_cb(struct nl_msg *msg, void *arg)
{
    admin_main_ue_eid_lookup_ctx_t *ctx = arg;
    struct nlmsghdr *hdr = nlmsg_hdr(msg);
    struct genlmsghdr *genlhdr = genlmsg_hdr(hdr);
    struct nlattr *attrs[UBCORE_ATTR_AFTER_LAST] = {0};
    struct nlattr *attr;
    int ret;

    if (ctx == NULL) {
        return NL_OK;
    }

    ret = nla_parse(attrs, UBCORE_ATTR_AFTER_LAST - 1,
                    genlmsg_attrdata(genlhdr, 0),
                    genlmsg_attrlen(genlhdr, 0), NULL);
    if (ret != 0) {
        ctx->ret = ret;
        return NL_STOP;
    }

    attr = attrs[UBCORE_ATTR_MAIN_UE_EID];
    if (attr == NULL || nla_len(attr) != sizeof(ctx->main_ue_eid)) {
        ctx->ret = -EINVAL;
        return NL_STOP;
    }

    (void)memcpy(&ctx->main_ue_eid, nla_data(attr), sizeof(ctx->main_ue_eid));
    ctx->found = true;
    return NL_STOP;
}

static int admin_main_ue_eid_lookup(const urma_eid_t *eid,
                                    urma_eid_t *main_ue_eid)
{
    admin_main_ue_eid_lookup_ctx_t ctx = {0};
    struct nl_msg *msg = admin_nl_alloc_msg(URMA_CORE_ADMIN_LOOKUP_MAIN_UE_EID, 0);
    int ret;

    if (msg == NULL) {
        return -ENOMEM;
    }

    ret = admin_main_ue_eid_put_eid(msg, UBCORE_ATTR_EID, eid);
    if (ret == 0) {
        ret = admin_nl_send_recv_valid_msg(msg, admin_main_ue_eid_lookup_cb,
                                           &ctx);
    }

    admin_nl_free_msg(msg);
    if (ret != 0) {
        return ret;
    }
    if (ctx.ret != 0) {
        return ctx.ret;
    }
    if (!ctx.found) {
        return -ENOENT;
    }

    *main_ue_eid = ctx.main_ue_eid;
    return 0;
}

static int admin_main_ue_eid_flush(void)
{
    struct nl_msg *msg = admin_nl_alloc_msg(URMA_CORE_ADMIN_FLUSH_MAIN_UE_EID, 0);
    int ret;

    if (msg == NULL) {
        return -ENOMEM;
    }

    ret = admin_nl_send_recv_msg_default(msg);
    admin_nl_free_msg(msg);
    return ret;
}

static int admin_main_ue_eid_insert_batch(const urma_eid_t *main_ue_eid,
                                          const urma_eid_t *eids,
                                          uint32_t eid_num)
{
    struct nl_msg *msg;
    int ret;

    if (eid_num == 0 || eid_num > ADMIN_MAIN_UE_EID_BATCH_MAX) {
        return -EINVAL;
    }

    msg = admin_nl_alloc_msg(URMA_CORE_ADMIN_INSERT_MAIN_UE_EID_BATCH, 0);
    if (msg == NULL) {
        return -ENOMEM;
    }

    ret = admin_main_ue_eid_put_eid(msg, UBCORE_ATTR_MAIN_UE_EID, main_ue_eid);
    if (ret == 0) {
        ret = admin_nl_put_u32(msg, UBCORE_ATTR_EID_NUM, eid_num);
    }
    if (ret == 0) {
        ret = admin_nl_put_binary(msg, UBCORE_ATTR_EID_LIST, eids,
                                  (size_t)eid_num * sizeof(*eids));
    }
    if (ret == 0) {
        ret = admin_nl_send_recv_msg_default(msg);
    }

    admin_nl_free_msg(msg);
    return ret;
}

static int cmd_main_ue_eid_usage(admin_config_t *cfg)
{
    (void)cfg;
    printf("Usage:\n"
           "  urma_admin main_ue_eid insert <eid> <main_ue_eid>\n"
           "  urma_admin main_ue_eid insert_batch <main_ue_eid> <eid> [eid...]\n"
           "  urma_admin main_ue_eid delete <eid>\n"
           "  urma_admin main_ue_eid del <eid>\n"
           "  urma_admin main_ue_eid lookup <eid>\n"
           "  urma_admin main_ue_eid flush\n"
           "\n"
           "Options:\n"
           "  <eid>          EID key stored in the main UE EID table\n"
           "  <main_ue_eid>  Main UE EID mapped from one or more EIDs\n");
    return 0;
}

static int cmd_main_ue_eid_insert(admin_config_t *cfg)
{
    urma_eid_t eid = {0};
    urma_eid_t main_ue_eid = {0};
    int ret;

    ret = admin_main_ue_eid_pop_eid(cfg, "eid", &eid);
    if (ret != 0) {
        return ret;
    }
    ret = admin_main_ue_eid_pop_eid(cfg, "main_ue_eid", &main_ue_eid);
    if (ret != 0) {
        return ret;
    }

    ret = admin_main_ue_eid_insert(&eid, &main_ue_eid);
    if (ret != 0) {
        printf("Failed to insert main_ue_eid entry, ret:%d\n", ret);
    }
    return ret;
}

static int cmd_main_ue_eid_insert_batch(admin_config_t *cfg)
{
    urma_eid_t main_ue_eid = {0};
    urma_eid_t *eids;
    uint32_t eid_num;
    int ret;

    ret = admin_main_ue_eid_pop_eid(cfg, "main_ue_eid", &main_ue_eid);
    if (ret != 0) {
        return ret;
    }
    if (cfg->argc <= 0 || cfg->argc > ADMIN_MAIN_UE_EID_BATCH_MAX) {
        printf("Invalid eid num: %d.\n", cfg->argc);
        return -EINVAL;
    }

    eid_num = (uint32_t)cfg->argc;
    eids = calloc(eid_num, sizeof(*eids));
    if (eids == NULL) {
        return -ENOMEM;
    }

    for (uint32_t i = 0; i < eid_num; i++) {
        ret = admin_main_ue_eid_pop_eid(cfg, "eid", &eids[i]);
        if (ret != 0) {
            free(eids);
            return ret;
        }
    }

    ret = admin_main_ue_eid_insert_batch(&main_ue_eid, eids, eid_num);
    if (ret != 0) {
        printf("Failed to insert main_ue_eid batch, ret:%d\n", ret);
    }
    free(eids);
    return ret;
}

static int cmd_main_ue_eid_delete(admin_config_t *cfg)
{
    urma_eid_t eid = {0};
    int ret;

    ret = admin_main_ue_eid_pop_eid(cfg, "eid", &eid);
    if (ret != 0) {
        return ret;
    }

    ret = admin_main_ue_eid_send_eid_cmd(URMA_CORE_ADMIN_DELETE_MAIN_UE_EID,
                                         &eid);
    if (ret != 0) {
        printf("Failed to delete main_ue_eid entry, ret:%d\n", ret);
    }
    return ret;
}

static int cmd_main_ue_eid_lookup(admin_config_t *cfg)
{
    urma_eid_t main_ue_eid = {0};
    urma_eid_t eid = {0};
    int ret;

    ret = admin_main_ue_eid_pop_eid(cfg, "eid", &eid);
    if (ret != 0) {
        return ret;
    }

    ret = admin_main_ue_eid_lookup(&eid, &main_ue_eid);
    if (ret != 0) {
        printf("Failed to lookup main_ue_eid, ret:%d\n", ret);
        return ret;
    }

    printf(EID_FMT "\n", EID_ARGS(main_ue_eid));
    return 0;
}

static int cmd_main_ue_eid_flush(admin_config_t *cfg)
{
    int ret;

    (void)cfg;
    ret = admin_main_ue_eid_flush();
    if (ret != 0) {
        printf("Failed to flush main_ue_eid table, ret:%d\n", ret);
    }
    return ret;
}

int admin_cmd_main_ue_eid(admin_config_t *cfg)
{
    if (cfg->help) {
        return cmd_main_ue_eid_usage(cfg);
    }

    static const admin_cmd_t cmds[] = {
        {NULL, cmd_main_ue_eid_usage},
        {"insert", cmd_main_ue_eid_insert},
        {"insert_batch", cmd_main_ue_eid_insert_batch},
        {"delete", cmd_main_ue_eid_delete},
        {"del", cmd_main_ue_eid_delete},
        {"lookup", cmd_main_ue_eid_lookup},
        {"flush", cmd_main_ue_eid_flush},
        {0},
    };
    return exec_cmd(cfg, cmds);
}
