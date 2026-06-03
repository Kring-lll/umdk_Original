/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bond worker thread implementation
 */

#include <pthread.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include "bondp_timewheel.h"
#include "ub_hash.h"
#include "ub_hmap.h"
#include "ub_util.h"
#include "urma_log.h"
#include "bondp_worker.h"

#define BONDP_WORKER_EVENT_NUM (16)
#define BONDP_WORKER_HANDLER_MAP_SIZE (64U)

typedef struct bondp_worker_event_handler {
    int fd;
    bondp_worker_event_fn_t handler;
    void *arg;
    bool deleting;
    uint32_t refcnt;
    struct ub_hmap_node hmap_node;
} bondp_worker_event_handler_t;

typedef struct bondp_worker {
    pthread_t thread;
    pthread_mutex_t lock;
    int epoll_fd;
    int wake_fd;
    bool stopping;
    bool thread_created;
    uint64_t last_tick_ns;
    struct ub_hmap handler_map;
    tw_t *tw;
} bondp_worker_t;

static bondp_worker_t *g_bondp_worker;

static uint32_t bondp_worker_event_hash(int fd)
{
    return ub_hash_bytes(&fd, sizeof(fd), 0);
}

static bondp_worker_event_handler_t *bondp_worker_lookup_event_handler(bondp_worker_t *worker, int fd)
{
    bondp_worker_event_handler_t *entry = NULL;
    uint32_t hash = bondp_worker_event_hash(fd);

    HMAP_FOR_EACH_WITH_HASH(entry, hmap_node, hash, &worker->handler_map) {
        if (entry->fd == fd) {
            return entry;
        }
    }

    return NULL;
}

static void __attribute__((unused)) bondp_worker_free_event_handler(bondp_worker_t *worker, bondp_worker_event_handler_t *entry)
{
    ub_hmap_remove(&worker->handler_map, &entry->hmap_node);
    free(entry);
}

static uint64_t bondp_worker_now_ns(void)
{
    struct timespec ts = {0};

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return (uint64_t)ts.tv_sec * NANO_IN_SEC + (uint64_t)ts.tv_nsec;
}

static int bondp_worker_wakeup(bondp_worker_t *worker)
{
    uint64_t one = 1;
    ssize_t ret = write(worker->wake_fd, &one, sizeof(one));

    if (ret == (ssize_t)sizeof(one) || (ret < 0 && errno == EAGAIN)) {
        return TW_OK;
    }

    URMA_LOG_ERR("Failed to wake bond worker thread, errno: %d.\n", errno);
    return TW_SYS_ERROR;
}

static void bondp_worker_drain_wakeup_fd(int wake_fd)
{
    uint64_t value;

    while (read(wake_fd, &value, sizeof(value)) == (ssize_t)sizeof(value)) {
    }

    if (errno != EAGAIN) {
        URMA_LOG_WARN("Failed to drain bond worker wake fd, errno: %d.\n", errno);
    }
}

static bool bondp_worker_is_stopping(bondp_worker_t *worker)
{
    bool stopping;

    (void)pthread_mutex_lock(&worker->lock);
    stopping = worker->stopping;
    (void)pthread_mutex_unlock(&worker->lock);
    return stopping;
}

static int bondp_worker_get_wait_timeout_ms(bondp_worker_t *worker, uint64_t now_ns)
{
    uint32_t tick_ms = tw_get_tick_ms(worker->tw);
    uint64_t tick_ns = (uint64_t)tick_ms * 1000000ULL;
    uint64_t next_tick_ns = worker->last_tick_ns + tick_ns;

    if (tick_ms == 0 || now_ns >= next_tick_ns) {
        return 0;
    }

    return (int)DIV_ROUND_UP(next_tick_ns - now_ns, 1000000ULL);
}

static void bondp_worker_advance_by_now(bondp_worker_t *worker)
{
    uint32_t tick_ms = tw_get_tick_ms(worker->tw);
    uint64_t tick_ns = (uint64_t)tick_ms * 1000000ULL;
    uint64_t now_ns = bondp_worker_now_ns();
    uint64_t tick_cnt;

    if (tick_ms == 0 || now_ns < worker->last_tick_ns + tick_ns) {
        return;
    }

    tick_cnt = (now_ns - worker->last_tick_ns) / tick_ns;
    worker->last_tick_ns += tick_cnt * tick_ns;
    tw_advance(worker->tw, tick_cnt);
}

static void bondp_worker_put_event_handler(bondp_worker_t *worker, bondp_worker_event_handler_t *entry)
{
    if (entry->refcnt > 0) {
        entry->refcnt--;
    }
    if (entry->deleting && entry->refcnt == 0) {
        free(entry);
    }
}

