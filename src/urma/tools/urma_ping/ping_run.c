/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: urma_ping run implementation file
 * Author: Wang Hang
 * Create: 2026-02-03
 * Note:
 * History: 2026-02-03 Create file
 */

#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "urma_api.h"
#include "urma_types.h"

#include "ping_log.h"
#include "ping_stat.h"

#include "ping_run.h"

const uint32_t PING_WK_JETTY_ID = 123;
const uint32_t PING_SEND_DEPTH = 1024;
const uint32_t PING_RECV_DEPTH = 1024;

typedef struct ping_urma_resource {
    urma_context_t *ctx;
    void *buf;
    urma_target_seg_t *seg;
    urma_jfc_t *send_jfc;
    urma_jfc_t *recv_jfc;
    urma_jfr_t *jfr;
    urma_jetty_t *jetty;
    urma_target_jetty_t *tjetty;
} ping_urma_resource_t;

static urma_device_t *find_first_urma_device()
{
    int num_devices = 0;
    urma_device_t **devices = urma_get_device_list(&num_devices);
    urma_device_t *device_selected = NULL;

    for (int i = 0; i < num_devices; i++) {
        if (devices[i]->type == URMA_TRANSPORT_UB &&
            strncmp(devices[i]->name, "bonding_dev", strlen("bonding_dev")) == 0) {
            device_selected = devices[i];
        }
    }

    urma_free_device_list(devices);
    return device_selected;
}

static void urma_log_func(int level, char *message)
{
    LOG_VVERBOSE("%s", message);
}

