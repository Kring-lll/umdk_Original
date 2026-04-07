/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bond provider health check implementation
 */

#include <errno.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/eventfd.h>

#include "urma_log.h"
#include "urma_api.h"
#include "bondp_health_check.h"
#include "bondp_context_table.h"

#define UBAGG_MAX_EVENT 1
#define BONDP_HEALTH_CHECK_ENV "BondHealthCheck"
#define BONDP_HEALTH_CHECK_BUF_LEN (4096)
#define BONDP_HEALTH_CHECK_4K_ALIGN (4096)
#define BONDP_HEALTH_CHECK_EPOLL_TIMEOUT_MS (100)
#define BONDP_HEALTH_CHECK_INTERVAL_US (20000000ULL)
#define BONDP_HEALTH_CHECK_MAGIC (0x48434b0000000000ULL)
#define BONDP_HEALTH_CHECK_MAGIC_MASK (0xffff000000000000ULL)
#define BONDP_HEALTH_CHECK_DEV_IDX_BITS (12)
#define BONDP_HEALTH_CHECK_DEV_IDX_MASK ((1ULL << BONDP_HEALTH_CHECK_DEV_IDX_BITS) - 1)
#define BONDP_HEALTH_CHECK_TARGET_SHIFT (0)
#define BONDP_HEALTH_CHECK_LOCAL_SHIFT (BONDP_HEALTH_CHECK_DEV_IDX_BITS)

typedef struct bondp_health_ctx_node {
    bondp_context_t *bdp_ctx;
    struct bondp_health_ctx_node *next;
} bondp_health_ctx_node_t;

static pthread_rwlock_t g_health_ctx_lock = PTHREAD_RWLOCK_INITIALIZER;
static bondp_health_ctx_node_t *g_health_ctx_head = NULL;

static uint64_t bondp_build_health_user_ctx(uint32_t local_idx, uint32_t target_idx)
{
    return BONDP_HEALTH_CHECK_MAGIC |
        ((uint64_t)(local_idx & BONDP_HEALTH_CHECK_DEV_IDX_MASK) << BONDP_HEALTH_CHECK_LOCAL_SHIFT) |
        ((uint64_t)(target_idx & BONDP_HEALTH_CHECK_DEV_IDX_MASK) << BONDP_HEALTH_CHECK_TARGET_SHIFT);
}

static void bondp_parse_health_user_ctx(uint64_t user_ctx, uint32_t *local_idx, uint32_t *target_idx)
{
    *local_idx = (uint32_t)((user_ctx >> BONDP_HEALTH_CHECK_LOCAL_SHIFT) & BONDP_HEALTH_CHECK_DEV_IDX_MASK);
    *target_idx = (uint32_t)((user_ctx >> BONDP_HEALTH_CHECK_TARGET_SHIFT) & BONDP_HEALTH_CHECK_DEV_IDX_MASK);
}

