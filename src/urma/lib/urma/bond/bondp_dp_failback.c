/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bond provider failback helpers.
 */

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bondp_context_table.h"
#include "bondp_datapath.h"
#include "bondp_types.h"
#include "bondp_worker.h"
#include "ub_hmap.h"
#include "ubagg_ioctl.h"
#include "urma_api.h"
#include "urma_log.h"
#include "urma_provider.h"
#include "urma_types.h"

#include "bondp_dp_failback.h"

typedef enum bondp_fb_task_type {
    /* Recreate a failed pjetty in place so the slot stays pre-built. */
    BONDP_FB_TASK_REBUILD = 0,
    /* Create the not-yet-built cross-IODIE pjettys on demand (lazy failover). */
    BONDP_FB_TASK_LAZY_CREATE,
    /* Destroy materialized cross-IODIE pjettys after failback. */
    BONDP_FB_TASK_LAZY_DESTROY,
} bondp_fb_task_type_t;

typedef struct bondp_fb_task_key {
    uint32_t vjetty_id;
    uint32_t pjetty_idx;
    uint32_t type;
} bondp_fb_task_key_t;

typedef struct bondp_fb_task {
    struct ub_hmap_node hmap_node;
    urma_ref_t use_cnt;
    bondp_context_t *bond_ctx;
    bondp_worker_task_id_t worker_task_id;
    uint32_t request_id;
    uint32_t vjetty_id;
    uint32_t pjetty_idx;
    bondp_fb_task_type_t type;
    /* Protected by fb_ctx->task_lock. Once set, the callback must not
     * transition this entry to another worker task. */
    bool canceling;
} bondp_fb_task_t;

struct bondp_fb_ctx {
    struct ub_hmap task_map;
    pthread_rwlock_t task_lock;
    /* Protected by task_lock. Rejects new tasks and task transitions while
     * context teardown drains the table. */
    bool shutting_down;
#ifndef __cplusplus
    atomic_uint request_id;
#else
    std::atomic_uint request_id;
#endif
};

bool bondp_fb_need_switch_path(bondp_comp_t *bdp_comp)
{
    if (!bdp_comp->bondp_ctx->enable_failback) {
        return false;
    }

    for (uint32_t i = 0; i < URMA_UBAGG_DEV_MAX_NUM; ++i) {
        if (atomic_load(&bdp_comp->hc_valid[i])) {
            return true;
        }
    }
    return false;
}

uint32_t bondp_fb_switch_path(bondp_comp_t *bdp_comp)
{
    uint32_t recovered_cnt = 0;

    for (uint32_t i = 0; i < URMA_UBAGG_DEV_MAX_NUM; ++i) {
        if (!atomic_exchange(&bdp_comp->hc_valid[i], false)) {
            continue;
        }
        recovered_cnt++;
        atomic_store(&bdp_comp->valid[i], true);
        URMA_LOG_INFO("Path restored: local jetty recovered, vjetty_id=%u, local_idx=%u\n",
                      bdp_comp->v_jetty.jetty_id.id, i);
    }
    return recovered_cnt;
}

#define BONDP_FB_TASK_HASH_BASIS    0x9d4f21U
#define BONDP_FB_TASK_TABLE_SIZE    1024U
/* Delay (ms) before rebuilding a failed-back pjetty on the bond worker. */
#define BONDP_FB_REBUILD_DELAY_MS   2000U
/* Delay (ms) between rebuild retries after a transient failure. */
#define BONDP_FB_REBUILD_RETRY_MS   1000U
/* Publish rebuild completion on the next worker tick to reject stale CRs. */
#define BONDP_FB_MARK_DONE_DELAY_MS 0U
/* Delay (ms) between lazy-create retries after a transient create failure. */
#define BONDP_LAZY_CREATE_RETRY_MS   1000U
/* Delay (ms) while cold-standby WRs drain before pjetty destruction. */
#define BONDP_LAZY_DESTROY_RETRY_MS  1000U

static uint32_t next_request_id(bondp_fb_ctx_t *fb_ctx)
{
    return atomic_fetch_add(&fb_ctx->request_id, 1) + 1;
}

static void init_request_id(bondp_fb_ctx_t *fb_ctx)
{
    struct timespec ts = {0};
    uint32_t seed;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        seed = (uint32_t)getpid();
    } else {
        seed = (uint32_t)ts.tv_nsec ^ (uint32_t)ts.tv_sec ^ (uint32_t)getpid();
    }

    atomic_store(&fb_ctx->request_id, seed);
}

static bool fb_task_matches_key(const struct ub_hmap_node *node, const bondp_fb_task_key_t *key)
{
    const bondp_fb_task_t *fb_task = CONTAINER_OF_FIELD(node, bondp_fb_task_t, hmap_node);

    return fb_task->vjetty_id == key->vjetty_id &&
           fb_task->pjetty_idx == key->pjetty_idx &&
           (uint32_t)fb_task->type == key->type;
}

static uint32_t fb_task_hash_key(const bondp_fb_task_key_t *key)
{
    return ub_hash_bytes(key, sizeof(bondp_fb_task_key_t), BONDP_FB_TASK_HASH_BASIS);
}

static bondp_fb_ctx_t *fb_task_table_create(void)
{
    bondp_fb_ctx_t *fb_ctx = calloc(1, sizeof(*fb_ctx));
    if (fb_ctx == NULL) {
        return NULL;
    }

    if (pthread_rwlock_init(&fb_ctx->task_lock, NULL) != 0) {
        free(fb_ctx);
        return NULL;
    }

    if (ub_hmap_init(&fb_ctx->task_map, BONDP_FB_TASK_TABLE_SIZE) != 0) {
        (void)pthread_rwlock_destroy(&fb_ctx->task_lock);
        free(fb_ctx);
        return NULL;
    }

    return fb_ctx;
}