static void bondp_worker_handle_epoll_event(bondp_worker_t *worker, int fd, uint32_t events)
{
    bondp_worker_event_handler_t *entry;
    bondp_worker_event_fn_t handler;
    void *arg;

    if (fd == worker->wake_fd) {
        if ((events & EPOLLIN) != 0) {
            bondp_worker_drain_wakeup_fd(worker->wake_fd);
        }
        return;
    }

    if ((events & EPOLLIN) == 0) {
        return;
    }

    (void)pthread_mutex_lock(&worker->lock);
    entry = bondp_worker_lookup_event_handler(worker, fd);
    if (entry == NULL || entry->deleting) {
        (void)pthread_mutex_unlock(&worker->lock);
        return;
    }
    entry->refcnt++;
    handler = entry->handler;
    arg = entry->arg;
    (void)pthread_mutex_unlock(&worker->lock);

    handler(fd, arg);

    (void)pthread_mutex_lock(&worker->lock);
    bondp_worker_put_event_handler(worker, entry);
    (void)pthread_mutex_unlock(&worker->lock);
}

static void *bondp_worker_thread_main(void *arg)
{
    bondp_worker_t *worker = arg;
    struct epoll_event events[BONDP_WORKER_EVENT_NUM];
    int i;

    while (true) {
        uint64_t now_ns = bondp_worker_now_ns();
        int timeout_ms = bondp_worker_get_wait_timeout_ms(worker, now_ns);
        int nfds = epoll_wait(worker->epoll_fd, events, BONDP_WORKER_EVENT_NUM, timeout_ms);

        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            URMA_LOG_ERR("Bond worker epoll_wait failed, errno: %d.\n", errno);
            break;
        }

        for (i = 0; i < nfds; i++) {
            bondp_worker_handle_epoll_event(worker, events[i].data.fd, events[i].events);
        }

        if (bondp_worker_is_stopping(worker)) {
            break;
        }

        bondp_worker_advance_by_now(worker);
    }

    return NULL;
}

static bondp_worker_t *bondp_worker_create_instance(void)
{
    bondp_worker_t *worker = calloc(1, sizeof(*worker));
    tw_cfg_t tw_cfg = {
        .tick_ms = TW_DEFAULT_TICK_MS,
        .slot_num = TW_DEFAULT_SLOT_NUM,
    };
    struct epoll_event event = {
        .events = EPOLLIN,
        .data.fd = -1,
    };
    int ret;

    if (worker == NULL) {
        URMA_LOG_ERR("Failed to alloc bond worker.\n");
        return NULL;
    }

    worker->epoll_fd = -1;
    worker->wake_fd = -1;
    worker->tw = tw_create(&tw_cfg);
    if (worker->tw == NULL) {
        free(worker);
        return NULL;
    }

    ret = pthread_mutex_init(&worker->lock, NULL);
    if (ret != 0) {
        URMA_LOG_ERR("Failed to init bond worker mutex, ret: %d.\n", ret);
        goto err_tw;
    }

    if (ub_hmap_init(&worker->handler_map, BONDP_WORKER_HANDLER_MAP_SIZE) != 0) {
        URMA_LOG_ERR("Failed to init bond worker handler map.\n");
        goto err_lock;
    }

    worker->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (worker->epoll_fd < 0) {
        URMA_LOG_ERR("Failed to create bond worker epoll fd, errno: %d.\n", errno);
        goto err_map;
    }

    worker->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (worker->wake_fd < 0) {
        URMA_LOG_ERR("Failed to create bond worker wake fd, errno: %d.\n", errno);
        goto err_epoll;
    }

    event.data.fd = worker->wake_fd;
    if (epoll_ctl(worker->epoll_fd, EPOLL_CTL_ADD, worker->wake_fd, &event) != 0) {
        URMA_LOG_ERR("Failed to add wake fd to bond worker epoll, errno: %d.\n", errno);
        goto err_wake;
    }

    worker->last_tick_ns = bondp_worker_now_ns();
    ret = pthread_create(&worker->thread, NULL, bondp_worker_thread_main, worker);
    if (ret != 0) {
        URMA_LOG_ERR("Failed to create bond worker thread, ret: %d.\n", ret);
        goto err_wake;
    }
    worker->thread_created = true;

    return worker;

err_wake:
    (void)close(worker->wake_fd);
err_epoll:
    (void)close(worker->epoll_fd);
err_map:
    ub_hmap_destroy(&worker->handler_map);
err_lock:
    (void)pthread_mutex_destroy(&worker->lock);
err_tw:
    tw_destroy(worker->tw);
    free(worker);
    return NULL;
}

int bondp_worker_create(void)
{
    if (g_bondp_worker != NULL) {
        URMA_LOG_ERR("Bond worker already created.\n");
        return TW_SYS_ERROR;
    }

    g_bondp_worker = bondp_worker_create_instance();
    return g_bondp_worker == NULL ? TW_SYS_ERROR : TW_OK;
}