static uint64_t bondp_get_monotonic_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static int bondp_send_health_probe(bondp_context_t *bdp_ctx, bondp_target_jetty_t *bdp_tjetty,
    bondp_health_sub_task_t *sub_task)
{
    urma_target_jetty_t *p_tjetty = bdp_tjetty->p_tjetty[sub_task->local_idx][sub_task->target_idx];
    urma_target_seg_t *p_remote_tseg = bdp_tjetty->p_check_tseg[sub_task->local_idx][sub_task->target_idx];
    urma_target_seg_t *p_local_tseg = bdp_ctx->bondp_heath_check_ctx.check_tseg[sub_task->local_idx];
    if (p_tjetty == NULL || p_remote_tseg == NULL || p_local_tseg == NULL) {
        URMA_LOG_WARN("Health probe precheck failed, lidx:%d tidx:%d tjetty:%p remote_tseg:%p local_tseg:%p\n",
            sub_task->local_idx, sub_task->target_idx, (void *)p_tjetty, (void *)p_remote_tseg, (void *)p_local_tseg);
        return -1;
    }

    urma_jetty_t *p_sjetty = NULL;
    pthread_rwlock_rdlock(&bdp_ctx->p_vjetty_id_table.lock);
    bdp_p_vjetty_id_t *item = NULL;
    HMAP_FOR_EACH(item, hmap_node, &bdp_ctx->p_vjetty_id_table.hmap) {
        if (item->key.type != JETTY || item->comp == NULL) {
            continue;
        }
        if (item->comp->p_jetty[sub_task->local_idx] != NULL) {
            p_sjetty = item->comp->p_jetty[sub_task->local_idx];
            break;
        }
    }
    pthread_rwlock_unlock(&bdp_ctx->p_vjetty_id_table.lock);
    if (p_sjetty == NULL) {
        URMA_LOG_WARN("Health probe failed to find sjetty, lidx:%d tidx:%d\n",
            sub_task->local_idx, sub_task->target_idx);
        return -1;
    }

    uint64_t *probe = (uint64_t *)bdp_ctx->bondp_heath_check_ctx.check_buf;
    probe[sub_task->local_idx] += 1;

    urma_sge_t src_sge = {
        .addr = (uint64_t)&probe[sub_task->local_idx],
        .len = sizeof(uint64_t),
        .tseg = p_local_tseg,
    };
    urma_sge_t dst_sge = {
        .addr = p_remote_tseg->seg.ubva.va,
        .len = sizeof(uint64_t),
        .tseg = p_remote_tseg,
    };
    urma_sg_t src = {
        .sge = &src_sge,
        .num_sge = 1,
    };
    urma_sg_t dst = {
        .sge = &dst_sge,
        .num_sge = 1,
    };
    urma_jfs_wr_t wr = {
        .opcode = URMA_OPC_WRITE,
        .flag.bs.complete_enable = 1,
        .flag.bs.inline_flag = 0,
        .tjetty = p_tjetty,
        .user_ctx = sub_task->user_ctx,
        .rw = {
            .src = src,
            .dst = dst,
        },
        .next = NULL,
    };

    urma_jfs_wr_t *bad_wr = NULL;
    return urma_post_jetty_send_wr(p_sjetty, &wr, &bad_wr);
}

static void bondp_free_health_task(bondp_health_task_t *task)
{
    if (task == NULL) {
        return;
    }
    free(task);
}

static void bondp_register_health_ctx_global(bondp_context_t *bond_ctx)
{
    pthread_rwlock_wrlock(&g_health_ctx_lock);
    for (bondp_health_ctx_node_t *node = g_health_ctx_head; node != NULL; node = node->next) {
        if (node->bdp_ctx == bond_ctx) {
            pthread_rwlock_unlock(&g_health_ctx_lock);
            return;
        }
    }
    bondp_health_ctx_node_t *node = calloc(1, sizeof(bondp_health_ctx_node_t));
    if (node != NULL) {
        node->bdp_ctx = bond_ctx;
        node->next = g_health_ctx_head;
        g_health_ctx_head = node;
    }
    pthread_rwlock_unlock(&g_health_ctx_lock);
}