static void fb_task_free(bondp_fb_task_t *fb_task)
{
    free(fb_task);
}

static void fb_task_put(bondp_fb_task_t *fb_task)
{
    if (atomic_fetch_sub(&fb_task->use_cnt.atomic_cnt, 1) == 1) {
        fb_task_free(fb_task);
    }
}

static bondp_fb_task_t *fb_task_get(bondp_fb_ctx_t *fb_ctx, const bondp_fb_task_key_t *key)
{
    uint32_t hash = fb_task_hash_key(key);
    bondp_fb_task_t *fb_task = NULL;

    (void)pthread_rwlock_rdlock(&fb_ctx->task_lock);
    HMAP_FOR_EACH_WITH_HASH (fb_task, hmap_node, hash, &fb_ctx->task_map) {
        if (!fb_task_matches_key(&fb_task->hmap_node, key)) {
            continue;
        }
        atomic_fetch_add(&fb_task->use_cnt.atomic_cnt, 1);
        (void)pthread_rwlock_unlock(&fb_ctx->task_lock);
        return fb_task;
    }
    (void)pthread_rwlock_unlock(&fb_ctx->task_lock);
    return NULL;
}

static int fb_task_schedule_next(bondp_fb_ctx_t *fb_ctx, bondp_fb_task_t *fb_task,
                                 uint64_t delay_ms, bondp_worker_task_fn_t fn, void *arg)
{
    bondp_worker_task_id_t worker_task_id = 0;

    (void)pthread_rwlock_wrlock(&fb_ctx->task_lock);
    if (fb_ctx->shutting_down || fb_task->canceling) {
        (void)pthread_rwlock_unlock(&fb_ctx->task_lock);
        return -ECANCELED;
    }

    int ret = bondp_worker_schedule(delay_ms, fn, arg, &worker_task_id);
    if (ret == 0) {
        fb_task->worker_task_id = worker_task_id;
    }
    (void)pthread_rwlock_unlock(&fb_ctx->task_lock);
    return ret;
}

static int fb_task_del(bondp_fb_ctx_t *fb_ctx, const bondp_fb_task_key_t *key);

static int fb_task_table_destroy(bondp_fb_ctx_t *fb_ctx)
{
    bondp_fb_task_t *fb_task = NULL;

    /* Serialize the shutdown flag with every task transition. An executing
     * callback either publishes its next task before this lock is acquired
     * (and we cancel that exact ID), or observes shutting_down/canceling and
     * deletes its table entry without requeueing. */
    (void)pthread_rwlock_wrlock(&fb_ctx->task_lock);
    fb_ctx->shutting_down = true;
    (void)pthread_rwlock_unlock(&fb_ctx->task_lock);

    while (true) {
        bondp_worker_task_id_t task_id = 0;
        uint32_t task_count;

        (void)pthread_rwlock_wrlock(&fb_ctx->task_lock);
        task_count = ub_hmap_count(&fb_ctx->task_map);
        fb_task = NULL;
        HMAP_FOR_EACH (fb_task, hmap_node, &fb_ctx->task_map) {
            fb_task->canceling = true;
            task_id = fb_task->worker_task_id;
            break;
        }
        (void)pthread_rwlock_unlock(&fb_ctx->task_lock);

        if (fb_task == NULL) {
            break;
        }
        fb_task = NULL;
        if (task_id == 0) {
            URMA_LOG_ERR("Failback task has no worker task ID during teardown.\n");
            return -EIO;
        }

        int ret = bondp_worker_cancel_batch(&task_id, 1);
        if (ret != 0) {
            /* Do not release the table or its lock: a queued callback still
             * owns fb_ctx and would otherwise dereference freed memory. */
            URMA_LOG_ERR("Failed to cancel failback task during teardown, ret=%d.\n", ret);
            return ret;
        }

        (void)pthread_rwlock_rdlock(&fb_ctx->task_lock);
        uint32_t remaining = ub_hmap_count(&fb_ctx->task_map);
        (void)pthread_rwlock_unlock(&fb_ctx->task_lock);
        if (remaining >= task_count) {
            URMA_LOG_ERR("Failback task table did not drain after cancellation, before=%u, after=%u.\n",
                         task_count, remaining);
            return -EIO;
        }
    }

    (void)pthread_rwlock_wrlock(&fb_ctx->task_lock);
    ub_hmap_destroy(&fb_ctx->task_map);
    (void)pthread_rwlock_unlock(&fb_ctx->task_lock);
    (void)pthread_rwlock_destroy(&fb_ctx->task_lock);
    free(fb_ctx);
    return 0;
}

static int fb_task_del(bondp_fb_ctx_t *fb_ctx, const bondp_fb_task_key_t *key)
{
    uint32_t hash = fb_task_hash_key(key);
    bondp_fb_task_t *fb_task = NULL;

    (void)pthread_rwlock_wrlock(&fb_ctx->task_lock);
    HMAP_FOR_EACH_WITH_HASH (fb_task, hmap_node, hash, &fb_ctx->task_map) {
        if (!fb_task_matches_key(&fb_task->hmap_node, key)) {
            continue;
        }

        ub_hmap_remove(&fb_ctx->task_map, &fb_task->hmap_node);
        (void)pthread_rwlock_unlock(&fb_ctx->task_lock);
        fb_task_put(fb_task);
        return 0;
    }
    (void)pthread_rwlock_unlock(&fb_ctx->task_lock);
    return -ENOENT;
}

