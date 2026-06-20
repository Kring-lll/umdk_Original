/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bond provider health check v2 declarations (out-of-band, per-context)
 */

#ifndef BONDP_HEALTH_CHECK_V2_H
#define BONDP_HEALTH_CHECK_V2_H

#include <stdbool.h>
#include <stdint.h>
#include "urma_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct bondp_context;
struct bondp_comp;
struct bondp_target_jetty;

/**
 * Setup per-context probe resources for health-check v2.
 * Called from bondp_create_context() after p_ctxs[] are ready.
 * Creates one out-of-band probe jetty per active local device via
 * bdp_ctx->p_ctxs[i], and registers the jfce fd to bondp_worker.
 */
int bondp_hc_v2_setup_ctx(struct bondp_context *bdp_ctx);

/**
 * Cleanup per-context probe resources for health-check v2.
 * Called from bondp_delete_context() before tearing down p_ctxs[].
 */
void bondp_hc_v2_cleanup_ctx(struct bondp_context *bdp_ctx);

/**
 * Register a remote node into the health-check array when importing a jetty.
 *
 * @bdp_ctx:    bond context
 * @cfg_jetty:  the local vjetty (bondp_comp_t) associated with this import
 * @bdp_tjetty: the imported target-jetty (bonding EID is taken from v_tjetty.id.eid)
 *
 * If the node already exists, the vjetty is appended to that node.
 */
int bondp_hc_v2_register_node(struct bondp_context *bdp_ctx,
                              struct bondp_comp *cfg_jetty,
                              struct bondp_target_jetty *bdp_tjetty);

/**
 * Unregister a remote node (or decrement its reference) when unimporting a jetty.
 */
void bondp_hc_v2_unregister_node(struct bondp_context *bdp_ctx,
                                 struct bondp_target_jetty *bdp_tjetty);

/**
 * Query whether a specific (local_idx, target_idx) path to a given node is healthy.
 * Invalid path defaults to false; unknown node defaults to true.
 */
bool bondp_hc_v2_is_node_link_ok(struct bondp_context *bdp_ctx, uint32_t node_id,
                                  uint32_t local_idx, uint32_t target_idx);

/**
 * Query whether the given node has any healthy path (aggregated state).
 * Unknown node defaults to true.
 */
bool bondp_hc_v2_is_node_ok(struct bondp_context *bdp_ctx, uint32_t node_id);

#ifdef __cplusplus
}
#endif

#endif // BONDP_HEALTH_CHECK_V2_H
