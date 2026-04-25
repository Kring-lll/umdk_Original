/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bond provider link recovery implementation
 */

#include "bondp_link_recovery.h"

#include "bondp_context_table.h"
#include "urma_log.h"
#include "urma_api.h"

static int bondp_get_target_idx_by_local_idx(const bondp_health_task_t *task, int local_idx, int *target_idx)
{
    for (int i = 0; i < URMA_UBAGG_DEV_MAX_NUM; ++i) {
        for (int j = 0; j < URMA_UBAGG_DEV_MAX_NUM; ++j) {
            const bondp_health_sub_task_t *sub = &task->sub_tasks[i][j];
            if (!sub->valid || sub->local_idx != local_idx) {
                continue;
            }
            *target_idx = sub->target_idx;
            return 0;
        }
    }
    return -1;
}

static int bondp_update_pjetty_id_mapping(
    bondp_context_t *bdp_ctx, urma_jetty_id_t old_id, urma_jetty_id_t new_id, bondp_comp_t *bdp_comp)
{
    bdp_p_vjetty_type_t type;
    uint32_t vobj_id;
    switch (bdp_comp->comp_type) {
        case BONDP_COMP_JETTY:
            type = JETTY;
            vobj_id = bdp_comp->v_jetty.jetty_id.id;
            break;
        case BONDP_COMP_JFS:
            type = JFS;
            vobj_id = bdp_comp->v_jfs.jfs_id.id;
            break;
        default:
            URMA_LOG_ERR("Unsupported comp type for id mapping update:%d\n", bdp_comp->comp_type);
            return -1;
    }

    int ret = 0;
    pthread_rwlock_wrlock(&bdp_ctx->p_vjetty_id_table.lock);
    ret = bdp_p_vjetty_id_table_del_without_lock(&bdp_ctx->p_vjetty_id_table, old_id, type);
    if (ret != 0) {
        pthread_rwlock_unlock(&bdp_ctx->p_vjetty_id_table.lock);
        URMA_LOG_ERR("Failed to delete stale pjetty id mapping: " URMA_JETTY_ID_FMT ", ret:%d\n",
            URMA_JETTY_ID_ARGS(&old_id), ret);
        return -1;
    }
    ret = bdp_p_vjetty_id_table_add_without_lock(
        &bdp_ctx->p_vjetty_id_table, new_id, type, vobj_id, bdp_comp);
    pthread_rwlock_unlock(&bdp_ctx->p_vjetty_id_table.lock);
    if (ret != 0) {
        URMA_LOG_ERR("Failed to add recreated pjetty id mapping: " URMA_JETTY_ID_FMT ", ret:%d\n",
            URMA_JETTY_ID_ARGS(&new_id), ret);
        return -1;
    }
    return 0;
}

static void bondp_drain_pjetty_wr(urma_jetty_t *old_jetty, int local_idx)
{
    urma_cr_t cr_buf[URMA_UBAGG_MAX_CR_CNT_PER_DEV] = {0};
    int flushed = 0;
    do {
        flushed = urma_flush_jetty(old_jetty, URMA_UBAGG_MAX_CR_CNT_PER_DEV, cr_buf);
        if (flushed < 0) {
            URMA_LOG_WARN("Failed to flush pjetty before rebuild, idx:%d ret:%d\n", local_idx, flushed);
            break;
        }
    } while (flushed > 0);
}

static void bondp_drain_pjfs_wr(urma_jfs_t *old_jfs, int local_idx)
{
    urma_cr_t cr_buf[URMA_UBAGG_MAX_CR_CNT_PER_DEV] = {0};
    int flushed = 0;
    do {
        flushed = urma_flush_jfs(old_jfs, URMA_UBAGG_MAX_CR_CNT_PER_DEV, cr_buf);
        if (flushed < 0) {
            URMA_LOG_WARN("Failed to flush pjfs before rebuild, idx:%d ret:%d\n", local_idx, flushed);
            break;
        }
    } while (flushed > 0);
}