static int bondp_update_pjetty_id_mapping(
    bondp_context_t *bdp_ctx, urma_jetty_id_t old_id,
    urma_jetty_id_t new_id, bondp_comp_t *bdp_jetty)
{
    int ret = 0;

    pthread_rwlock_wrlock(&bdp_ctx->p_vjetty_id_table.lock);
    ret = bdp_p_vjetty_id_table_add_without_lock(
        &bdp_ctx->p_vjetty_id_table, new_id, JETTY, bdp_jetty->v_jetty.jetty_id.id, bdp_jetty);
    if (ret != 0) {
        pthread_rwlock_unlock(&bdp_ctx->p_vjetty_id_table.lock);
        URMA_LOG_ERR("Failed to add recreated pjetty id mapping: " URMA_JETTY_ID_FMT ", ret=%d\n",
                     URMA_JETTY_ID_ARGS(&new_id), ret);
        return -1;
    }
    ret = bdp_p_vjetty_id_table_del_without_lock(&bdp_ctx->p_vjetty_id_table, old_id, JETTY);
    if (ret != 0) {
        (void)bdp_p_vjetty_id_table_del_without_lock(&bdp_ctx->p_vjetty_id_table, new_id, JETTY);
        pthread_rwlock_unlock(&bdp_ctx->p_vjetty_id_table.lock);
        URMA_LOG_ERR("Failed to delete stale pjetty id mapping: " URMA_JETTY_ID_FMT ", ret=%d\n",
                     URMA_JETTY_ID_ARGS(&old_id), ret);
        return -1;
    }
    pthread_rwlock_unlock(&bdp_ctx->p_vjetty_id_table.lock);
    return 0;
}

static bool bondp_is_cold_lazy_idx(const bondp_comp_t *bdp_jetty, uint32_t local_idx)
{
    if (!bondp_comp_uses_lazy_pjetty(bdp_jetty) || bdp_jetty->active_count == 0) {
        return false;
    }

    uint32_t primary_iodie = bondp_iodie_idx_of(bdp_jetty->active_indices[0]);
    uint32_t local_iodie = bondp_iodie_idx_of(local_idx);
    return primary_iodie != UINT32_MAX && local_iodie != UINT32_MAX &&
           local_iodie != primary_iodie;
}

static bool bondp_primary_iodie_alive(const bondp_comp_t *bdp_jetty)
{
    uint32_t primary_iodie = bondp_iodie_idx_of(bdp_jetty->active_indices[0]);
    if (primary_iodie == UINT32_MAX) {
        return false;
    }

    for (uint32_t n = 0; n < bdp_jetty->enabled_count; ++n) {
        uint32_t idx = bdp_jetty->enabled_indices[n];
        if (bondp_iodie_idx_of(idx) == primary_iodie && atomic_load(&bdp_jetty->valid[idx])) {
            return true;
        }
    }
    return false;
}

static bool bondp_lazy_pjetty_has_inflight(const bondp_comp_t *bdp_jetty, uint32_t local_idx)
{
    for (uint32_t target_idx = 0; target_idx < URMA_UBAGG_DEV_MAX_NUM; ++target_idx) {
        if (atomic_load(&bdp_jetty->sqe_cnt[local_idx][target_idx]) != 0) {
            return true;
        }
    }
    return false;
}

static void bondp_clear_lazy_pjetty_slot(bondp_comp_t *bdp_jetty, uint32_t local_idx)
{
    bdp_jetty->p_jetty[local_idx] = NULL;
    atomic_store(&bdp_jetty->valid[local_idx], false);
    atomic_store(&bdp_jetty->hc_valid[local_idx], false);
    atomic_store(&bdp_jetty->rebuild_done[local_idx], false);
}

static int bondp_detach_lazy_pjetty(bondp_comp_t *bdp_jetty, uint32_t local_idx,
                                    urma_jetty_t **old_jetty)
{
    *old_jetty = bdp_jetty->p_jetty[local_idx];
    if (*old_jetty == NULL) {
        bondp_clear_lazy_pjetty_slot(bdp_jetty, local_idx);
        return 0;
    }

    bondp_context_t *bdp_ctx = bdp_jetty->bondp_ctx;
    pthread_rwlock_wrlock(&bdp_ctx->p_vjetty_id_table.lock);
    int ret = bdp_p_vjetty_id_table_del_without_lock(
        &bdp_ctx->p_vjetty_id_table, (*old_jetty)->jetty_id, JETTY);
    pthread_rwlock_unlock(&bdp_ctx->p_vjetty_id_table.lock);
    if (ret != 0 && ret != BONDP_HASH_MAP_NOT_FOUND_ERROR) {
        *old_jetty = NULL;
        return -EAGAIN;
    }
    bondp_clear_lazy_pjetty_slot(bdp_jetty, local_idx);
    return 0;
}

static void bondp_restore_lazy_pjetty(bondp_comp_t *bdp_jetty, uint32_t local_idx,
                                      urma_jetty_t *old_jetty)
{
    (void)pthread_spin_lock(&bdp_jetty->send_lock);
    bdp_jetty->p_jetty[local_idx] = old_jetty;
    pthread_rwlock_wrlock(&bdp_jetty->bondp_ctx->p_vjetty_id_table.lock);
    int ret = bdp_p_vjetty_id_table_add_without_lock(
        &bdp_jetty->bondp_ctx->p_vjetty_id_table, old_jetty->jetty_id, JETTY,
        bdp_jetty->v_jetty.jetty_id.id, bdp_jetty);
    pthread_rwlock_unlock(&bdp_jetty->bondp_ctx->p_vjetty_id_table.lock);
    (void)pthread_spin_unlock(&bdp_jetty->send_lock);
    if (ret != 0 && ret != BONDP_HASH_MAP_COLLIDE_ERROR) {
        URMA_LOG_WARN("Failed to restore cold-standby pjetty mapping, "
                      "vjetty_id=%u, idx=%u, ret=%d\n",
                      bdp_jetty->v_jetty.jetty_id.id, local_idx, ret);
    }
}

