/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bond worker thread implementation
 */

#include <errno.h>
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

#define BONDP_WORKER_EVENT_NUM         (16)
#define BONDP_WORKER_HANDLER_MAP_SIZE  (64U)
#define BONDP_WORKER_MAX_ADVANCE_TICKS (64U)

typedef struct bondp_worker_event_handler {
    int fd;
    bondp_worker_event_fn_t handler;
    void *arg;
    bool deleting;
    uint32_t refcnt;
    struct ub_hmap_node hmap_node;
} bondp_worker_event_handler_t;

typedef enum bondp_worker_state {
    BONDP_WORKER_STATE_INIT = 0,
    BONDP_WORKER_STATE_RUNNING,
    BONDP_WORKER_STATE_STOPPING,
    BONDP_WORKER_STATE_FAILED,
} bondp_worker_state_t;

typedef struct bondp_worker {
    pthread_t thread;
    pthread_mutex_t lock;
    int epoll_fd;
    int wake_fd;
    bondp_worker_state_t state;
    int err_code;
    uint64_t last_tick_ns;
    struct ub_hmap handler_map;
    tw_t *tw;
} bondp_worker_t;

static bondp_worker_t *g_bondp_worker;
static pthread_mutex_t g_bondp_worker_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t bondp_worker_event_hash(int fd)
{
    return ub_hash_bytes(&fd, sizeof(fd), 0);
}

static bondp_worker_event_handler_t *bondp_worker_lookup_event_handler(bondp_worker_t *worker, int fd)
{
    bondp_worker_event_handler_t *entry = NULL;
    uint32_t hash = bondp_worker_event_hash(fd);

    HMAP_FOR_EACH_WITH_HASH(entry, hmap_node, hash, &worker->handler_map)
    {
        if (entry->fd == fd) {
            return entry;
        }
    }

    return NULL;
}

static void bondp_worker_free_event_handler(bondp_worker_t *worker, bondp_worker_event_handler_t *entry)
{
    ub_hmap_remove(&worker->handler_map, &entry->hmap_node);
    free(entry);
}

static uint64_t bondp_worker_now_ns(void)
{
    struct timespec ts = {0};

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        URMA_LOG_ERR("Failed to get bond worker monotonic time, errno: %d.\n", errno);
        return 0;
    }

    return (uint64_t)ts.tv_sec * NANO_IN_SEC + (uint64_t)ts.tv_nsec;
}

static int bondp_worker_wakeup(bondp_worker_t *worker)
{
    uint64_t one = 1;
    ssize_t ret;

    do {
        ret = write(worker->wake_fd, &one, sizeof(one));
    } while (ret < 0 && errno == EINTR);

    if (ret == (ssize_t)sizeof(one) || (ret < 0 && errno == EAGAIN)) {
        return 0;
    }

    URMA_LOG_ERR("Failed to wake bond worker thread, errno: %d.\n", errno);
    return ret < 0 ? -errno : -EIO;
}

static void bondp_worker_drain_wakeup_fd(int wake_fd)
{
    uint64_t value;
    ssize_t ret;

    while (true) {
        ret = read(wake_fd, &value, sizeof(value));
        if (ret == (ssize_t)sizeof(value)) {
            continue;
        }
        if (ret < 0 && errno == EINTR) {
            continue;
        }
        if (ret < 0 && errno == EAGAIN) {
            return;
        }
        break;
    }

    if (ret == 0) {
        URMA_LOG_WARN("Unexpected EOF while draining bond worker wake fd.\n");
        return;
    }

    URMA_LOG_WARN("Failed to drain bond worker wake fd, errno: %d.\n", errno);
}

static int bondp_worker_get_status_locked(bondp_worker_t *worker)
{
    if (worker->state == BONDP_WORKER_STATE_FAILED) {
        return worker->err_code != 0 ? worker->err_code : -EIO;
    }

    if (worker->state == BONDP_WORKER_STATE_STOPPING) {
        return -ESHUTDOWN;
    }

    return 0;
}

