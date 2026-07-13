/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bond provider health check declarations
 */

#ifndef BONDP_HEALTH_H
#define BONDP_HEALTH_H

#include "urma_types.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct bondp_context;
struct bondp_target_jetty;
struct bondp_comp;

#define BONDP_HC_DEFAULT_PROBE_INTERVAL_MS (1000)
#define BONDP_HC_DEFAULT_PROBE_NODE_NUM    (1024)

typedef struct bondp_hc_cfg {
    /*
     * Interval between two health probe batches.
     * Set to 0 to use BONDP_HC_DEFAULT_PROBE_INTERVAL_MS.
     */
    uint64_t probe_interval_ms;
    /*
     * Maximum nodes probed in one batch.
     * Set to 0 to use BONDP_HC_DEFAULT_PROBE_NODE_NUM.
     */
    uint32_t probe_node_num;
} bondp_hc_cfg_t;

int bondp_hc_init(struct bondp_context *bdp_ctx, const bondp_hc_cfg_t *cfg);
void bondp_hc_uninit(struct bondp_context *bdp_ctx);

/**
 * Register a target jetty for health path tracking.
 * The peer topo node is resolved by the target EID.
 */
int bondp_hc_register_tjetty(struct bondp_context *bdp_ctx,
                             struct bondp_target_jetty *bdp_tjetty);

/**
 * Unregister a target jetty from health path tracking.
 */
void bondp_hc_unregister_tjetty(struct bondp_context *bdp_ctx,
                                struct bondp_target_jetty *bdp_tjetty);

/**
 * Query health status for one target jetty path.
 * Returns true when health check is not registered for this target jetty.
 */
bool bondp_hc_tjetty_path_valid(const struct bondp_target_jetty *bdp_tjetty,
                                uint32_t local_idx, uint32_t target_idx);

/**
 * Mark a datapath-confirmed failure for every registered link sharing local_idx.
 * Called by the datapath failover CR handler once it has flipped bdp_comp->valid=false,
 * so the health-check recovery gate (hc_port_links_all_recovered) starts from the
 * datapath failure rather than waiting for a probe to independently observe it.
 * This closes the window where a transient failure recovered before the next probe
 * round would leave link_failed=false and permanently wedged valid=false.
 */
void bondp_hc_mark_pctx_failed(struct bondp_context *bdp_ctx, uint32_t local_idx);

#ifdef __cplusplus
}
#endif

#endif // BONDP_HEALTH_H