static int bondp_destroy_lazy_pjetty(bondp_comp_t *bdp_jetty, uint32_t local_idx)
{
    if (bdp_jetty == NULL || bdp_jetty->bondp_ctx == NULL ||
        local_idx >= URMA_UBAGG_DEV_MAX_NUM || !bondp_is_cold_lazy_idx(bdp_jetty, local_idx)) {
        return -EINVAL;
    }

    (void)pthread_spin_lock(&bdp_jetty->send_lock);
    if (atomic_load(&bdp_jetty->modify_to_error) || atomic_load(&bdp_jetty->deleting)) {
        (void)pthread_spin_unlock(&bdp_jetty->send_lock);
        return -ECANCELED;
    }
    bool primary_alive = bondp_primary_iodie_alive(bdp_jetty);
    bool has_inflight = bondp_lazy_pjetty_has_inflight(bdp_jetty, local_idx);
    if (!primary_alive || has_inflight) {
        if (has_inflight) {
            /* The task retries every BONDP_LAZY_DESTROY_RETRY_MS, so keep the
             * snapshot rate-limited: it answers whether the drain check is
             * seeing the real outstanding WR count or a stale one. */
            char inflight[256];
            size_t off = 0;
            for (uint32_t t = 0; t < URMA_UBAGG_DEV_MAX_NUM && off + 1 < sizeof(inflight); ++t) {
                uint32_t cnt = atomic_load(&bdp_jetty->sqe_cnt[local_idx][t]);
                if (cnt == 0) {
                    continue;
                }
                int n = snprintf(inflight + off, sizeof(inflight) - off, "%s[%u]=%u",
                                 off != 0 ? " " : "", t, cnt);
                if (n < 0) {
                    break;
                }
                if ((size_t)n >= sizeof(inflight) - off) {
                    off = sizeof(inflight) - 1;
                    break;
                }
                off += (size_t)n;
            }
            inflight[off] = '\0';
            URMA_LOG_WARN_RL("Cold-standby destroy deferred, wrs still in flight, vjetty_id=%u, "
                             "idx=%u, sqe_cnt(target=count):%s\n",
                             bdp_jetty->v_jetty.jetty_id.id, local_idx, inflight);
        } else {
            URMA_LOG_INFO("Cold-standby destroy deferred, primary iodie not alive, "
                          "vjetty_id=%u, idx=%u\n",
                          bdp_jetty->v_jetty.jetty_id.id, local_idx);
        }
        (void)pthread_spin_unlock(&bdp_jetty->send_lock);
        return -EAGAIN;
    }
    urma_jetty_t *old_jetty = NULL;
    int ret = bondp_detach_lazy_pjetty(bdp_jetty, local_idx, &old_jetty);
    (void)pthread_spin_unlock(&bdp_jetty->send_lock);
    if (ret != 0 || old_jetty == NULL) {
        return ret;
    }

    urma_jetty_id_t old_id = old_jetty->jetty_id;
    ret = urma_delete_jetty(old_jetty);
    if (ret != URMA_SUCCESS) {
        URMA_LOG_WARN("Failed to delete cold-standby pjetty, vjetty_id=%u, idx=%u, ret=%d\n",
                      bdp_jetty->v_jetty.jetty_id.id, local_idx, ret);
        bondp_restore_lazy_pjetty(bdp_jetty, local_idx, old_jetty);
        return -EAGAIN;
    }
    URMA_LOG_INFO("Cold-standby pjetty destroyed after failback, vjetty_id=%u, idx=%u, jetty_id=%u\n",
                  bdp_jetty->v_jetty.jetty_id.id, local_idx, old_id.id);
    return 0;
}

static int bondp_rebuild_pjetty(bondp_comp_t *bdp_jetty, uint32_t local_idx)
{
    int ret;

    if (bdp_jetty == NULL || bdp_jetty->bondp_ctx == NULL ||
        local_idx >= URMA_UBAGG_DEV_MAX_NUM) {
        return -1;
    }

    bondp_context_t *bdp_ctx = bdp_jetty->bondp_ctx;

    (void)pthread_spin_lock(&bdp_jetty->send_lock);
    if (atomic_load(&bdp_jetty->modify_to_error) || atomic_load(&bdp_jetty->deleting)) {
        (void)pthread_spin_unlock(&bdp_jetty->send_lock);
        return -ECANCELED;
    }
    urma_jetty_t *old_jetty = bdp_jetty->p_jetty[local_idx];
    if (old_jetty == NULL) {
        (void)pthread_spin_unlock(&bdp_jetty->send_lock);
        if (bondp_is_cold_lazy_idx(bdp_jetty, local_idx)) {
            return -ECANCELED;
        }
        URMA_LOG_ERR("pjetty at idx=%u is NULL, cannot rebuild\n", local_idx);
        return -1;
    }
    urma_jetty_cfg_t p_cfg = old_jetty->jetty_cfg;
    urma_jetty_id_t old_id = old_jetty->jetty_id;
    (void)pthread_spin_unlock(&bdp_jetty->send_lock);

    urma_jetty_t *new_jetty = urma_create_jetty(bdp_ctx->p_ctxs[local_idx], &p_cfg);
    if (new_jetty == NULL) {
        URMA_LOG_ERR("Failed to recreate pjetty at idx=%d\n", local_idx);
        return -1;
    }

    new_jetty->remote_jetty = NULL;
    new_jetty->jetty_cfg.user_ctx = (uint64_t)bdp_jetty;

    (void)pthread_spin_lock(&bdp_jetty->send_lock);
    if (atomic_load(&bdp_jetty->modify_to_error) || atomic_load(&bdp_jetty->deleting) ||
        bdp_jetty->p_jetty[local_idx] != old_jetty) {
        (void)pthread_spin_unlock(&bdp_jetty->send_lock);
        (void)urma_delete_jetty(new_jetty);
        return -ECANCELED;
    }
    ret = bondp_update_pjetty_id_mapping(bdp_ctx, old_id, new_jetty->jetty_id, bdp_jetty);
    if (ret != 0) {
        (void)pthread_spin_unlock(&bdp_jetty->send_lock);
        (void)urma_delete_jetty(new_jetty);
        return -1;
    }
    bdp_jetty->p_jetty[local_idx] = new_jetty;
    (void)pthread_spin_unlock(&bdp_jetty->send_lock);

    ret = urma_delete_jetty(old_jetty);
    if (ret != URMA_SUCCESS) {
        URMA_LOG_WARN("Failed to delete old pjetty at idx=%d\n", local_idx);
    }

    URMA_LOG_INFO("Failback pjetty rebuilt, idx=%d old=" URMA_JETTY_ID_FMT " new=" URMA_JETTY_ID_FMT "\n",
                  local_idx, URMA_JETTY_ID_ARGS(&old_id), URMA_JETTY_ID_ARGS(&new_jetty->jetty_id));
    return 0;
}