static int init_urma_resource(ping_cfg_t *cfg, ping_urma_resource_t *res)
{
    int ret = 0;
    if ((ret = urma_register_log_func(urma_log_func)) != URMA_SUCCESS) {
        LOG_ERROR("Failed to register urma log func, ret:%d\n", ret);
        return ret;
    }

    urma_init_attr_t conf = {0};
    if ((ret = urma_init(&conf)) != URMA_SUCCESS) {
        LOG_ERROR("Failed to init urma, ret:%d\n", ret);
        return ret;
    }

    urma_device_t *dev = find_first_urma_device();
    if (dev == NULL) {
        LOG_ERROR("Failed to find urma device.\n");
        ret = -ENODEV;
        goto uninit_urma;
    }

    urma_device_attr_t dev_attr = {0};
    if ((ret = urma_query_device(dev, &dev_attr)) != URMA_SUCCESS) {
        LOG_ERROR("Failed to query urma device attr, ret:%d\n", ret);
        goto uninit_urma;
    }
    if (cfg->size > dev_attr.dev_cap.max_msg_size) {
        LOG_ERROR("Ping message size %u exceeds device max msg size %u.\n",
                  cfg->size, dev_attr.dev_cap.max_msg_size);
        ret = -EINVAL;
        goto uninit_urma;
    }

    res->ctx = urma_create_context(dev, 0);
    if (res->ctx == NULL) {
        LOG_ERROR("Failed to create urma context.\n");
        ret = -EINVAL;
        goto uninit_urma;
    }

    res->buf = memalign(getpagesize(), cfg->size);
    if (res->buf == NULL) {
        LOG_ERROR("Failed to alloc memory for ping buffer.\n");
        ret = -ENOMEM;
        goto delete_context;
    }

    urma_seg_cfg_t seg_cfg = {
        .va = (uint64_t)(uintptr_t)res->buf,
        .len = cfg->size,
        .flag.bs.access = URMA_ACCESS_LOCAL_ONLY,
    };
    res->seg = urma_register_seg(res->ctx, &seg_cfg);
    if (res->seg == NULL) {
        LOG_ERROR("Failed to register urma segment.\n");
        ret = -EINVAL;
        goto free_buf;
    }

    urma_jfc_cfg_t send_jfc_cfg = {
        .depth = PING_SEND_DEPTH,
    };
    res->send_jfc = urma_create_jfc(res->ctx, &send_jfc_cfg);
    if (res->send_jfc == NULL) {
        LOG_ERROR("Failed to create urma jfc for send.\n");
        ret = -EINVAL;
        goto unregister_seg;
    }

    urma_jfc_cfg_t recv_jfc_cfg = {
        .depth = PING_RECV_DEPTH,
    };
    res->recv_jfc = urma_create_jfc(res->ctx, &recv_jfc_cfg);
    if (res->recv_jfc == NULL) {
        LOG_ERROR("Failed to create urma jfc for recv.\n");
        ret = -EINVAL;
        goto delete_send_jfc;
    }

    urma_jfr_cfg_t jfr_cfg = {
        .depth = PING_RECV_DEPTH,
        .trans_mode = URMA_TM_RM,
        .max_sge = 1,
        .jfc = res->recv_jfc,
    };
    res->jfr = urma_create_jfr(res->ctx, &jfr_cfg);
    if (res->jfr == NULL) {
        LOG_ERROR("Failed to create urma jfr.\n");
        ret = -EINVAL;
        goto delete_recv_jfc;
    }

    urma_jetty_cfg_t jetty_cfg = {
        .flag.bs.share_jfr = 1,
        .jfs_cfg = {
            .depth = PING_SEND_DEPTH,
            .trans_mode = URMA_TM_RM,
            .max_sge = 1,
            .rnr_retry = URMA_TYPICAL_RNR_RETRY,
            .err_timeout = URMA_TYPICAL_ERR_TIMEOUT,
            .jfc = res->send_jfc,
        },
        .shared.jfr = res->jfr,
    };
    res->jetty = urma_create_jetty(res->ctx, &jetty_cfg);
    if (res->jetty == NULL) {
        LOG_ERROR("Failed to create urma jetty.\n");
        ret = -EINVAL;
        goto delete_jfr;
    }

    urma_rjetty_t rjetty = {
        .jetty_id.eid = cfg->dest,
        .jetty_id.id = PING_WK_JETTY_ID,
        .trans_mode = URMA_TM_RM,
        .type = URMA_JETTY,
        .tp_type = URMA_CTP,
    };
    urma_token_t token_value = {0};
    res->tjetty = urma_import_jetty(res->ctx, &rjetty, &token_value);
    if (res->tjetty == NULL) {
        LOG_ERROR("Failed to import target urma jetty.\n");
        ret = -EINVAL;
        goto delete_jetty;
    }

    return 0;

delete_jetty:
    urma_delete_jetty(res->jetty);
delete_jfr:
    urma_delete_jfr(res->jfr);
delete_recv_jfc:
    urma_delete_jfc(res->recv_jfc);
delete_send_jfc:
    urma_delete_jfc(res->send_jfc);
unregister_seg:
    urma_unregister_seg(res->seg);
free_buf:
    free(res->buf);
delete_context:
    urma_delete_context(res->ctx);
uninit_urma:
    urma_uninit();
    return ret;
}

static void uninit_urma_resource(ping_urma_resource_t *res)
{
    urma_unimport_jetty(res->tjetty);
    urma_delete_jetty(res->jetty);
    urma_delete_jfr(res->jfr);
    urma_unregister_seg(res->seg);
    free(res->buf);
    urma_delete_context(res->ctx);
    urma_uninit();
}

typedef struct ping_per_seq_info {
    uint32_t seq;
    clock_t start_time;
    clock_t end_time;
    double rtt;
} ping_per_seq_info_t;

static int fill_recv_wr(ping_cfg_t *cfg, ping_urma_resource_t *res)
{
    urma_sge_t sge = {
        .addr = (uint64_t)(uintptr_t)res->buf,
        .len = cfg->size,
        .tseg = res->seg,
    };
    urma_jfr_wr_t wr = {
        .src = {
            .sge = &sge,
            .num_sge = 1,
        },
    };
    return urma_post_jetty_recv_wr(res->jetty, &wr, NULL);
}

