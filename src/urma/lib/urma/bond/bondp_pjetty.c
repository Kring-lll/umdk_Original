/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Physical jetty references and deferred reclamation.
 */

#include <stdlib.h>
#include "bondp_pjetty.h"
#include "bondp_context_table.h"
#include "urma_api.h"
#include "urma_log.h"

bondp_pjetty_ref_t *bondp_pjetty_create(urma_context_t *ctx, urma_jetty_cfg_t *cfg)
{
    bondp_pjetty_ref_t *ref = calloc(1, sizeof(*ref));
    if (ref == NULL) {
        return NULL;
    }
    ref->jetty = urma_create_jetty(ctx, cfg);
    if (ref->jetty == NULL) {
        free(ref);
        return NULL;
    }
    ref->id = ref->jetty->jetty_id;
    return ref;
}

bondp_pjetty_ref_t *bondp_pjetty_get(bondp_comp_t *comp, uint32_t idx)
{
    bondp_pjetty_ref_t *ref = NULL;
    bondp_hash_table_t *table = &comp->bondp_ctx->p_vjetty_id_table;
    (void)pthread_rwlock_rdlock(&table->lock);
    if (!atomic_load(&comp->deleting)) {
        (void)pthread_spin_lock(&comp->send_lock);
        ref = comp->pjetty_refs[idx];
        if (ref != NULL) {
            ref->readers++;
            atomic_fetch_add(&comp->use_cnt.atomic_cnt, 1);
        }
        (void)pthread_spin_unlock(&comp->send_lock);
    }
    (void)pthread_rwlock_unlock(&table->lock);
    return ref;
}

void bondp_pjetty_retire(bondp_comp_t *comp, bondp_pjetty_ref_t *ref)
{
    (void)pthread_spin_lock(&comp->send_lock);
    ref->next = comp->retired_pjettys;
    comp->retired_pjettys = ref;
    (void)pthread_spin_unlock(&comp->send_lock);
}

static bondp_pjetty_ref_t *bondp_pjetty_collect(bondp_comp_t *comp)
{
    bondp_pjetty_ref_t *ready = NULL;
    (void)pthread_spin_lock(&comp->send_lock);
    bondp_pjetty_ref_t **link = &comp->retired_pjettys;
    while (*link != NULL) {
        bondp_pjetty_ref_t *ref = *link;
        if (ref->readers != 0) {
            link = &ref->next;
            continue;
        }
        *link = ref->next;
        ref->next = ready;
        ready = ref;
    }
    (void)pthread_spin_unlock(&comp->send_lock);
    return ready;
}

static int bondp_pjetty_destroy(bondp_comp_t *comp, bondp_pjetty_ref_t *ref)
{
    if (ref->jetty != NULL) {
        int ret = urma_delete_jetty(ref->jetty);
        if (ret != URMA_SUCCESS) {
            return ret;
        }
        ref->jetty = NULL;
    }
    if (ref->mapped) {
        bondp_hash_table_t *table = &comp->bondp_ctx->p_vjetty_id_table;
        (void)pthread_rwlock_wrlock(&table->lock);
        int ret = bdp_p_vjetty_id_table_del_without_lock(table, ref->id, JETTY);
        (void)pthread_rwlock_unlock(&table->lock);
        if (ret != 0) {
            return ret;
        }
    }
    return URMA_SUCCESS;
}

int bondp_pjetty_reap(bondp_comp_t *comp)
{
    bondp_pjetty_ref_t *ref = bondp_pjetty_collect(comp);
    int ret = URMA_SUCCESS;
    while (ref != NULL) {
        bondp_pjetty_ref_t *next = ref->next;
        int status = bondp_pjetty_destroy(comp, ref);
        if (status != URMA_SUCCESS) {
            ret = status;
            URMA_LOG_WARN("Failed to reclaim physical jetty %u, ret=%d.\n", ref->id.id, status);
            bondp_pjetty_retire(comp, ref);
        } else {
            free(ref);
        }
        ref = next;
    }
    (void)pthread_spin_lock(&comp->send_lock);
    if (ret == URMA_SUCCESS && comp->retired_pjettys != NULL) {
        ret = URMA_EAGAIN;
    }
    (void)pthread_spin_unlock(&comp->send_lock);
    return ret;
}

void bondp_pjetty_put(bondp_comp_t *comp, bondp_pjetty_ref_t *ref)
{
    (void)pthread_spin_lock(&comp->send_lock);
    ref->readers--;
    (void)pthread_spin_unlock(&comp->send_lock);
    (void)bondp_pjetty_reap(comp);
    atomic_fetch_sub(&comp->use_cnt.atomic_cnt, 1);
}