/*
 * Ref-taking lookup for worker tasks: a task may run urma_create_jetty (a
 * multi-ms ioctl) while a concurrent bondp_delete_jetty frees the comp. Take
 * a use_cnt reference under the table lock and refuse comps already deleting,
 * so the delete path observes use_cnt > 0 and returns URMA_EAGAIN instead of
 * removing mappings / freeing the comp. Callers must drop the reference
 * (use_cnt--) on EVERY exit after a successful lookup.
 */
static bondp_comp_t *bondp_get_jetty_by_vjetty_id_ref(bondp_context_t *bond_ctx, uint32_t vjetty_id)
{
    bondp_comp_t *bdp_jetty = NULL;

    if (bond_ctx == NULL) {
        return NULL;
    }

    pthread_rwlock_rdlock(&bond_ctx->p_vjetty_id_table.lock);
    bdp_p_vjetty_id_t *item = NULL;
    HMAP_FOR_EACH (item, hmap_node, &bond_ctx->p_vjetty_id_table.hmap) {
        if (item->key.type != JETTY || item->vjetty_id != vjetty_id) {
            continue;
        }
        if (atomic_load(&item->comp->deleting)) {
            break; /* being deleted: do not touch */
        }
        atomic_fetch_add(&item->comp->use_cnt.atomic_cnt, 1);
        bdp_jetty = item->comp;
        break;
    }
    pthread_rwlock_unlock(&bond_ctx->p_vjetty_id_table.lock);
    return bdp_jetty;
}

static inline void bondp_put_jetty_ref(bondp_comp_t *bdp_jetty)
{
    if (bdp_jetty != NULL) {
        atomic_fetch_sub(&bdp_jetty->use_cnt.atomic_cnt, 1);
    }
}

typedef struct bondp_fb_async_arg {
    bondp_fb_ctx_t *fb_ctx;
    bondp_fb_task_key_t key;
    bondp_fb_task_type_t type;
    /* Retry counter for rebuild/create/destroy retries. */
    uint32_t retry_cnt;
    /* Payload for BONDP_FB_TASK_LAZY_CREATE. */
    bondp_lazy_create_info_t lazy;
} bondp_fb_async_arg_t;

static int fb_add_task_common(bondp_context_t *bond_ctx, bondp_fb_task_type_t type,
                              uint64_t delay_ms, bondp_fb_async_arg_t *async_arg);

typedef struct bondp_fb_mark_done_arg {
    bondp_fb_ctx_t *fb_ctx;
    bondp_fb_task_key_t key;
    bondp_fb_task_t *fb_task;
    bondp_context_t *bond_ctx;
    uint32_t vjetty_id;
    uint32_t pjetty_idx;
} bondp_fb_mark_done_arg_t;

static void bondp_mark_rebuild_done_async(bondp_worker_task_reason_t reason, void *arg)
{
    bondp_fb_mark_done_arg_t *mark_arg = arg;

    if (mark_arg == NULL) {
        return;
    }
    if (reason == BONDP_WORKER_TASK_EXECUTED) {
        bondp_comp_t *bdp_jetty = bondp_get_jetty_by_vjetty_id_ref(
            mark_arg->bond_ctx, mark_arg->vjetty_id);
        if (bdp_jetty != NULL) {
            atomic_store(&bdp_jetty->rebuild_done[mark_arg->pjetty_idx], true);
            URMA_LOG_INFO("Failback pjetty ready for health-check recovery, "
                          "vjetty_id=%u, idx=%u\n",
                          mark_arg->vjetty_id, mark_arg->pjetty_idx);
            bondp_put_jetty_ref(bdp_jetty);
        }
    }

    (void)fb_task_del(mark_arg->fb_ctx, &mark_arg->key);
    fb_task_put(mark_arg->fb_task);
    free(mark_arg);
}

static int bondp_schedule_mark_rebuild_done(bondp_fb_ctx_t *fb_ctx, bondp_fb_task_t *fb_task,
                                            const bondp_fb_task_key_t *key)
{
    bondp_fb_mark_done_arg_t *mark_arg = calloc(1, sizeof(*mark_arg));
    if (mark_arg == NULL) {
        return -ENOMEM;
    }
    mark_arg->fb_ctx = fb_ctx;
    mark_arg->key = *key;
    mark_arg->fb_task = fb_task;
    mark_arg->bond_ctx = fb_task->bond_ctx;
    mark_arg->vjetty_id = fb_task->vjetty_id;
    mark_arg->pjetty_idx = fb_task->pjetty_idx;

    int ret = fb_task_schedule_next(fb_ctx, fb_task, BONDP_FB_MARK_DONE_DELAY_MS,
                                     bondp_mark_rebuild_done_async, mark_arg);
    if (ret != 0) {
        free(mark_arg);
    }
    return ret;
}

