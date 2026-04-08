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
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/eventfd.h>

#include "urma_log.h"
#include "urma_api.h"
#include "bondp_health_check.h"

#define UBAGG_MAX_EVENT 1
#define BONDP_HEALTH_CHECK_ENV "BondHealthCheck"
#define BONDP_HEALTH_CHECK_BUF_LEN (4096)
#define BONDP_HEALTH_CHECK_EPOLL_TIMEOUT_MS (100)


typedef struct bondp_health_epoll_data {
    bondp_context_t *bond_ctx;
} bondp_health_epoll_data_t;

typedef struct bondp_health_event_node {
    bondp_health_event_t event;
    bondp_health_event_info_t info;
    struct ub_list node;
} bondp_health_event_node_t;

static bool bondp_valid_health_event(bondp_health_event_t event)
{
    return ((uint32_t)event < (uint32_t)BONDP_HEALTH_EVENT_MAX);
}

static bool bondp_health_event_push(bondp_context_t *bdp_ctx, bondp_health_event_t event,
    const bondp_health_event_info_t *info)
{
    bondp_health_event_node_t *node = calloc(1, sizeof(*node));
    if (node == NULL) {
        URMA_LOG_WARN("Failed to alloc health event node, event:%d\n", event);
        return false;
    }

    node->event = event;
    node->info = *info;

    ub_list_init(&node->node);

    bondp_heath_check_ctx_t *health = &bdp_ctx->bondp_heath_check_ctx;
    pthread_spin_lock(&health->event_lock);
    ub_list_push_back(&health->event_list, &node->node);
    pthread_spin_unlock(&health->event_lock);
    return true;
}

static void bondp_health_event_pop(bondp_heath_check_ctx_t *health, struct ub_list *list)
{
    pthread_spin_lock(&health->event_lock);
    ub_list_move(&health->event_list, list);
    pthread_spin_unlock(&health->event_lock);
}

static void bondp_free_health_event_list(struct ub_list *list)
{
    bondp_health_event_node_t *event = NULL;
    bondp_health_event_node_t *next = NULL;

    UB_LIST_FOR_EACH_SAFE(event, next, node, list) {
        ub_list_remove(&event->node);
        free(event);
    }
}

void bondp_notify_health_event(bondp_context_t *bdp_ctx, bondp_health_event_t event,
    const bondp_health_event_info_t *info)
{
    if (bdp_ctx == NULL || info == NULL || !bondp_health_check_enabled()) {
        return;
    }

    bondp_heath_check_ctx_t *health = &bdp_ctx->bondp_heath_check_ctx;
    if (health->health_check_fd < 0) {
        return;
    }

    if (!bondp_valid_health_event(event)) {
        return;
    }

    if (!bondp_health_event_push(bdp_ctx, event, info)) {
        return;
    }

    if (eventfd_write(health->health_check_fd, 1) != 0 && errno != EAGAIN) {
        URMA_LOG_WARN("Failed to notify health event, fd:%d event:%d errno:%d\n",
            health->health_check_fd, event, errno);
    }
}

static void bondp_handle_health_event(bondp_context_t *bond_ctx, bondp_health_event_t event,
    const bondp_health_event_info_t *info)
{
    switch (event) {
        case BONDP_HEALTH_EVENT_TA_TIMEOUT:
            if (info != NULL) {
                URMA_LOG_DEBUG("Health event handled: CR update, dev_name:%s eid_idx:%u lidx:%d tidx:%d "
                    "user_ctx:0x%lx old:%d new:%d cr_status:%u\n",
                    bond_ctx->v_ctx.dev->name, bond_ctx->v_ctx.eid_index,
                    info->local_idx, info->target_idx, info->user_ctx,
                    info->old_ok, info->new_ok, info->cr_status);
            } else {
                URMA_LOG_DEBUG("Health event handled: CR update, dev_name:%s eid_idx:%u\n",
                    bond_ctx->v_ctx.dev->name, bond_ctx->v_ctx.eid_index);
            }
            break;
        default:
            URMA_LOG_WARN("Unknown health event:%d, dev_name:%s eid_idx:%u\n",
                event, bond_ctx->v_ctx.dev->name, bond_ctx->v_ctx.eid_index);
            break;
    }
}

static void bondp_handle_health_queued_events(bondp_context_t *bond_ctx)
{
    struct ub_list event_list = UB_LIST_INITIALIZER(&event_list);
    bondp_heath_check_ctx_t *health = &bond_ctx->bondp_heath_check_ctx;
    bondp_health_event_node_t *cur = NULL;
    bondp_health_event_node_t *next = NULL;

    bondp_health_event_pop(health, &event_list);

    UB_LIST_FOR_EACH_SAFE(cur, next, node, &event_list) {
        bondp_handle_health_event(bond_ctx, cur->event, &cur->info);
        ub_list_remove(&cur->node);
        free(cur);
    }
}