static void bondp_unregister_health_ctx_global(bondp_context_t *bond_ctx)
{
    pthread_rwlock_wrlock(&g_health_ctx_lock);
    bondp_health_ctx_node_t **pp = &g_health_ctx_head;
    while (*pp != NULL) {
        if ((*pp)->bdp_ctx == bond_ctx) {
            bondp_health_ctx_node_t *to_free = *pp;
            *pp = (*pp)->next;
            free(to_free);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_rwlock_unlock(&g_health_ctx_lock);
}

static bool bondp_read_health_check_enable(void)
{
    const char *value = getenv(BONDP_HEALTH_CHECK_ENV);
    return (value != NULL && strcmp(value, "true") == 0);
}

bool bondp_health_check_enabled(void)
{
    return g_bondp_global_ctx->health_thread_ctx.health_check_enable;
}

void bondp_health_check_global_ctx_init(bondp_global_context_t *ctx)
{
    ctx->health_thread_ctx.health_epoll_fd = -1;
    ctx->health_thread_ctx.health_check_enable = bondp_read_health_check_enable();
    atomic_init(&ctx->health_thread_ctx.health_thread_stop, false);
}

void bondp_health_check_ctx_init(bondp_context_t *bond_ctx)
{
    bond_ctx->bondp_heath_check_ctx.check_buf_len = BONDP_HEALTH_CHECK_BUF_LEN;
    bond_ctx->bondp_heath_check_ctx.health_check_fd = -1;
}

static void bondp_unregister_health_check_seg(bondp_context_t *bond_ctx)
{
    bondp_heath_check_ctx_t *health = &bond_ctx->bondp_heath_check_ctx;

    for (int i = 0; i < URMA_UBAGG_DEV_MAX_NUM; ++i) {
        if (health->check_tseg[i] == NULL) {
            continue;
        }

        if (urma_unregister_seg(health->check_tseg[i]) != URMA_SUCCESS) {
            URMA_LOG_ERR("Failed to unregister health check segment %d\n", i);
        }
        health->check_tseg[i] = NULL;
    }

    free(health->check_buf);
    health->check_buf = NULL;
}

static int bondp_register_health_check_seg(bondp_context_t *bond_ctx)
{
    bondp_heath_check_ctx_t *health = &bond_ctx->bondp_heath_check_ctx;
    urma_seg_cfg_t seg_cfg = {
        .va = 0,
        .len = health->check_buf_len,
        .token_id = NULL,
        .token_value = {0},
        .flag = {
            /* only used for health check cnt, using plaintext tokens, no security risk. */
            .bs.token_policy = URMA_TOKEN_PLAIN_TEXT,
            .bs.cacheable = URMA_NON_CACHEABLE,
            .bs.access = URMA_ACCESS_WRITE | URMA_ACCESS_READ,
            .bs.reserved = 0,
        },
        .user_ctx = 0,
        .iova = 0,
    };

    health->check_buf = memalign(BONDP_HEALTH_CHECK_4K_ALIGN, health->check_buf_len);
    if (health->check_buf == NULL) {
        URMA_LOG_ERR("Failed to alloc health check buffer\n");
        return -1;
    }

    seg_cfg.va = (uint64_t)health->check_buf;

    for (int i = 0; i < bond_ctx->dev_num; ++i) {
        if (bond_ctx->p_ctxs[i] == NULL) {
            continue;
        }
        health->check_tseg[i] = urma_register_seg(bond_ctx->p_ctxs[i], &seg_cfg);
        if (health->check_tseg[i] == NULL) {
            URMA_LOG_ERR("Failed to register health check segment %d\n", i);
            bondp_unregister_health_check_seg(bond_ctx);
            return -1;
        }

        URMA_LOG_INFO("Succeed to register health check segment %d, len: %lu\n", i, health->check_buf_len);
    }
    return 0;
}

int bondp_fill_vjetty_health_info(bondp_context_t *bond_ctx, bondp_comp_t *bdp_jetty,
    urma_bond_seg_info_out_t *health_check_seg, bool *is_health_check_enable)
{
    *is_health_check_enable = bondp_health_check_enabled();
    if (!(*is_health_check_enable)) {
        return 0;
    }

    health_check_seg->dev_num = bond_ctx->dev_num;

    bondp_heath_check_ctx_t *health_ctx = &bond_ctx->bondp_heath_check_ctx;
    for (int i = 0; i < bond_ctx->dev_num; ++i) {
        if (bdp_jetty->p_jetty[i] == NULL || health_ctx->check_tseg[i] == NULL) {
            continue;
        }
        health_check_seg->slaves[i] = health_ctx->check_tseg[i]->seg;
    }

    URMA_LOG_INFO("Succeed to fill health check seg info to kernel, dev_num: %d\n", bond_ctx->dev_num);
    return 0;
}

static int import_check_tseg_for_primary_eid(
    bondp_context_t *bdp_ctx, bondp_target_jetty_t *bdp_tjetty,
    urma_bond_id_info_out_t *rvjetty_info)
{
    urma_import_seg_flag_t flag = {
        .bs.cacheable = URMA_NON_CACHEABLE,
        .bs.mapping = URMA_SEG_NOMAP,
        .bs.reserved = 0,
        .bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE,
    };

    for (int i = 0; i < bdp_tjetty->local_dev_num; ++i) {
        if (bdp_ctx->p_ctxs[i] == NULL || bdp_tjetty->p_tjetty[i][i] == NULL) {
            URMA_LOG_ERR("Primary dev is NULL or p_tjetty is NULL (%d, %d)\n", i, i);
            return -1;
        }
        if (i >= rvjetty_info->health_check_seg.dev_num) {
            URMA_LOG_ERR("local dev num %d exceeds health check seg dev num %d\n", i,
                rvjetty_info->health_check_seg.dev_num);
            return -1;
        }

        urma_seg_t *check_seg = &rvjetty_info->health_check_seg.slaves[i];
        urma_token_t token = {
            .token = check_seg->token_id,
        };

        bdp_tjetty->p_check_tseg[i][i] = urma_import_seg(bdp_ctx->p_ctxs[i], check_seg, &token, 0, flag);
        if (bdp_tjetty->p_check_tseg[i][i] == NULL) {
            URMA_LOG_ERR("Failed to import health check seg (%d, %d)\n", i, i);
            return -1;
        }

        URMA_LOG_INFO("Import health check seg [%d]("EID_FMT")<-[%d]("EID_FMT")\n", i,
            EID_ARGS(bdp_ctx->p_ctxs[i]->eid), i, EID_ARGS(rvjetty_info->health_check_seg.slaves[i].ubva.eid));
    }

    URMA_LOG_INFO("Succeed to import health check segs\n");
    return 0;
}

static int import_check_tseg_for_port_eid(
    bondp_context_t *bdp_ctx, bondp_target_jetty_t *bdp_tjetty,
    urma_bond_id_info_out_t *rvjetty_info)
{
    bool has_valid_route = false;
    urma_import_seg_flag_t flag = {
        .bs.cacheable = URMA_NON_CACHEABLE,
        .bs.mapping = URMA_SEG_NOMAP,
        .bs.reserved = 0,
        .bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE,
    };

    for (int i = 0; i < IODIE_NUM; i++) {
        for (int j = 0; j < PORT_NUM; j++) {
            int local_port = IODIE_NUM + PORT_NUM * i + j;
            int target_port = IODIE_NUM + PORT_NUM * i + rvjetty_info->ports[i][j];

            if (local_port >= bdp_tjetty->local_dev_num ||
                target_port >= bdp_tjetty->target_dev_num ||
                local_port >= bdp_ctx->dev_num ||
                target_port >= rvjetty_info->health_check_seg.dev_num ||
                bdp_ctx->p_ctxs[local_port] == NULL ||
                bdp_tjetty->p_tjetty[local_port][target_port] == NULL) {
                URMA_LOG_DEBUG("BONDP skip check seg route (%d %d)\n", local_port, target_port);
                continue;
            }

            urma_seg_t *check_seg = &rvjetty_info->health_check_seg.slaves[target_port];
            urma_token_t token = {
                .token = check_seg->token_id,
            };

            bdp_tjetty->p_check_tseg[local_port][target_port] =
                urma_import_seg(bdp_ctx->p_ctxs[local_port], check_seg, &token, 0, flag);
            if (bdp_tjetty->p_check_tseg[local_port][target_port] == NULL) {
                URMA_LOG_ERR("Failed to import health check seg (%d, %d)\n", local_port, target_port);
                return -1;
            }

            URMA_LOG_INFO("Import health check seg [%d]("EID_FMT")<-[%d]("EID_FMT")\n", local_port,
                EID_ARGS(bdp_ctx->p_ctxs[local_port]->eid), target_port, EID_ARGS(rvjetty_info->health_check_seg.slaves[target_port].ubva.eid));
            has_valid_route = true;
        }
    }

    if (!has_valid_route) {
        URMA_LOG_ERR("No valid direct route for health check seg\n");
        return -1;
    }

    URMA_LOG_INFO("Succeed to import port health check segs\n");
    return 0;
}

int bondp_unimport_health_check_tseg(bondp_target_jetty_t *bdp_tjetty)
{
    int ret = URMA_SUCCESS;

    for (int i = 0; i < bdp_tjetty->local_dev_num; ++i) {
        for (int j = 0; j < bdp_tjetty->target_dev_num; ++j) {
            if (bdp_tjetty->p_check_tseg[i][j] == NULL) {
                continue;
            }

            if (urma_unimport_seg(bdp_tjetty->p_check_tseg[i][j]) != URMA_SUCCESS) {
                URMA_LOG_ERR("Failed to unimport health check seg (%d, %d)\n", i, j);
                ret = URMA_FAIL;
            }
            bdp_tjetty->p_check_tseg[i][j] = NULL;
        }
    }

    URMA_LOG_INFO("Finish to unimport health check segs, ret: %d\n", ret);
    return ret;
}

int bondp_import_health_check_tseg(bondp_context_t *bdp_ctx, bondp_target_jetty_t *bdp_tjetty,
    urma_bond_id_info_out_t *rvjetty_info)
{
    if (!rvjetty_info->is_health_check_enable || !bondp_health_check_enabled()) {
        return 0;
    }

    if (bdp_tjetty->is_multipath) {
        return import_check_tseg_for_primary_eid(bdp_ctx, bdp_tjetty, rvjetty_info);
    }

    return import_check_tseg_for_port_eid(bdp_ctx, bdp_tjetty, rvjetty_info);
}

static void *bondp_health_check_thread(void *arg)
{
    bondp_global_context_t *global_ctx = (bondp_global_context_t *)arg;
    struct epoll_event events[UBAGG_MAX_EVENT];

    if (prctl(PR_SET_NAME, "bond_health_t", 0, 0, 0) != 0) {
        URMA_LOG_WARN("Failed to set health thread name, errno: %d\n", errno);
    }

    int epoll_fd = global_ctx->health_thread_ctx.health_epoll_fd;

    while (true) {
        bool stop = atomic_load(&global_ctx->health_thread_ctx.health_thread_stop);
        if (stop) {
            break;
        }

        if (epoll_wait(epoll_fd, events, UBAGG_MAX_EVENT, BONDP_HEALTH_CHECK_EPOLL_TIMEOUT_MS) < 0 &&
            errno != EINTR) {
            URMA_LOG_ERR("Health check epoll_wait failed, errno: %d\n", errno);
            (void)usleep(BONDP_HEALTH_CHECK_EPOLL_TIMEOUT_MS * 1000);
            continue;
        }

        uint64_t now_us = bondp_get_monotonic_us();
        pthread_rwlock_rdlock(&g_health_ctx_lock);
        for (bondp_health_ctx_node_t *ctx_node = g_health_ctx_head; ctx_node != NULL; ctx_node = ctx_node->next) {
            bondp_heath_check_ctx_t *health = &ctx_node->bdp_ctx->bondp_heath_check_ctx;
            pthread_rwlock_wrlock(&health->task_lock);
            for (bondp_health_task_t *task = health->task_list; task != NULL; task = task->next) {
                if (now_us < task->next_probe_ts_us) {
                    continue;
                }
                for (int i = 0; i < URMA_UBAGG_DEV_MAX_NUM; ++i) {
                    for (int j = 0; j < URMA_UBAGG_DEV_MAX_NUM; ++j) {
                        bondp_health_sub_task_t *sub = &task->sub_tasks[i][j];
                        if (!sub->valid) {
                            continue;
                        }
                        if (bondp_send_health_probe(ctx_node->bdp_ctx, task->bdp_tjetty, sub) != URMA_SUCCESS) {
                            if (sub->link_ok) {
                                URMA_LOG_WARN("Health probe send failed, link down, tjetty:%p lidx:%d tidx:%d user_ctx:0x%lx\n",
                                    (void *)task->bdp_tjetty, sub->local_idx, sub->target_idx, sub->user_ctx);
                            }
                            sub->link_ok = false;
                        }
                    }
                }
                task->next_probe_ts_us = now_us + BONDP_HEALTH_CHECK_INTERVAL_US;
            }
            pthread_rwlock_unlock(&health->task_lock);
        }
        pthread_rwlock_unlock(&g_health_ctx_lock);
    }
    return NULL;
}

int bondp_register_health_check_task(bondp_context_t *bdp_ctx, bondp_target_jetty_t *bdp_tjetty)
{
    if (!bondp_health_check_enabled()) {
        return 0;
    }

    bondp_heath_check_ctx_t *health = &bdp_ctx->bondp_heath_check_ctx;
    bondp_health_task_t *task = calloc(1, sizeof(bondp_health_task_t));
    if (task == NULL) {
        return -1;
    }
    task->bdp_tjetty = bdp_tjetty;
    task->next_probe_ts_us = bondp_get_monotonic_us() + BONDP_HEALTH_CHECK_INTERVAL_US;

    int sub_task_cnt = 0;
    for (int i = 0; i < URMA_UBAGG_DEV_MAX_NUM; ++i) {
        for (int j = 0; j < URMA_UBAGG_DEV_MAX_NUM; ++j) {
            task->sub_tasks[i][j].valid = false;
        }
    }

    for (int i = 0; i < bdp_tjetty->local_dev_num; ++i) {
        for (int j = 0; j < bdp_tjetty->target_dev_num; ++j) {
            if (bdp_tjetty->p_tjetty[i][j] == NULL || bdp_tjetty->p_check_tseg[i][j] == NULL) {
                continue;
            }

            bondp_health_sub_task_t *sub = &task->sub_tasks[i][j];
            (void)memset(sub, 0, sizeof(*sub));
            sub->local_idx = i;
            sub->target_idx = j;
            sub->valid = true;
            sub->link_ok = true;
            sub->user_ctx = bondp_build_health_user_ctx((uint32_t)i, (uint32_t)j);
            URMA_LOG_DEBUG("Health subtask registered, tjetty:%p lidx:%d tidx:%d user_ctx:0x%lx\n",
                (void *)bdp_tjetty, i, j, sub->user_ctx);
            sub_task_cnt++;
        }
    }
    if (sub_task_cnt == 0) {
        free(task);
        URMA_LOG_ERR("Failed to register health task: no valid route\n");
        return -1;
    }
    pthread_rwlock_wrlock(&health->task_lock);
    task->next = health->task_list;
    health->task_list = task;
    pthread_rwlock_unlock(&health->task_lock);

    URMA_LOG_INFO("Health check task registered, tjetty:%p sub_task_cnt:%d next_probe_ts_us:%lu\n",
        (void *)bdp_tjetty, sub_task_cnt, task->next_probe_ts_us);
    return 0;
}

void bondp_unregister_health_check_task(bondp_target_jetty_t *bdp_tjetty)
{
    if (!bondp_health_check_enabled()) {
        return;
    }

    pthread_rwlock_rdlock(&g_health_ctx_lock);
    for (bondp_health_ctx_node_t *ctx_node = g_health_ctx_head; ctx_node != NULL; ctx_node = ctx_node->next) {
        bondp_heath_check_ctx_t *health = &ctx_node->bdp_ctx->bondp_heath_check_ctx;
        bool removed = false;
        pthread_rwlock_wrlock(&health->task_lock);
        bondp_health_task_t **pp = &health->task_list;
        while (*pp != NULL) {
            if ((*pp)->bdp_tjetty == bdp_tjetty) {
                bondp_health_task_t *to_free = *pp;
                *pp = (*pp)->next;
                bondp_free_health_task(to_free);
                removed = true;
                break;
            }
            pp = &(*pp)->next;
        }
        pthread_rwlock_unlock(&health->task_lock);
        if (removed) {
            URMA_LOG_INFO("Health check task unregistered, tjetty:%p\n", (void *)bdp_tjetty);
            break;
        }
    }
    pthread_rwlock_unlock(&g_health_ctx_lock);
}

bool bondp_try_handle_health_check_cr(bondp_context_t *bdp_ctx, int local_idx, urma_cr_t *cr)
{
    if (!bondp_health_check_enabled()) {
        return false;
    }
    if ((cr->user_ctx & BONDP_HEALTH_CHECK_MAGIC_MASK) != BONDP_HEALTH_CHECK_MAGIC) {
        return false;
    }

    uint32_t cr_local_idx = 0;
    uint32_t target_idx = 0;
    bondp_parse_health_user_ctx(cr->user_ctx, &cr_local_idx, &target_idx);
    if (cr_local_idx >= URMA_UBAGG_DEV_MAX_NUM || target_idx >= URMA_UBAGG_DEV_MAX_NUM ||
        (uint32_t)local_idx != cr_local_idx) {
        URMA_LOG_WARN("Health CR decode mismatch, user_ctx:0x%lx local_idx:%d parsed_local:%u parsed_target:%u\n",
            cr->user_ctx, local_idx, cr_local_idx, target_idx);
        return false;
    }

    bondp_heath_check_ctx_t *health = &bdp_ctx->bondp_heath_check_ctx;
    bool consumed = false;
    pthread_rwlock_wrlock(&health->task_lock);
    for (bondp_health_task_t *task = health->task_list; task != NULL; task = task->next) {
        bondp_health_sub_task_t *sub = &task->sub_tasks[cr_local_idx][target_idx];
        if (!sub->valid || sub->user_ctx != cr->user_ctx || sub->local_idx != local_idx) {
            continue;
        }

        bool old_ok = sub->link_ok;
        bool ok = (cr->status == URMA_CR_SUCCESS);
        sub->link_ok = ok;
        if (old_ok != ok) {
            URMA_LOG_INFO("Health link state changed, tjetty:%p lidx:%d tidx:%d user_ctx:0x%lx old:%d new:%d cr_status:%u\n",
                (void *)task->bdp_tjetty, local_idx, (int)target_idx, cr->user_ctx, old_ok, ok, cr->status);
        } else {
            URMA_LOG_DEBUG("Health CR handled, tjetty:%p lidx:%d tidx:%d user_ctx:0x%lx status:%u\n",
                (void *)task->bdp_tjetty, local_idx, (int)target_idx, cr->user_ctx, cr->status);
        }
        consumed = true;
        break;
    }
    pthread_rwlock_unlock(&health->task_lock);
    if (!consumed) {
        URMA_LOG_WARN("Health CR not matched to subtask, local_idx:%d user_ctx:0x%lx\n", local_idx, cr->user_ctx);
    }
    return consumed;
}

void bondp_stop_health_check_thread(void)
{
    bondp_global_context_t *global_ctx = g_bondp_global_ctx;
    if (!bondp_health_check_enabled()) {
        return;
    }

    if (global_ctx->health_thread_ctx.health_epoll_fd < 0) {
        return;
    }

    atomic_store(&global_ctx->health_thread_ctx.health_thread_stop, true);
    (void)pthread_join(global_ctx->health_thread_ctx.health_thread, NULL);
    atomic_store(&global_ctx->health_thread_ctx.health_thread_stop, false);

    if (global_ctx->health_thread_ctx.health_epoll_fd >= 0) {
        (void)close(global_ctx->health_thread_ctx.health_epoll_fd);
        global_ctx->health_thread_ctx.health_epoll_fd = -1;
    }

    pthread_rwlock_wrlock(&g_health_ctx_lock);
    while (g_health_ctx_head != NULL) {
        bondp_health_ctx_node_t *next = g_health_ctx_head->next;
        free(g_health_ctx_head);
        g_health_ctx_head = next;
    }
    pthread_rwlock_unlock(&g_health_ctx_lock);

    URMA_LOG_INFO("Health check thread stopped.\n");
}

int bondp_start_health_check_thread(void)
{
    bondp_global_context_t *global_ctx = g_bondp_global_ctx;
    int health_epoll_fd;

    if (!bondp_health_check_enabled()) {
        return 0;
    }

    health_epoll_fd = epoll_create(UBAGG_MAX_EVENT);
    if (health_epoll_fd == -1) {
        URMA_LOG_ERR("Failed to create health epoll %s\n", ub_strerror(errno));
        return -1;
    }

    global_ctx->health_thread_ctx.health_epoll_fd = health_epoll_fd;
    atomic_store(&global_ctx->health_thread_ctx.health_thread_stop, false);
    if (pthread_create(&global_ctx->health_thread_ctx.health_thread, NULL, bondp_health_check_thread, global_ctx) != 0) {
        URMA_LOG_ERR("Failed to create health check thread\n");
        (void)close(health_epoll_fd);
        global_ctx->health_thread_ctx.health_epoll_fd = -1;
        return -1;
    }
    URMA_LOG_INFO("Health check thread started.\n");
    return 0;
}

void bondp_destroy_health_check_ctx(bondp_context_t *bond_ctx)
{
    bondp_global_context_t *global_ctx = g_bondp_global_ctx;
    bondp_heath_check_ctx_t *health = NULL;

    if (!bondp_health_check_enabled()) {
        return;
    }

    health = &bond_ctx->bondp_heath_check_ctx;
    pthread_rwlock_wrlock(&health->task_lock);
    while (health->task_list != NULL) {
        bondp_health_task_t *next = health->task_list->next;
        bondp_free_health_task(health->task_list);
        health->task_list = next;
    }
    pthread_rwlock_unlock(&health->task_lock);
    bondp_unregister_health_ctx_global(bond_ctx);

    if (global_ctx->health_thread_ctx.health_epoll_fd >= 0 && health->health_check_fd >= 0) {
        (void)epoll_ctl(global_ctx->health_thread_ctx.health_epoll_fd,
            EPOLL_CTL_DEL, health->health_check_fd, NULL);
        (void)close(health->health_check_fd);
        health->health_check_fd = -1;
    }

    bondp_unregister_health_check_seg(bond_ctx);

    URMA_LOG_INFO("Health check ctx free, dev_name: %s, eid_idx: %u.\n",
        bond_ctx->v_ctx.dev->name, bond_ctx->v_ctx.eid_index);
}

int bondp_create_health_check_ctx(bondp_context_t *bond_ctx)
{
    bondp_global_context_t *global_ctx = g_bondp_global_ctx;
    bondp_heath_check_ctx_t *health = &bond_ctx->bondp_heath_check_ctx;
    struct epoll_event ev = {0};

    if (!bondp_health_check_enabled()) {
        return 0;
    }

    pthread_rwlock_init(&health->task_lock, NULL);
    health->task_list = NULL;

    if (global_ctx->health_thread_ctx.health_epoll_fd < 0) {
        URMA_LOG_ERR("Health check thread is not created\n");
        return -1;
    }

    if (bondp_register_health_check_seg(bond_ctx) != 0) {
        return -1;
    }

    if (global_ctx->health_thread_ctx.health_epoll_fd < 0) {
        URMA_LOG_ERR("Invalid health epoll fd\n");
        bondp_unregister_health_check_seg(bond_ctx);
        return -1;
    }

    health->health_check_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (health->health_check_fd < 0) {
        URMA_LOG_ERR("Failed to create health_check_fd, errno: %d\n", errno);
        bondp_unregister_health_check_seg(bond_ctx);
        return -1;
    }

    ev.events = EPOLLIN;
    ev.data.ptr = (void *)bond_ctx;
    if (epoll_ctl(global_ctx->health_thread_ctx.health_epoll_fd,
        EPOLL_CTL_ADD, health->health_check_fd, &ev) != 0) {
        URMA_LOG_ERR("Failed to add ctx async fd to health epoll, errno: %d\n", errno);
        (void)close(health->health_check_fd);
        health->health_check_fd = -1;
        bondp_unregister_health_check_seg(bond_ctx);
        return -1;
    }

    URMA_LOG_INFO("Health check ctx enabled, dev_name: %s, eid_idx: %u, fd: %d.\n",
        bond_ctx->v_ctx.dev->name, bond_ctx->v_ctx.eid_index, health->health_check_fd);

    bondp_register_health_ctx_global(bond_ctx);

    return 0;
}