static void bondp_fb_task_async(bondp_worker_task_reason_t reason, void *arg)
{
    bondp_fb_async_arg_t *arg_typed = (bondp_fb_async_arg_t *)arg;
    if (arg_typed == NULL) {
        return;
    }
    if (reason == BONDP_WORKER_TASK_CANCELED) {
        if (arg_typed->fb_ctx != NULL) {
            if (arg_typed->type == BONDP_FB_TASK_LAZY_CREATE) {
                bondp_fb_task_t *task = fb_task_get(arg_typed->fb_ctx, &arg_typed->key);
                if (task != NULL) {
                    bondp_comp_t *comp = bondp_get_jetty_by_vjetty_id_ref(task->bond_ctx,
                                                                         task->vjetty_id);
                    if (comp != NULL) {
                        atomic_store(&comp->lazy_create_pending, false);
                        bondp_put_jetty_ref(comp);
                    }
                    fb_task_put(task);
                }
            }
            (void)fb_task_del(arg_typed->fb_ctx, &arg_typed->key);
        }
        goto free_arg;
    }

    bondp_fb_ctx_t *fb_ctx = arg_typed->fb_ctx;
    if (fb_ctx == NULL) {
        goto free_arg;
    }

    bondp_fb_task_t *fb_task = fb_task_get(fb_ctx, &arg_typed->key);
    if (fb_task == NULL) {
        goto free_arg;
    }
    bondp_comp_t *bdp_jetty = bondp_get_jetty_by_vjetty_id_ref(fb_task->bond_ctx, fb_task->vjetty_id);
    if (bdp_jetty == NULL) {
        URMA_LOG_ERR("Failed to run fb task, vjetty %u not found (destroyed?), pjetty_idx=%u.\n",
                     fb_task->vjetty_id, fb_task->pjetty_idx);
        goto del_task;
    }

    if (fb_task->type == BONDP_FB_TASK_LAZY_CREATE) {
        /* urma_create_jetty is heavy (ioctl + HW alloc): worker thread only,
         * never in the poll thread spinlock. */
        int ret = bondp_lazy_create_and_resend(bdp_jetty, fb_task->pjetty_idx, &arg_typed->lazy);
        if (ret != 0 && ret != -ECANCELED) {
            /* The triggering CR has already been consumed, so there is no
             * later event guaranteed to rescue its cached WR. Keep a single
             * low-frequency task alive until create succeeds or ERROR/delete
             * cancels it; never silently abandon the WR after a retry cap. */
            if (arg_typed->retry_cnt < UINT32_MAX) {
                arg_typed->retry_cnt++;
            }
            int rret = fb_task_schedule_next(fb_ctx, fb_task, BONDP_LAZY_CREATE_RETRY_MS,
                                              bondp_fb_task_async, arg_typed);
            if (rret == 0) {
                if (arg_typed->retry_cnt == 1 || arg_typed->retry_cnt % 30 == 0) {
                    URMA_LOG_WARN("Lazy create still failing, retry %u scheduled, vjetty_id=%u, "
                                  "pjetty_idx=%u, ret=%d\n", arg_typed->retry_cnt,
                                  fb_task->vjetty_id, fb_task->pjetty_idx, ret);
                }
                fb_task_put(fb_task);
                bondp_put_jetty_ref(bdp_jetty);
                return;
            }
            if (rret == -ECANCELED) {
                ret = rret;
            } else {
                ret = rret;
                URMA_LOG_ERR("Failed to schedule lazy create retry, vjetty_id=%u, "
                             "pjetty_idx=%u, ret=%d\n",
                             fb_task->vjetty_id, fb_task->pjetty_idx, ret);
            }
        }
        /* Close only a canceled task or a task that cannot be rescheduled. */
        atomic_store(&bdp_jetty->lazy_create_pending, false);
        if (ret != 0) {
            URMA_LOG_ERR("Lazy create task failed, vjetty_id=%u, pjetty_idx=%u, ret=%d\n",
                         fb_task->vjetty_id, fb_task->pjetty_idx, ret);
        }
        goto del_task;
    }

    if (fb_task->type == BONDP_FB_TASK_LAZY_DESTROY) {
        int ret = bondp_destroy_lazy_pjetty(bdp_jetty, fb_task->pjetty_idx);
        if (ret == -EAGAIN) {
            if (arg_typed->retry_cnt < UINT32_MAX) {
                arg_typed->retry_cnt++;
            }
            int rret = fb_task_schedule_next(fb_ctx, fb_task, BONDP_LAZY_DESTROY_RETRY_MS,
                                              bondp_fb_task_async, arg_typed);
            if (rret == 0) {
                fb_task_put(fb_task);
                bondp_put_jetty_ref(bdp_jetty);
                return;
            }
            ret = rret;
        }
        if (ret != 0 && ret != -ECANCELED) {
            URMA_LOG_ERR("Failed to destroy cold-standby pjetty, vjetty_id=%u, "
                         "pjetty_idx=%u, ret=%d\n",
                         fb_task->vjetty_id, fb_task->pjetty_idx, ret);
        }
        goto del_task;
    }

    int ret = bondp_rebuild_pjetty(bdp_jetty, fb_task->pjetty_idx);
    if (ret != 0 && ret != -ECANCELED) {
        if (arg_typed->retry_cnt < UINT32_MAX) {
            arg_typed->retry_cnt++;
        }
        int rret = fb_task_schedule_next(fb_ctx, fb_task, BONDP_FB_REBUILD_RETRY_MS,
                                          bondp_fb_task_async, arg_typed);
        if (rret == 0) {
            if (arg_typed->retry_cnt == 1 || arg_typed->retry_cnt % 30 == 0) {
                URMA_LOG_WARN("Failback rebuild still failing, retry %u scheduled, "
                              "vjetty_id=%u, pjetty_idx=%u, ret=%d\n",
                              arg_typed->retry_cnt, fb_task->vjetty_id,
                              fb_task->pjetty_idx, ret);
            }
            fb_task_put(fb_task);
            bondp_put_jetty_ref(bdp_jetty);
            return;
        }
        ret = rret;
    }
    if (ret == 0) {
        ret = bondp_schedule_mark_rebuild_done(fb_ctx, fb_task, &arg_typed->key);
        if (ret == 0) {
            bondp_put_jetty_ref(bdp_jetty);
            free(arg_typed);
            return;
        }
        if (ret != -ECANCELED) {
            atomic_store(&bdp_jetty->rebuild_done[fb_task->pjetty_idx], true);
            URMA_LOG_WARN("Failed to defer rebuild completion; publish recovery directly, "
                          "vjetty_id=%u, pjetty_idx=%u, ret=%d\n",
                          fb_task->vjetty_id, fb_task->pjetty_idx, ret);
        }
    } else if (ret != -ECANCELED) {
        URMA_LOG_ERR("Failed to rebuild failback pjetty, vjetty_id=%u, pjetty_idx=%u, ret=%d\n",
                     fb_task->vjetty_id, fb_task->pjetty_idx, ret);
    }
    goto del_task;

del_task:
    bondp_put_jetty_ref(bdp_jetty);
    (void)fb_task_del(fb_ctx, &arg_typed->key);
    fb_task_put(fb_task);
free_arg:
    free(arg_typed);
    return;
}