static void bondp_handle_epoll_event(const struct epoll_event *event)
{
    if (event == NULL) {
        return;
    }

    if ((event->events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
        URMA_LOG_WARN("Health epoll error event:0x%x\n", event->events);
        return;
    }

    if ((event->events & EPOLLIN) == 0) {
        return;
    }

    bondp_health_epoll_data_t *epoll_data = (bondp_health_epoll_data_t *)event->data.ptr;
    if (epoll_data == NULL || epoll_data->bond_ctx == NULL) {
        URMA_LOG_WARN("Health epoll event has NULL context\n");
        return;
    }

    bondp_context_t *bond_ctx = epoll_data->bond_ctx;

    bondp_heath_check_ctx_t *health = &bond_ctx->bondp_heath_check_ctx;
    if (health->health_check_fd < 0) {
        return;
    }

    while (true) {
        eventfd_t cnt = 0;
        if (eventfd_read(health->health_check_fd, &cnt) == 0) {
            continue;
        }
        if (errno == EAGAIN || errno == EINTR) {
            break;
        }
        URMA_LOG_WARN("Failed to read health eventfd, fd:%d errno:%d\n", health->health_check_fd, errno);
        break;
    }

    bondp_handle_health_queued_events(bond_ctx);
}

static bool bondp_read_health_check_enable(void)
{
    const char *value = getenv(BONDP_HEALTH_CHECK_ENV);
    return (value != NULL && strcmp(value, "true") == 0);
}

static bool bondp_health_check_enabled(void)
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
    bond_ctx->bondp_heath_check_ctx.epoll_data = NULL;
    ub_list_init(&bond_ctx->bondp_heath_check_ctx.event_list);
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

    health->check_buf = calloc(1, health->check_buf_len);
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
    }
    return 0;
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

        int event_num = epoll_wait(epoll_fd, events, UBAGG_MAX_EVENT, BONDP_HEALTH_CHECK_EPOLL_TIMEOUT_MS);
        if (event_num < 0 && errno != EINTR) {
            URMA_LOG_ERR("Health check epoll_wait failed, errno: %d\n", errno);
            (void)usleep(BONDP_HEALTH_CHECK_EPOLL_TIMEOUT_MS * 1000);
            continue;
        }

        if (event_num > 0) {
            for (int i = 0; i < event_num; ++i) {
                bondp_handle_epoll_event(&events[i]);
            }
        }

    }
    return NULL;
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
    if (global_ctx->health_thread_ctx.health_epoll_fd >= 0 && health->health_check_fd >= 0) {
        (void)epoll_ctl(global_ctx->health_thread_ctx.health_epoll_fd,
            EPOLL_CTL_DEL, health->health_check_fd, NULL);
        (void)close(health->health_check_fd);
        health->health_check_fd = -1;
    }

    free(health->epoll_data);
    health->epoll_data = NULL;
    struct ub_list event_list = UB_LIST_INITIALIZER(&event_list);
    bondp_detach_health_event_queue(health, &event_list);
    bondp_free_health_event_queue(&event_list);
    pthread_spin_destroy(&health->event_lock);

    bondp_unregister_health_check_seg(bond_ctx);

    URMA_LOG_INFO("Health check ctx free, dev_name: %s, eid_idx: %u.\n",
        bond_ctx->v_ctx.dev->name, bond_ctx->v_ctx.eid_index);
}

int bondp_create_health_check_ctx(bondp_context_t *bond_ctx)
{
    bondp_global_context_t *global_ctx = g_bondp_global_ctx;
    bondp_heath_check_ctx_t *health = &bond_ctx->bondp_heath_check_ctx;
    struct epoll_event ev = {0};
    bondp_health_epoll_data_t *epoll_data = NULL;
    int ret = -1;

    if (!bondp_health_check_enabled()) {
        return 0;
    }

    if (pthread_spin_init(&health->event_lock, PTHREAD_PROCESS_PRIVATE) != 0) {
        URMA_LOG_ERR("Failed to init health event lock\n");
        goto ERR;
    }

    if (global_ctx->health_thread_ctx.health_epoll_fd < 0) {
        URMA_LOG_ERR("Health check thread is not created\n");
        goto ERR_EVENT_LOCK;
    }

    if (bondp_register_health_check_seg(bond_ctx) != 0) {
        goto ERR_EVENT_LOCK;
    }

    if (global_ctx->health_thread_ctx.health_epoll_fd < 0) {
        URMA_LOG_ERR("Invalid health epoll fd\n");
        goto ERR_SEG;
    }

    health->health_check_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (health->health_check_fd < 0) {
        URMA_LOG_ERR("Failed to create health_check_fd, errno: %d\n", errno);
        goto ERR_SEG;
    }

    epoll_data = calloc(1, sizeof(*epoll_data));
    if (epoll_data == NULL) {
        URMA_LOG_ERR("Failed to alloc health epoll data\n");
        goto ERR_EVENTFD;
    }
    epoll_data->bond_ctx = bond_ctx;
    health->epoll_data = epoll_data;

    ev.events = EPOLLIN;
    ev.data.ptr = health->epoll_data;
    if (epoll_ctl(global_ctx->health_thread_ctx.health_epoll_fd,
        EPOLL_CTL_ADD, health->health_check_fd, &ev) != 0) {
        URMA_LOG_ERR("Failed to add ctx async fd to health epoll, errno: %d\n", errno);
        goto ERR_EPOLL_ADD;
    }

    URMA_LOG_INFO("Health check ctx enabled, dev_name: %s, eid_idx: %u, fd: %d.\n",
        bond_ctx->v_ctx.dev->name, bond_ctx->v_ctx.eid_index, health->health_check_fd);

    return 0;

ERR_EPOLL_ADD:
    free(health->epoll_data);
    health->epoll_data = NULL;
ERR_EVENTFD:
    (void)close(health->health_check_fd);
    health->health_check_fd = -1;
ERR_SEG:
    bondp_unregister_health_check_seg(bond_ctx);
ERR_EVENT_LOCK:
    pthread_spin_destroy(&health->event_lock);
ERR:
    return ret;
}