static int send_ping_msg(ping_cfg_t *cfg, ping_urma_resource_t *res, ping_per_seq_info_t *seq_info)
{
    update_stat_on_send();
    seq_info->start_time = clock();

    urma_sge_t sge = {
        .addr = (uint64_t)(uintptr_t)res->buf,
        .len = cfg->size,
        .tseg = res->seg,
    };
    urma_jfs_wr_t wr = {
        .opcode = URMA_OPC_SEND_IMM,
        .flag.bs.complete_enable = 0,
        .tjetty = res->tjetty,
        .send = {
            .src = {
                .sge = &sge,
                .num_sge = 1,
            },
            .imm_data = seq_info->seq,
        },
    };
    int ret = urma_post_jetty_send_wr(res->jetty, &wr, NULL);
    if (ret < 0) {
        LOG_ERROR("Failed to post jetty send wr, ret:%d\n", ret);
        return ret;
    }
    return 0;
}

static int recv_ping_msg(ping_cfg_t *cfg, ping_urma_resource_t *res, ping_per_seq_info_t *seq_info)
{
    bool timed_out = false;
    urma_cr_t cr = {0};
    while (true) {
        int ret = urma_poll_jfc(res->recv_jfc, 1, &cr);

        seq_info->end_time = clock();
        seq_info->rtt = (double)(seq_info->end_time - seq_info->start_time) * 1000 / CLOCKS_PER_SEC;

        bool unmatched = (ret > 0 && cr.imm_data != seq_info->seq);
        bool received = (ret > 0 && cr.imm_data == seq_info->seq);
        bool timed_out = (seq_info->rtt >= cfg->timeout);

        if (unmatched) {
            LOG_VERBOSE("Unmatched reply received: expected seq=%d, got seq=%d\n",
                        seq_info->seq, cr.imm_data);
        }
        if (received || timed_out) {
            break;
        }
    }

    if (timed_out) {
        // timeout
        LOG_NORMAL("Request timeout for seq=%d\n", seq_info->seq);
    } else {
        int ret = fill_recv_wr(cfg, res);
        if (ret < 0) {
            LOG_ERROR("Failed to fill recv wr, ret:%d\n", ret);
            return ret;
        }

        if (cr.status == URMA_CR_SUCCESS) {
            // success
            update_stat_on_recv(seq_info->rtt);
            LOG_NORMAL("%d bytes from " EID_FMT ": seq=%d time=%.3f ms\n",
                       cfg->size, EID_ARGS(cfg->dest), seq_info->seq, seq_info->rtt);
        } else {
            // failure
            LOG_NORMAL("From " EID_FMT " seq=%d cr_status=%d\n",
                       EID_ARGS(cfg->dest), seq_info->seq, cr.status);
        }
    }

    return 0;
}

int start_ping(ping_cfg_t *cfg)
{
    verbose_set_level(cfg->verbose_level);

    int ret = 0;
    ping_urma_resource_t res = {0};

    if ((ret = init_urma_resource(cfg, &res)) != 0) {
        return ret;
    }

    if ((ret = fill_recv_wr(cfg, &res)) != 0) {
        LOG_ERROR("Failed to prepare recv wr, ret:%d\n", ret);
        return ret;
    }

    LOG_QUIET("URMA_PING " EID_FMT " %u bytes of data.\n", EID_ARGS(cfg->dest), cfg->size);
    LOG_VERBOSE("Destination : %s\n", cfg->dest);
    LOG_VERBOSE("Count       : %u\n", cfg->count);
    LOG_VERBOSE("Interval(s) : %u\n", cfg->interval);
    LOG_VERBOSE("Size(bytes) : %u\n", cfg->size);
    LOG_VERBOSE("Deadline(s) : %u\n", cfg->deadline);
    LOG_VERBOSE("Timeout(s)  : %u\n", cfg->timeout);

    init_stat();

    for (uint32_t i = 0; i < cfg->count; i++) {
        ping_per_seq_info_t seq_info = {
            .seq = i + 1,
        };

        ret = send_ping_msg(cfg, &res, &seq_info);
        if (ret != 0) {
            LOG_ERROR("Failed to send ping msg, ret:%d\n", ret);
            break;
        }

        ret = recv_ping_msg(cfg, &res, &seq_info);
        if (ret != 0) {
            LOG_ERROR("Failed to receive ping msg, ret:%d\n", ret);
            break;
        }

        if (i + 1 < cfg->count) {
            sleep(cfg->interval);
        }
    }

    if (false) {
        uninit_urma_resource(&res);
    }
    return ret;
}