static int fb_add_task_common(bondp_context_t *bond_ctx, bondp_fb_task_type_t type,
                              uint64_t delay_ms, bondp_fb_async_arg_t *async_arg)
{
    bondp_fb_ctx_t *fb_ctx = bond_ctx->fb_ctx;
    async_arg->key.type = (uint32_t)type;
    bondp_fb_task_t *fb_task = calloc(1, sizeof(*fb_task));
    if (fb_task == NULL) {
        return -ENOMEM;
    }

    fb_task->bond_ctx = bond_ctx;
    fb_task->request_id = next_request_id(fb_ctx);
    fb_task->vjetty_id = async_arg->key.vjetty_id;
    fb_task->pjetty_idx = async_arg->key.pjetty_idx;
    fb_task->type = type;
    atomic_init(&fb_task->use_cnt.atomic_cnt, 1);

    bondp_fb_task_key_t key = async_arg->key;
    uint32_t hash = fb_task_hash_key(&key);

    bondp_fb_task_t *existing_task = NULL;
    int ret = 0;

    (void)pthread_rwlock_wrlock(&fb_ctx->task_lock);
    if (fb_ctx->shutting_down) {
        ret = -ECANCELED;
        goto out_unlock;
    }
    HMAP_FOR_EACH_WITH_HASH (existing_task, hmap_node, hash, &fb_ctx->task_map) {
        if (fb_task_matches_key(&existing_task->hmap_node, &key)) {
            ret = -EEXIST;
            goto out_unlock;
        }
    }

    ub_hmap_insert(&fb_ctx->task_map, &fb_task->hmap_node, hash);
    ret = bondp_worker_schedule(delay_ms, bondp_fb_task_async, async_arg,
                                &fb_task->worker_task_id);
    if (ret != 0) {
        ub_hmap_remove(&fb_ctx->task_map, &fb_task->hmap_node);
        URMA_LOG_ERR("Failed to schedule fb task (type=%d), vjetty_id=%u, pjetty_idx=%u, ret=%d.\n",
                     type, key.vjetty_id, key.pjetty_idx, ret);
    }

out_unlock:
    (void)pthread_rwlock_unlock(&fb_ctx->task_lock);
    if (ret != 0) {
        fb_task_put(fb_task);
        return ret;
    }

    return 0;
}

int bondp_fb_add_task(bondp_context_t *bond_ctx, uint32_t vjetty_id, uint32_t pjetty_idx)
{
    if (bond_ctx == NULL || bond_ctx->fb_ctx == NULL) {
        return -EINVAL;
    }

    bondp_fb_async_arg_t *async_arg = calloc(1, sizeof(*async_arg));
    if (async_arg == NULL) {
        return -ENOMEM;
    }
    async_arg->fb_ctx = bond_ctx->fb_ctx;
    async_arg->key.vjetty_id = vjetty_id;
    async_arg->key.pjetty_idx = pjetty_idx;
    async_arg->type = BONDP_FB_TASK_REBUILD;

    int ret = fb_add_task_common(bond_ctx, BONDP_FB_TASK_REBUILD, BONDP_FB_REBUILD_DELAY_MS, async_arg);
    if (ret != 0) {
        /* Ownership of async_arg only transfers to the worker task on success. */
        free(async_arg);
    }
    return ret;
}