void bondp_worker_destroy(void)
{
    bondp_worker_t *worker = g_bondp_worker;
    bondp_worker_event_handler_t *entry = NULL;
    bondp_worker_event_handler_t *next = NULL;

    if (worker == NULL) {
        return;
    }
    g_bondp_worker = NULL;

    (void)pthread_mutex_lock(&worker->lock);
    worker->stopping = true;
    (void)pthread_mutex_unlock(&worker->lock);
    (void)bondp_worker_wakeup(worker);

    if (worker->thread_created) {
        (void)pthread_join(worker->thread, NULL);
    }

    if (worker->wake_fd >= 0) {
        (void)close(worker->wake_fd);
    }
    if (worker->epoll_fd >= 0) {
        (void)close(worker->epoll_fd);
    }
    HMAP_FOR_EACH_SAFE(entry, next, hmap_node, &worker->handler_map) {
        ub_hmap_remove(&worker->handler_map, &entry->hmap_node);
        free(entry);
    }
    ub_hmap_destroy(&worker->handler_map);
    (void)pthread_mutex_destroy(&worker->lock);
    tw_destroy(worker->tw);
    free(worker);
}

int bondp_worker_schedule(uint64_t delay_ms, bondp_worker_task_fn_t fn, void *arg, bondp_worker_task_id_t *task_id)
{
    int ret;
    bondp_worker_t *worker = g_bondp_worker;

    if (worker == NULL) {
        return TW_INVALID_PARAM_ERROR;
    }

    ret = tw_schedule(worker->tw, delay_ms, fn, arg, task_id);
    if (ret == TW_OK) {
        (void)bondp_worker_wakeup(worker);
    }
    return ret;
}

int bondp_worker_cancel(bondp_worker_task_id_t task_id)
{
    int ret;
    bondp_worker_t *worker = g_bondp_worker;

    if (worker == NULL) {
        return TW_INVALID_PARAM_ERROR;
    }

    ret = tw_cancel(worker->tw, task_id);
    if (ret == TW_OK) {
        (void)bondp_worker_wakeup(worker);
    }
    return ret;
}

int bondp_worker_add_fd(int fd, bondp_worker_event_fn_t handler, void *arg)
{
    bondp_worker_t *worker = g_bondp_worker;
    bondp_worker_event_handler_t *entry;
    struct epoll_event event = {
        .events = EPOLLIN,
        .data.fd = fd,
    };
    int ret = TW_OK;

    if (worker == NULL || fd < 0 || handler == NULL) {
        return TW_INVALID_PARAM_ERROR;
    }

    entry = calloc(1, sizeof(*entry));
    if (entry == NULL) {
        URMA_LOG_ERR("Failed to alloc bond worker event handler.\n");
        return TW_NO_MEMORY_ERROR;
    }

    entry->fd = fd;
    entry->handler = handler;
    entry->arg = arg;

    (void)pthread_mutex_lock(&worker->lock);
    if (bondp_worker_lookup_event_handler(worker, fd) != NULL) {
        ret = TW_SYS_ERROR;
        goto out_unlock;
    }
    if (epoll_ctl(worker->epoll_fd, EPOLL_CTL_ADD, fd, &event) != 0) {
        URMA_LOG_ERR("Failed to add fd %d to bond worker epoll, errno: %d.\n", fd, errno);
        ret = TW_SYS_ERROR;
        goto out_unlock;
    }
    ub_hmap_insert(&worker->handler_map, &entry->hmap_node, bondp_worker_event_hash(fd));

out_unlock:
    (void)pthread_mutex_unlock(&worker->lock);
    if (ret != TW_OK) {
        free(entry);
        return ret;
    }

    (void)bondp_worker_wakeup(worker);
    return TW_OK;
}

int bondp_worker_del_fd(int fd)
{
    bondp_worker_t *worker = g_bondp_worker;
    bondp_worker_event_handler_t *entry;

    if (worker == NULL || fd < 0) {
        return TW_INVALID_PARAM_ERROR;
    }

    (void)pthread_mutex_lock(&worker->lock);
    entry = bondp_worker_lookup_event_handler(worker, fd);
    if (entry == NULL) {
        (void)pthread_mutex_unlock(&worker->lock);
        return TW_NOT_FOUND_ERROR;
    }
    if (epoll_ctl(worker->epoll_fd, EPOLL_CTL_DEL, fd, NULL) != 0) {
        URMA_LOG_ERR("Failed to del fd %d from bond worker epoll, errno: %d.\n", fd, errno);
        (void)pthread_mutex_unlock(&worker->lock);
        return TW_SYS_ERROR;
    }
    ub_hmap_remove(&worker->handler_map, &entry->hmap_node);
    entry->deleting = true;
    if (entry->refcnt == 0) {
        free(entry);
    }
    (void)pthread_mutex_unlock(&worker->lock);

    (void)bondp_worker_wakeup(worker);
    return TW_OK;
}