static bondp_worker_event_handler_t *bondp_worker_acquire_event_handler_locked(bondp_worker_t *worker,
                                                                               int fd, bondp_worker_event_fn_t *handler, void **arg)
{
    bondp_worker_event_handler_t *entry = bondp_worker_lookup_event_handler(worker, fd);

    if (entry == NULL || entry->deleting) {
        return NULL;
    }

    entry->refcnt++;
    *handler = entry->handler;
    *arg = entry->arg;
    return entry;
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
    tick_cnt = MIN(tick_cnt, BONDP_WORKER_MAX_ADVANCE_TICKS);
    worker->last_tick_ns += tick_cnt * tick_ns;
    tw_advance(worker->tw, tick_cnt);
}

static void bondp_worker_put_event_handler(bondp_worker_event_handler_t *entry)
{
    if (entry->refcnt > 0) {
        entry->refcnt--;
    }
    if (entry->deleting && entry->refcnt == 0) {
        free(entry);
    }
}

static bool bondp_worker_should_stop_locked(bondp_worker_t *worker)
{
    return worker->state == BONDP_WORKER_STATE_STOPPING || worker->state == BONDP_WORKER_STATE_FAILED;
}

static void bondp_worker_handle_epoll_event(bondp_worker_t *worker, int fd, uint32_t events)
{
    bondp_worker_event_handler_t *entry;
    bondp_worker_event_fn_t handler;
    void *arg;
    uint32_t handled_events = EPOLLIN | EPOLLERR | EPOLLHUP;

#ifdef EPOLLRDHUP
    handled_events |= EPOLLRDHUP;
#endif

    if (fd == worker->wake_fd) {
        if ((events & EPOLLIN) != 0) {
            bondp_worker_drain_wakeup_fd(worker->wake_fd);
        }
        return;
    }

    if ((events & handled_events) == 0) {
        return;
    }

    (void)pthread_mutex_lock(&worker->lock);
    entry = bondp_worker_acquire_event_handler_locked(worker, fd, &handler, &arg);
    if (entry == NULL) {
        (void)pthread_mutex_unlock(&worker->lock);
        return;
    }
    (void)pthread_mutex_unlock(&worker->lock);

    handler(arg);

    (void)pthread_mutex_lock(&worker->lock);
    bondp_worker_put_event_handler(entry);
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
            (void)pthread_mutex_lock(&worker->lock);
            worker->state = BONDP_WORKER_STATE_FAILED;
            worker->err_code = -errno;
            (void)pthread_mutex_unlock(&worker->lock);
            break;
        }

        for (i = 0; i < nfds; i++) {
            bondp_worker_handle_epoll_event(worker, events[i].data.fd, events[i].events);
        }

        (void)pthread_mutex_lock(&worker->lock);
        if (bondp_worker_should_stop_locked(worker)) {
            (void)pthread_mutex_unlock(&worker->lock);
            break;
        }
        (void)pthread_mutex_unlock(&worker->lock);

        bondp_worker_advance_by_now(worker);
    }

    return NULL;
}