int bondp_fb_add_lazy_task(bondp_context_t *bond_ctx, uint32_t vjetty_id, uint32_t pjetty_idx,
                           const bondp_lazy_create_info_t *info)
{
    if (bond_ctx == NULL || bond_ctx->fb_ctx == NULL || info == NULL) {
        return -EINVAL;
    }

    bondp_fb_async_arg_t *async_arg = calloc(1, sizeof(*async_arg));
    if (async_arg == NULL) {
        return -ENOMEM;
    }
    async_arg->fb_ctx = bond_ctx->fb_ctx;
    async_arg->key.vjetty_id = vjetty_id;
    async_arg->key.pjetty_idx = pjetty_idx;
    async_arg->type = BONDP_FB_TASK_LAZY_CREATE;
    async_arg->lazy = *info;

    /* delay 0: the cross-IODIE failover is waiting for this pjetty, create it
     * as soon as the worker picks the task up. */
    int ret = fb_add_task_common(bond_ctx, BONDP_FB_TASK_LAZY_CREATE, 0, async_arg);
    if (ret != 0) {
        /* Ownership of async_arg only transfers to the worker task on success. */
        free(async_arg);
    }
    return ret;
}

static int bondp_fb_add_lazy_destroy_task(bondp_context_t *bond_ctx, uint32_t vjetty_id,
                                           uint32_t pjetty_idx)
{
    bondp_fb_async_arg_t *async_arg = calloc(1, sizeof(*async_arg));
    if (async_arg == NULL) {
        return -ENOMEM;
    }
    async_arg->fb_ctx = bond_ctx->fb_ctx;
    async_arg->key.vjetty_id = vjetty_id;
    async_arg->key.pjetty_idx = pjetty_idx;
    async_arg->type = BONDP_FB_TASK_LAZY_DESTROY;

    int ret = fb_add_task_common(bond_ctx, BONDP_FB_TASK_LAZY_DESTROY, 0, async_arg);
    if (ret != 0) {
        free(async_arg);
    }
    return ret;
}

void bondp_fb_schedule_lazy_destroy(bondp_comp_t *bdp_comp)
{
    if (bdp_comp == NULL || bdp_comp->bondp_ctx == NULL ||
        bdp_comp->bondp_ctx->fb_ctx == NULL || !bondp_comp_uses_lazy_pjetty(bdp_comp) ||
        bdp_comp->active_count == 0 || !bondp_primary_iodie_alive(bdp_comp)) {
        return;
    }

    uint32_t primary_iodie = bondp_iodie_idx_of(bdp_comp->active_indices[0]);
    for (uint32_t n = 0; n < bdp_comp->enabled_count; ++n) {
        uint32_t idx = bdp_comp->enabled_indices[n];
        if (bondp_iodie_idx_of(idx) == primary_iodie) {
            continue;
        }
        int ret = bondp_fb_add_lazy_destroy_task(bdp_comp->bondp_ctx,
                                                  bdp_comp->v_jetty.jetty_id.id, idx);
        if (ret == 0) {
            URMA_LOG_INFO("Cold-standby destroy scheduled after failback, vjetty_id=%u, idx=%u\n",
                          bdp_comp->v_jetty.jetty_id.id, idx);
        } else if (ret != -EEXIST) {
            URMA_LOG_WARN("Failed to schedule cold-standby destroy, vjetty_id=%u, idx=%u, ret=%d\n",
                          bdp_comp->v_jetty.jetty_id.id, idx, ret);
        }
    }
}

int bondp_fb_cancel_tasks(bondp_context_t *bond_ctx, uint32_t vjetty_id)
{
    bondp_fb_ctx_t *fb_ctx;
    bondp_fb_task_t *fb_task = NULL;
    bondp_worker_task_id_t task_ids[URMA_UBAGG_DEV_MAX_NUM];

    if (bond_ctx == NULL || bond_ctx->fb_ctx == NULL) {
        return 0;
    }
    fb_ctx = bond_ctx->fb_ctx;

    while (true) {
        size_t task_num = 0;
        (void)pthread_rwlock_wrlock(&fb_ctx->task_lock);
        HMAP_FOR_EACH (fb_task, hmap_node, &fb_ctx->task_map) {
            if (fb_task->vjetty_id == vjetty_id && task_num < URMA_UBAGG_DEV_MAX_NUM) {
                if (fb_task->worker_task_id == 0) {
                    (void)pthread_rwlock_unlock(&fb_ctx->task_lock);
                    URMA_LOG_ERR("Failback task has no worker task ID, vjetty_id=%u.\n", vjetty_id);
                    return -EIO;
                }
                fb_task->canceling = true;
                task_ids[task_num++] = fb_task->worker_task_id;
            }
        }
        (void)pthread_rwlock_unlock(&fb_ctx->task_lock);
        if (task_num == 0) {
            return 0;
        }

        int ret = bondp_worker_cancel_batch(task_ids, task_num);
        if (ret != 0) {
            URMA_LOG_ERR("Failed to cancel jetty failback tasks, vjetty_id=%u, ret=%d.\n",
                         vjetty_id, ret);
            return ret;
        }

        /* canceling is serialized with every transition. Entries therefore
         * must disappear after the synchronous cancellation; loop only to
         * cover tasks inserted before the caller's ERROR/deleting state won
         * its own serialization lock. */
    }
}

int bondp_fb_init(bondp_context_t *bond_ctx)
{
    if (bond_ctx == NULL) {
        return -EINVAL;
    }

    bond_ctx->fb_ctx = fb_task_table_create();
    if (bond_ctx->fb_ctx == NULL) {
        return -ENOMEM;
    }

    init_request_id(bond_ctx->fb_ctx);
    return 0;
}

int bondp_fb_uninit(bondp_context_t *bond_ctx)
{
    if (bond_ctx == NULL || bond_ctx->fb_ctx == NULL) {
        return 0;
    }

    int ret = fb_task_table_destroy(bond_ctx->fb_ctx);
    if (ret != 0) {
        return ret;
    }
    bond_ctx->fb_ctx = NULL;
    return 0;
}