int bondp_rebuild_local_pjetty(bondp_health_task_t *task, int local_idx)
{
    if (task == NULL || task->bondp_jetty == NULL ||
        local_idx < 0 || local_idx >= URMA_UBAGG_DEV_MAX_NUM) {
        return -1;
    }

    bondp_comp_t *bdp_jetty = task->bondp_jetty;
    bondp_context_t *bdp_ctx = bdp_jetty->bondp_ctx;
    if (bdp_jetty->members[local_idx] == NULL) {
        return -1;
    }

    int target_idx = -1;
    bondp_health_sub_task_t *sub_task = NULL;
    if (bondp_get_target_idx_by_local_idx(task, local_idx, &target_idx) == 0 &&
        target_idx >= 0 && target_idx < URMA_UBAGG_DEV_MAX_NUM) {
        sub_task = &task->sub_tasks[local_idx][target_idx];
        sub_task->valid = false;
        sub_task->probe_pending = false;
    }

    urma_jetty_id_t old_id;
    urma_jetty_id_t new_id;
    if (bdp_jetty->comp_type == BONDP_COMP_JETTY) {
        urma_jetty_t *old_jetty = bdp_jetty->p_jetty[local_idx];
        bondp_drain_pjetty_wr(old_jetty, local_idx);

        urma_jetty_cfg_t p_cfg = bdp_jetty->v_jetty.jetty_cfg;
        bondp_jfc_t *bdp_jfs_jfc = CONTAINER_OF_FIELD(p_cfg.jfs_cfg.jfc, bondp_jfc_t, v_jfc);
        bondp_comp_t *bdp_jfr = CONTAINER_OF_FIELD(p_cfg.shared.jfr, bondp_comp_t, base);
        bondp_jfc_t *bdp_rplc_jfc = NULL;
        if (p_cfg.shared.jfc != NULL) {
            bdp_rplc_jfc = CONTAINER_OF_FIELD(p_cfg.shared.jfc, bondp_jfc_t, v_jfc);
        }
        p_cfg.jfs_cfg.jfc = bdp_jfs_jfc->p_jfc[local_idx];
        p_cfg.shared.jfr = bdp_jfr->p_jfr[local_idx];
        if (bdp_rplc_jfc != NULL) {
            p_cfg.shared.jfc = bdp_rplc_jfc->p_jfc[local_idx];
        }

        old_id = old_jetty->jetty_id;
        if (urma_delete_jetty(old_jetty) != URMA_SUCCESS) {
            URMA_LOG_ERR("Failed to delete pjetty at idx:%d\n", local_idx);
            return -1;
        }

        urma_jetty_t *new_jetty = urma_create_jetty(bdp_ctx->p_ctxs[local_idx], &p_cfg);
        if (new_jetty == NULL) {
            URMA_LOG_ERR("Failed to recreate pjetty at idx:%d\n", local_idx);
            return -1;
        }

        /* Recreated pjetty should not keep stale peer binding. */
        new_jetty->remote_jetty = NULL;
        new_jetty->jetty_cfg.user_ctx = (uint64_t)bdp_jetty;
        bdp_jetty->p_jetty[local_idx] = new_jetty;
        new_id = new_jetty->jetty_id;
    } else if (bdp_jetty->comp_type == BONDP_COMP_JFS) {
        urma_jfs_t *old_jfs = bdp_jetty->p_jfs[local_idx];
        bondp_drain_pjfs_wr(old_jfs, local_idx);

        urma_jfs_cfg_t p_cfg = bdp_jetty->v_jfs.jfs_cfg;
        bondp_jfc_t *bdp_jfc = CONTAINER_OF_FIELD(p_cfg.jfc, bondp_jfc_t, v_jfc);
        p_cfg.jfc = bdp_jfc->p_jfc[local_idx];

        old_id = old_jfs->jfs_id;
        if (urma_delete_jfs(old_jfs) != URMA_SUCCESS) {
            URMA_LOG_ERR("Failed to delete pjfs at idx:%d\n", local_idx);
            return -1;
        }

        urma_jfs_t *new_jfs = urma_create_jfs(bdp_ctx->p_ctxs[local_idx], &p_cfg);
        if (new_jfs == NULL) {
            URMA_LOG_ERR("Failed to recreate pjfs at idx:%d\n", local_idx);
            return -1;
        }

        new_jfs->jfs_cfg.user_ctx = (uint64_t)bdp_jetty;
        bdp_jetty->p_jfs[local_idx] = new_jfs;
        new_id = new_jfs->jfs_id;
    } else {
        return -1;
    }
    bdp_jetty->valid[local_idx] = false;

    if (bondp_update_pjetty_id_mapping(bdp_ctx, old_id, new_id, bdp_jetty) != 0) {
        return -1;
    }

    if (sub_task != NULL) {
        sub_task->valid = true;
        sub_task->probe_pending = false;
        atomic_store(&sub_task->link_ok, true);
    }
    URMA_LOG_INFO("Health link rebuilt, idx:%d old:" URMA_JETTY_ID_FMT " new:" URMA_JETTY_ID_FMT "\n",
        local_idx, URMA_JETTY_ID_ARGS(&old_id), URMA_JETTY_ID_ARGS(&new_id));
    return 0;
}