static bondp_worker_t *bondp_worker_create_instance(int *err_code)
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
        *err_code = -ENOMEM;
        return NULL;
    }

    worker->epoll_fd = -1;
    worker->wake_fd = -1;
    worker->tw = tw_create(&tw_cfg);
    if (worker->tw == NULL) {
        *err_code = -ENOMEM;
        free(worker);
        return NULL;
    }

    ret = pthread_mutex_init(&worker->lock, NULL);
    if (ret != 0) {
        URMA_LOG_ERR("Failed to init bond worker mutex, ret: %d.\n", ret);
        *err_code = -ret;
        goto err_tw;
    }

    if (ub_hmap_init(&worker->handler_map, BONDP_WORKER_HANDLER_MAP_SIZE) != 0) {
        URMA_LOG_ERR("Failed to init bond worker handler map.\n");
        *err_code = -ENOMEM;
        goto err_lock;
    }

    worker->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (worker->epoll_fd < 0) {
        URMA_LOG_ERR("Failed to create bond worker epoll fd, errno: %d.\n", errno);
        *err_code = -errno;
        goto err_map;
    }

    worker->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (worker->wake_fd < 0) {
        URMA_LOG_ERR("Failed to create bond worker wake fd, errno: %d.\n", errno);
        *err_code = -errno;
        goto err_epoll;
    }

    event.data.fd = worker->wake_fd;
    if (epoll_ctl(worker->epoll_fd, EPOLL_CTL_ADD, worker->wake_fd, &event) != 0) {
        URMA_LOG_ERR("Failed to add wake fd to bond worker epoll, errno: %d.\n", errno);
        *err_code = -errno;
        goto err_wake;
    }

    worker->last_tick_ns = bondp_worker_now_ns();
    ret = pthread_create(&worker->thread, NULL, bondp_worker_thread_main, worker);
    if (ret != 0) {
        URMA_LOG_ERR("Failed to create bond worker thread, ret: %d.\n", ret);
        *err_code = -ret;
        goto err_wake;
    }
    worker->state = BONDP_WORKER_STATE_RUNNING;

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
    bondp_worker_t *worker;
    int ret = 0;

    (void)pthread_mutex_lock(&g_bondp_worker_lock);
    if (g_bondp_worker != NULL) {
        URMA_LOG_ERR("Bond worker already created.\n");
        ret = -EEXIST;
        goto out_unlock;
    }

    worker = bondp_worker_create_instance(&ret);
    if (worker == NULL) {
        goto out_unlock;
    }

    g_bondp_worker = worker;
    ret = 0;

out_unlock:
    (void)pthread_mutex_unlock(&g_bondp_worker_lock);
    return ret;
}

void bondp_worker_destroy(void)
{
    bondp_worker_t *worker;
    bondp_worker_event_handler_t *entry = NULL;
    bondp_worker_event_handler_t *next = NULL;

    (void)pthread_mutex_lock(&g_bondp_worker_lock);
    worker = g_bondp_worker;
    if (worker == NULL) {
        (void)pthread_mutex_unlock(&g_bondp_worker_lock);
        return;
    }
    /*
     * Destroy is only called after external users have stopped accessing the
     * worker, so clearing the global pointer here is enough to prevent later
     * accidental reuse during teardown.
     */
    g_bondp_worker = NULL;

    (void)pthread_mutex_lock(&worker->lock);
    worker->state = BONDP_WORKER_STATE_STOPPING;
    (void)pthread_mutex_unlock(&worker->lock);
    (void)bondp_worker_wakeup(worker);

    if (worker->state != BONDP_WORKER_STATE_INIT) {
        (void)pthread_join(worker->thread, NULL);
    }

    if (worker->wake_fd >= 0) {
        (void)close(worker->wake_fd);
    }
    if (worker->epoll_fd >= 0) {
        (void)close(worker->epoll_fd);
    }
    HMAP_FOR_EACH_SAFE(entry, next, hmap_node, &worker->handler_map)
    {
        bondp_worker_free_event_handler(worker, entry);
    }
    ub_hmap_destroy(&worker->handler_map);
    (void)pthread_mutex_destroy(&worker->lock);
    tw_destroy(worker->tw);
    free(worker);
    (void)pthread_mutex_unlock(&g_bondp_worker_lock);
}

int bondp_worker_schedule(uint64_t delay_ms, bondp_worker_task_fn_t fn, void *arg, bondp_worker_task_id_t *task_id)
{
    int ret;
    bondp_worker_t *worker;

    (void)pthread_mutex_lock(&g_bondp_worker_lock);
    worker = g_bondp_worker;
    if (worker == NULL) {
        ret = -ENODEV;
        goto out_unlock_global;
    }

    (void)pthread_mutex_lock(&worker->lock);
    ret = bondp_worker_get_status_locked(worker);
    (void)pthread_mutex_unlock(&worker->lock);
    if (ret != 0) {
        goto out_unlock_global;
    }

    ret = tw_schedule(worker->tw, delay_ms, fn, arg, task_id);
    if (ret == 0) {
        (void)bondp_worker_wakeup(worker);
    }

out_unlock_global:
    (void)pthread_mutex_unlock(&g_bondp_worker_lock);
    return ret;
}

int bondp_worker_cancel(bondp_worker_task_id_t task_id)
{
    int ret;
    bondp_worker_t *worker;

    (void)pthread_mutex_lock(&g_bondp_worker_lock);
    worker = g_bondp_worker;
    if (worker == NULL) {
        ret = -ENODEV;
        goto out_unlock_global;
    }

    (void)pthread_mutex_lock(&worker->lock);
    ret = bondp_worker_get_status_locked(worker);
    (void)pthread_mutex_unlock(&worker->lock);
    if (ret != 0) {
        goto out_unlock_global;
    }

    ret = tw_cancel(worker->tw, task_id);
    if (ret == 0) {
        (void)bondp_worker_wakeup(worker);
    }

out_unlock_global:
    (void)pthread_mutex_unlock(&g_bondp_worker_lock);
    return ret;
}

int bondp_worker_add_fd(int fd, bondp_worker_event_fn_t handler, void *arg)
{
    bondp_worker_t *worker;
    bondp_worker_event_handler_t *entry;
    struct epoll_event event = {
        .events = EPOLLIN,
        .data.fd = fd,
    };
    int ret = 0;

    if (fd < 0 || handler == NULL) {
        return -EINVAL;
    }

    entry = calloc(1, sizeof(*entry));
    if (entry == NULL) {
        URMA_LOG_ERR("Failed to alloc bond worker event handler.\n");
        return -ENOMEM;
    }

    entry->fd = fd;
    entry->handler = handler;
    entry->arg = arg;

    (void)pthread_mutex_lock(&g_bondp_worker_lock);
    worker = g_bondp_worker;
    if (worker == NULL) {
        ret = -ENODEV;
        goto out_unlock_global;
    }

    (void)pthread_mutex_lock(&worker->lock);
    ret = bondp_worker_get_status_locked(worker);
    if (ret != 0) {
        goto out_unlock_worker;
    }
    if (bondp_worker_lookup_event_handler(worker, fd) != NULL) {
        ret = -EEXIST;
        goto out_unlock_worker;
    }
    if (epoll_ctl(worker->epoll_fd, EPOLL_CTL_ADD, fd, &event) != 0) {
        URMA_LOG_ERR("Failed to add fd %d to bond worker epoll, errno: %d.\n", fd, errno);
        ret = -errno;
        goto out_unlock_worker;
    }
    ub_hmap_insert(&worker->handler_map, &entry->hmap_node, bondp_worker_event_hash(fd));

out_unlock_worker:
    (void)pthread_mutex_unlock(&worker->lock);
out_unlock_global:
    (void)pthread_mutex_unlock(&g_bondp_worker_lock);
    if (ret != 0) {
        free(entry);
        return ret;
    }

    (void)bondp_worker_wakeup(worker);
    return 0;
}

int bondp_worker_del_fd(int fd)
{
    bondp_worker_t *worker;
    bondp_worker_event_handler_t *entry;
    int ret = 0;

    if (fd < 0) {
        return -EINVAL;
    }

    (void)pthread_mutex_lock(&g_bondp_worker_lock);
    worker = g_bondp_worker;
    if (worker == NULL) {
        ret = -ENODEV;
        goto out_unlock_global;
    }

    (void)pthread_mutex_lock(&worker->lock);
    ret = bondp_worker_get_status_locked(worker);
    if (ret != 0) {
        goto out_unlock_worker;
    }
    entry = bondp_worker_lookup_event_handler(worker, fd);
    if (entry == NULL) {
        ret = -ENOENT;
        goto out_unlock_worker;
    }
    if (epoll_ctl(worker->epoll_fd, EPOLL_CTL_DEL, fd, NULL) != 0) {
        URMA_LOG_ERR("Failed to del fd %d from bond worker epoll, errno: %d.\n", fd, errno);
        ret = -errno;
        goto out_unlock_worker;
    }
    ub_hmap_remove(&worker->handler_map, &entry->hmap_node);
    entry->deleting = true;
    if (entry->refcnt == 0) {
        free(entry);
    }

out_unlock_worker:
    (void)pthread_mutex_unlock(&worker->lock);
out_unlock_global:
    (void)pthread_mutex_unlock(&g_bondp_worker_lock);
    if (ret != 0) {
        return ret;
    }

    (void)bondp_worker_wakeup(worker);
    return 0;
}
