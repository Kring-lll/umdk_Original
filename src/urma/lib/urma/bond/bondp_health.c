/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bond provider health check v2 implementation (per-context)
 *   - Node-granularity health probing (one-to-many / many-to-one)
 *   - Out-of-band jetty per path, isolated from user datapath
 *   - Driven by bondp_worker (epoll + timewheel)
 */

#include <errno.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bondp_api.h"
#include "bondp_types.h"
#include "bondp_worker.h"
#include "topo_info.h"
#include "ub_list.h"
#include "urma_api.h"
#include "urma_log.h"
#include "bondp_health.h"

#define HC_CQE_BATCH     (8)
#define HC_PROBE_BUF_LEN (1)
#define HC_PROBE_DEPTH   (1024)
/* Upper bound on poll rounds in one drain. The jfc depth is 4096 and each round
 * reaps up to HC_CQE_BATCH CQEs, so 4096/8 + headroom covers any backlog; the
 * bound also guarantees termination even if a provider never returns an empty poll. */
#define HC_DRAIN_MAX_ROUNDS (1024)
/* Stop posting probes when a local probe SQ has this many outstanding WRs,
 * leaving headroom so a batch cannot overflow the SQ (depth HC_PROBE_DEPTH) and
 * wedge urma_post_jetty_send_wr into ENOMEM. */
#define HC_PROBE_INFLIGHT_HI HC_PROBE_DEPTH
/* ummu_grant requires page-aligned VA and a length multiple of the page size.
 * The probe payload is only HC_PROBE_BUF_LEN bytes, but the registered segment
 * must cover a full page; the data path keeps its 1-byte sge length. */
#define HC_PROBE_SEG_LEN (getpagesize())

typedef struct bondp_hc_ctx bondp_hc_ctx_t;

typedef struct bondp_probe_res {
    bondp_hc_ctx_t *hc_ctx;
    int local_idx;
    void *buf;
    urma_jfce_t *jfce;
    urma_jfc_t *jfc;
    urma_jfr_t *jfr;
    urma_target_seg_t *seg;
    urma_jetty_t *jetty;
    int jfce_fd;
    /* Number of probe WRs posted but not yet completed. Only touched on the
     * single worker thread, so a plain counter is safe. Used to stop posting
     * when the probe SQ nears capacity, otherwise urma_post_jetty_send_wr
     * returns ENOMEM and the SQ wedges. */
    uint32_t inflight;
} bondp_probe_res_t;

typedef struct bondp_hc_node {
    uint32_t node_id;
    uint32_t node_idx;

#ifndef __cplusplus
    atomic_bool valid[URMA_UBAGG_DEV_MAX_NUM][URMA_UBAGG_DEV_MAX_NUM];
#else
    std::atomic_bool valid[URMA_UBAGG_DEV_MAX_NUM][URMA_UBAGG_DEV_MAX_NUM];
#endif
    /*
     * Set true the first time the probe reports a failure for (local_idx, target_idx).
     * hc_check_and_revalidate_port only restores bdp_comp->valid when every registered
     * link has valid==true AND link_failed==true, i.e. the link went down and then came
     * back. This prevents the initial valid==true (set at hc_init_node before any probe
     * ran) from being mistaken for "recovered", which would re-enable a still-down port
     * right after the failover CR set bdp_comp->valid=false.
     */
    bool link_failed[URMA_UBAGG_DEV_MAX_NUM][URMA_UBAGG_DEV_MAX_NUM];

    pthread_rwlock_t lock; /* Protects tjetty_list and hc_tjetty */
    struct bondp_target_jetty *hc_tjetty[URMA_UBAGG_DEV_MAX_NUM][URMA_UBAGG_DEV_MAX_NUM];
    struct ub_list tjetty_list;
} bondp_hc_node_t;

/* Per-context health-check context */
struct bondp_hc_ctx {
    bondp_hc_cfg_t cfg;
    bondp_context_t *bdp_ctx; /* Back-pointer to the owning bond context (to restore valid[]) */
    bondp_probe_res_t probes[URMA_UBAGG_DEV_MAX_NUM];
    bondp_worker_task_id_t probe_task_id;
    atomic_bool stopping; /* Set by uninit to stop the probe task from rescheduling */
    uint32_t probe_cur_idx; /* Current polling position for batched node probing */
    uint32_t node_num;
    bondp_hc_node_t nodes[MAX_NODE_NUM];
};

/*
 * Health probe user_ctx layout:
 *   bits [63:32]: node_idx, used to find the remote node health context.
 *   bits [31:0] : target_idx, used with local_idx to find the checked path.
 */
#define HC_USER_CTX_TARGET_BITS (32)

static inline uint64_t hc_encode_user_ctx(uint32_t node_idx, uint32_t target_idx)
{
    return ((uint64_t)node_idx << HC_USER_CTX_TARGET_BITS) | target_idx;
}

static inline void hc_decode_user_ctx(uint64_t user_ctx, uint32_t *node_idx, uint32_t *target_idx)
{
    *node_idx = (uint32_t)(user_ctx >> HC_USER_CTX_TARGET_BITS);
    *target_idx = (uint32_t)user_ctx;
}

static urma_jetty_t *hc_create_probe_jetty(urma_context_t *p_ctx, urma_jfc_t *jfc,
                                           urma_jfr_t *jfr, int local_idx)
{
    urma_jetty_cfg_t p_cfg = {
        .flag = {.bs = {.share_jfr = URMA_SHARE_JFR}},
        .jfs_cfg = {
            .depth = HC_PROBE_DEPTH,
            .trans_mode = URMA_TM_RM,
            .max_sge = 1,
            .rnr_retry = URMA_TYPICAL_RNR_RETRY,
            .err_timeout = 0,
            .jfc = jfc,
        },
        .shared.jfr = jfr,
        .shared.jfc = NULL,
        .jetty_grp = NULL,
    };
    urma_jetty_t *jetty = urma_create_jetty(p_ctx, &p_cfg);
    if (jetty == NULL) {
        URMA_LOG_ERR("Failed to create health probe jetty, local_idx=%d.\n", local_idx);
    }
    return jetty;
}

static void hc_rebuild_probe_jetty(bondp_probe_res_t *res)
{
    int local_idx = res->local_idx;
    if (res->jfc == NULL || res->jfc->urma_ctx == NULL || res->jfr == NULL) {
        URMA_LOG_ERR("Invalid health probe resource for jetty rebuild, local_idx=%d.\n", local_idx);
        return;
    }

    urma_context_t *p_ctx = res->jfc->urma_ctx;
    urma_jetty_attr_t attr = {
        .mask = JETTY_STATE,
        .state = URMA_JETTY_STATE_ERROR,
    };

    if (res->jetty != NULL) {
        urma_status_t ret = urma_modify_jetty(res->jetty, &attr);
        if (ret != URMA_SUCCESS) {
            URMA_LOG_WARN("Failed to modify health probe jetty to error, local_idx=%d, ret=%d.\n",
                          local_idx, ret);
        }

        urma_delete_jetty(res->jetty);
    }
    res->inflight = 0; /* The old SQ is gone; its outstanding WRs are flushed. */

    res->jetty = hc_create_probe_jetty(p_ctx, res->jfc, res->jfr, local_idx);
    if (res->jetty == NULL) {
        URMA_LOG_ERR("Failed to rebuild health probe jetty, local_idx=%d.\n", local_idx);
    }
}

static void hc_check_and_revalidate_port(bondp_hc_ctx_t *hc_ctx, int local_idx);

static bool hc_apply_probe_cqe(bondp_hc_ctx_t *hc_ctx, const urma_cr_t *cr, int local_idx)
{
    bool is_timeout = (cr->status == URMA_CR_ACK_TIMEOUT_ERR); /* status 9 */

    uint32_t node_idx;
    uint32_t target_idx;
    hc_decode_user_ctx(cr->user_ctx, &node_idx, &target_idx);

    if (node_idx >= hc_ctx->node_num || target_idx >= URMA_UBAGG_DEV_MAX_NUM) {
        return is_timeout; /* still rebuild on timeout even for out-of-range ctx */
    }
    bondp_hc_node_t *node = &hc_ctx->nodes[node_idx];

    bool ok = (cr->status == URMA_CR_SUCCESS);
    /* link_failed is a plain bool raced by the datapath thread
     * (bondp_hc_mark_pctx_failed); guard it with node->lock. valid is atomic
     * so its store is safe outside the lock, but we keep it inside to avoid
     * an extra lock round-trip. */
    pthread_rwlock_wrlock(&node->lock);
    atomic_store(&node->valid[local_idx][target_idx], ok);
    if (!ok) {
        /* Mark that this link has actually failed (not just the initial true from
         * hc_init_node). Only after a subsequent probe SUCCESS will
         * hc_check_and_revalidate_port consider the link "recovered". */
        node->link_failed[local_idx][target_idx] = true;
    }
    pthread_rwlock_unlock(&node->lock);
    return is_timeout;
}

static void hc_drain_probe_cq(bondp_probe_res_t *res)
{
    if (res == NULL || res->jfc == NULL || res->jfce == NULL) {
        return;
    }
    int local_idx = res->local_idx;
    bondp_hc_ctx_t *hc_ctx = res->hc_ctx;
    if (hc_ctx == NULL || local_idx < 0 || local_idx >= URMA_UBAGG_DEV_MAX_NUM ||
        res != &hc_ctx->probes[local_idx]) {
        return;
    }

    urma_jfc_t *jfc = res->jfc;
    urma_cr_t cr[HC_CQE_BATCH];
    bool need_rebuild = false;
    uint32_t drained = 0;

    /* Poll the CQ directly rather than going through urma_wait_jfc(jfce).
     * The wait() path is gated on a jfce fd event, which only fires for a CQ
     * that was armed; between two jfce events the worker keeps posting probes
     * (single-threaded), so relying on wait() would leave completions un-reaped
     * and overflow the probe SQ (ENOMEM). Polling the jfc here drains all
     * pending CQEs regardless of the jfce arm state, advancing sq->ci and
     * freeing SQ space before the next batch is posted. HC_DRAIN_MAX_ROUNDS
     * bounds the loop so it always exits even if a provider never reports an
     * empty poll. */
    for (uint32_t round = 0; round < HC_DRAIN_MAX_ROUNDS; round++) {
        int n = urma_poll_jfc(jfc, HC_CQE_BATCH, cr);
        if (n <= 0) {
            break;
        }
        drained += (uint32_t)n;
        for (int k = 0; k < n; k++) {
            if (hc_apply_probe_cqe(hc_ctx, &cr[k], local_idx)) {
                need_rebuild = true;
            }
        }
    }
    if (drained >= res->inflight) {
        res->inflight = 0;
    } else {
        res->inflight -= drained;
    }

    urma_status_t ret = urma_rearm_jfc(jfc, false);
    if (ret != URMA_SUCCESS) {
        URMA_LOG_WARN("Failed to rearm health probe jfc, local_idx=%d, ret=%d.\n", local_idx, ret);
    }

    if (need_rebuild) {
        hc_rebuild_probe_jetty(res);
    }

    /* Re-enable the datapath path for this local_idx once the health probe confirms
     * every link on it is back up. valid[local_idx] was flipped to false by the failover
     * CR handler (handle_send_cr_with_store) and must NOT be restored by try_failback
     * (which would re-enable a rebuilt-but-still-down pjetty and crash in
     * udma_u_post_one_wr). Here we only restore it after the probe CQEs show all
     * node->valid[local_idx][*] are SUCCESS again. If any link is still down we leave
     * valid=false and rely on the next probe round. */
    hc_check_and_revalidate_port(hc_ctx, local_idx);
}

/* Return true iff there is at least one registered link for local_idx AND every such
 * link is currently healthy (node->valid==true) AND has gone through a failure-recovery
 * cycle (link_failed==true). Requiring link_failed on every link (not just any one)
 * distinguishes a genuinely recovered port from a port that still has links on their
 * initial valid==true (set at hc_init_node before any probe ran) — the latter would let
 * hc_check_and_revalidate_port re-enable a partially-still-down port immediately after
 * the failover CR set bdp_comp->valid=false. link_failed is mirrored from the datapath
 * failover by bondp_hc_mark_pctx_failed, so this also gates recovery on the datapath
 * failure rather than only on an independently-observed probe failure.
 *
 * node->lock is held in read mode because hc_tjetty and link_failed are also written
 * from the datapath thread (bondp_hc_mark_pctx_failed) — without it this worker-side
 * reader races the writer. */
static bool hc_port_links_all_recovered(bondp_hc_ctx_t *hc_ctx, int local_idx)
{
    if (hc_ctx == NULL || local_idx < 0 || local_idx >= URMA_UBAGG_DEV_MAX_NUM) {
        return false;
    }
    bool any_registered = false;
    for (uint32_t n = 0; n < hc_ctx->node_num; ++n) {
        bondp_hc_node_t *node = &hc_ctx->nodes[n];
        pthread_rwlock_rdlock(&node->lock);
        for (uint32_t t = 0; t < URMA_UBAGG_DEV_MAX_NUM; ++t) {
            if (node->hc_tjetty[local_idx][t] == NULL) {
                continue;
            }
            any_registered = true;
            if (!atomic_load(&node->valid[local_idx][t])) {
                pthread_rwlock_unlock(&node->lock);
                return false;   /* link currently down */
            }
            if (!node->link_failed[local_idx][t]) {
                pthread_rwlock_unlock(&node->lock);
                return false;   /* link never failed this cycle; not yet recovered */
            }
        }
        pthread_rwlock_unlock(&node->lock);
    }
    return any_registered;
}

/* Restore bdp_comp->valid[local_idx]=true (via the bond API) once the probe confirms all
 * links on that local index are back up AND have gone through a failure first. Runs on
 * the bond worker thread; valid[] is atomic so this races safely with datapath readers. */
static void hc_check_and_revalidate_port(bondp_hc_ctx_t *hc_ctx, int local_idx)
{
    if (hc_ctx == NULL || hc_ctx->bdp_ctx == NULL) {
        return;
    }
    if (!hc_port_links_all_recovered(hc_ctx, local_idx)) {
        return;
    }
    /* Clear link_failed so the next failure cycle starts fresh; without this a second
     * port-down/up would be treated as already recovered on the initial valid==true.
     * Take node->lock to race safely against bondp_hc_mark_pctx_failed (datapath thread)
     * which may concurrently set link_failed on another local_idx of the same node. */
    for (uint32_t n = 0; n < hc_ctx->node_num; ++n) {
        bondp_hc_node_t *node = &hc_ctx->nodes[n];
        pthread_rwlock_wrlock(&node->lock);
        for (uint32_t t = 0; t < URMA_UBAGG_DEV_MAX_NUM; ++t) {
            if (node->hc_tjetty[local_idx][t] != NULL) {
                node->link_failed[local_idx][t] = false;
            }
        }
        pthread_rwlock_unlock(&node->lock);
    }
    bondp_revalidate_pctx_path(hc_ctx->bdp_ctx, (uint32_t)local_idx);
}

static void hc_jfce_handler(void *arg)
{
    /* The jfce fd fired: at least one CQE arrived since the last arm. Drain the
     * CQ and re-arm. */
    hc_drain_probe_cq((bondp_probe_res_t *)arg);
}

/* Build and post one probe WRITE for a single (local_idx, target_idx) path.
 * On success increments res->inflight; on failure marks the link down so the
 * recovery gate requires a later success before re-validating. Extracted from
 * hc_probe_link to keep that function within the length limit (G.FUD.05). */
static void hc_post_probe_wr(bondp_hc_ctx_t *hc_ctx, bondp_hc_node_t *node,
                             bondp_probe_res_t *res, int local_idx, int target_idx)
{
    bondp_target_jetty_t *bdp_tjetty = node->hc_tjetty[local_idx][target_idx];
    if (bdp_tjetty == NULL) {
        return;
    }
    urma_target_jetty_t *tjetty = bdp_tjetty->p_tjetty[local_idx][target_idx];
    urma_target_seg_t *tseg = bdp_tjetty->p_check_tseg[local_idx][target_idx];
    if (tjetty == NULL || tseg == NULL) {
        return;
    }

    urma_sge_t src_sge = {
        .addr = (uint64_t)res->buf,
        .len = HC_PROBE_BUF_LEN,
        .tseg = res->seg,
        .user_tseg = NULL,
    };
    urma_sge_t dst_sge = {
        .addr = tseg->seg.ubva.va,
        .len = HC_PROBE_BUF_LEN,
        .tseg = tseg,
        .user_tseg = NULL,
    };

    uint64_t user_ctx = hc_encode_user_ctx(node->node_idx, (uint32_t)target_idx);
    urma_jfs_wr_t wr = {
        .opcode = URMA_OPC_WRITE,
        .flag.bs.complete_enable = 1,
        .tjetty = tjetty,
        .user_ctx = user_ctx,
        .rw = {
            .src = {.sge = &src_sge, .num_sge = 1},
            .dst = {.sge = &dst_sge, .num_sge = 1},
        },
        .next = NULL,
    };

    urma_jfs_wr_t *bad_wr = NULL;
    urma_status_t ret = urma_post_jetty_send_wr(res->jetty, &wr, &bad_wr);
    if (ret == URMA_SUCCESS) {
        res->inflight++;
    } else {
        URMA_LOG_WARN("Failed to send health probe, node_id=%u, local_idx=%d, target_idx=%d, ret=%d.\n",
                      node->node_id, local_idx, target_idx, ret);
        atomic_store(&node->valid[local_idx][target_idx], false);
        node->link_failed[local_idx][target_idx] = true;
    }
}

static void hc_probe_link(bondp_hc_ctx_t *hc_ctx, bondp_hc_node_t *node,
                          int local_idx, int target_idx)
{
    if (hc_ctx == NULL || node == NULL ||
        local_idx < 0 || local_idx >= URMA_UBAGG_DEV_MAX_NUM ||
        target_idx < 0 || target_idx >= URMA_UBAGG_DEV_MAX_NUM) {
        return;
    }

    bondp_probe_res_t *res = &hc_ctx->probes[local_idx];
    if (res->jetty == NULL || res->seg == NULL || res->buf == NULL) {
        return;
    }

    /* Throttle: stop posting once the local probe SQ is close to capacity. The
     * worker is single-threaded, so completions cannot be reaped while this
     * batch is being posted; without throttling the SQ overflows and
     * urma_post_jetty_send_wr returns ENOMEM. Remaining paths are picked up by
     * the next firing after the CQ is drained. */
    if (res->inflight >= HC_PROBE_INFLIGHT_HI) {
        return;
    }

    hc_post_probe_wr(hc_ctx, node, res, local_idx, target_idx);
}

static bool hc_probe_sq_has_room(bondp_hc_ctx_t *hc_ctx)
{
    /* Return true if at least one local probe SQ still has room for a new WR.
     * Used to short-circuit the node scan once every local_idx is throttled. */
    for (uint32_t i = 0; i < URMA_UBAGG_DEV_MAX_NUM; ++i) {
        if (hc_ctx->probes[i].jetty != NULL && hc_ctx->probes[i].inflight < HC_PROBE_INFLIGHT_HI) {
            return true;
        }
    }
    return false;
}

static void hc_probe_node(bondp_hc_ctx_t *hc_ctx, bondp_hc_node_t *node)
{
    /* wrlock (not rdlock): hc_probe_link writes node->valid and node->link_failed on the
     * post-failure path, and link_failed is now also written from the datapath thread
     * (bondp_hc_mark_pctx_failed). Holding a write lock here both serializes against that
     * writer and lets hc_probe_link mutate link_failed without a nested lock. */
    pthread_rwlock_wrlock(&node->lock);
    for (int i = 0; i < URMA_UBAGG_DEV_MAX_NUM; ++i) {
        if (hc_ctx->probes[i].inflight >= HC_PROBE_INFLIGHT_HI) {
            continue;
        }
        for (int j = 0; j < URMA_UBAGG_DEV_MAX_NUM; ++j) {
            if (node->hc_tjetty[i][j] == NULL) {
                continue;
            }
            hc_probe_link(hc_ctx, node, i, j);
            if (hc_ctx->probes[i].inflight >= HC_PROBE_INFLIGHT_HI) {
                break;
            }
        }
    }
    pthread_rwlock_unlock(&node->lock);
}

static void hc_probe_fn(bondp_worker_task_reason_t reason, void *arg)
{
    bondp_hc_ctx_t *hc_ctx = (bondp_hc_ctx_t *)arg;
    /* Cancelled (e.g. by bondp_hc_uninit) or already stopping: do not run
     * probes and, crucially, do not reschedule. Otherwise the next firing
     * accesses hc_ctx after it has been freed. */
    if (reason == BONDP_WORKER_TASK_CANCELED || hc_ctx == NULL ||
        atomic_load(&hc_ctx->stopping)) {
        return;
    }
    if (hc_ctx->node_num == 0) {
        return;
    }

    /* Drain pending completions before posting new probes. The worker thread
     * is single-threaded, so the jfce handler cannot run concurrently while
     * hc_probe_fn is posting. Without this drain, the probe SQ (depth
     * HC_PROBE_DEPTH) can overflow when a batch covers many nodes x paths,
     * causing urma_post_jetty_send_wr to return ENOMEM and leaving the SQ
     * wedged (completions never reaped). Draining here keeps the SQ below
     * capacity and processes ACK-timeout rebuilds promptly. */
    for (uint32_t i = 0; i < URMA_UBAGG_DEV_MAX_NUM; ++i) {
        hc_drain_probe_cq(&hc_ctx->probes[i]);
    }

    uint32_t probe_cnt = MIN(hc_ctx->cfg.probe_node_num, hc_ctx->node_num);
    uint32_t node_idx = hc_ctx->probe_cur_idx % hc_ctx->node_num;
    for (uint32_t i = 0; i < probe_cnt; ++i) {
        if (!hc_probe_sq_has_room(hc_ctx)) {
            /* All local probe SQs are at the throttle limit; the remaining nodes
             * will be probed on the next firing after the CQs are drained. */
            break;
        }
        bondp_hc_node_t *node = &hc_ctx->nodes[node_idx];
        hc_probe_node(hc_ctx, node);
        node_idx = (node_idx + 1) % hc_ctx->node_num;
    }
    hc_ctx->probe_cur_idx = node_idx;

    /* Reschedule next global probe only if not being torn down. The
     * stopping flag is checked again to close the window between a concurrent
     * uninit and this reschedule. */
    if (atomic_load(&hc_ctx->stopping)) {
        return;
    }
    bondp_worker_task_id_t task_id = 0;
    int ret = bondp_worker_schedule(hc_ctx->cfg.probe_interval_ms,
                                    hc_probe_fn, hc_ctx, &task_id);
    if (ret == 0) {
        hc_ctx->probe_task_id = task_id;
    } else {
        URMA_LOG_ERR("Failed to reschedule health probe task, ret=%d.\n", ret);
    }
}

static int hc_init_node(bondp_hc_node_t *node, const topo_map_t *topo_map, uint32_t node_idx)
{
    const bondp_topo_node_t *topo_node = &topo_map->topo_infos[node_idx];
    node->node_id = topo_node->node_id;
    node->node_idx = node_idx;
    ub_list_init(&node->tjetty_list);
    (void)memset(node->hc_tjetty, 0, sizeof(node->hc_tjetty));
    for (uint32_t i = 0; i < URMA_UBAGG_DEV_MAX_NUM; ++i) {
        for (uint32_t j = 0; j < URMA_UBAGG_DEV_MAX_NUM; ++j) {
            atomic_store(&node->valid[i][j], true);
        }
    }

    if (pthread_rwlock_init(&node->lock, NULL) != 0) {
        return -1;
    }
    return 0;
}

static void hc_destroy_node(bondp_hc_node_t *node)
{
    if (node == NULL) {
        return;
    }

    bondp_target_jetty_t *tjetty = NULL;
    bondp_target_jetty_t *next = NULL;
    UB_LIST_FOR_EACH_SAFE (tjetty, next, hc_entry, &node->tjetty_list) {
        ub_list_remove(&tjetty->hc_entry);
        tjetty->hc_registered = false;
        tjetty->hc_node_idx = 0;
    }

    pthread_rwlock_destroy(&node->lock);
}

static void hc_destroy_nodes(bondp_hc_ctx_t *hc_ctx)
{
    if (hc_ctx == NULL || hc_ctx->node_num == 0) {
        return;
    }

    for (uint32_t i = 0; i < hc_ctx->node_num; ++i) {
        hc_destroy_node(&hc_ctx->nodes[i]);
    }
    hc_ctx->node_num = 0;
}

static int hc_init_nodes(bondp_context_t *bdp_ctx, bondp_hc_ctx_t *hc_ctx, bool *has_nodes)
{
    uint32_t init_node_num = 0;

    *has_nodes = false;
    if (bdp_ctx->topo_map == NULL || bdp_ctx->topo_map->node_num == 0) {
        URMA_LOG_INFO("No topo node for health check, skip probe task.\n");
        return 0;
    }
    if (bdp_ctx->topo_map->node_num > MAX_NODE_NUM) {
        URMA_LOG_ERR("Invalid topo node num for health check, node_num=%u.\n", bdp_ctx->topo_map->node_num);
        return -1;
    }

    for (uint32_t i = 0; i < bdp_ctx->topo_map->node_num; ++i) {
        int ret = hc_init_node(&hc_ctx->nodes[i], bdp_ctx->topo_map, i);
        if (ret != 0) {
            URMA_LOG_ERR("Failed to init health check node, node_idx=%u.\n", i);
            goto ERR_DESTROY_NODES;
        }
        init_node_num++;
    }

    hc_ctx->node_num = bdp_ctx->topo_map->node_num;
    *has_nodes = true;
    return 0;

ERR_DESTROY_NODES:
    for (uint32_t i = 0; i < init_node_num; ++i) {
        hc_destroy_node(&hc_ctx->nodes[i]);
    }
    hc_ctx->node_num = 0;
    return -1;
}

static void hc_destroy_probe_resource(bondp_hc_ctx_t *hc_ctx, int local_idx)
{
    bondp_probe_res_t *res = &hc_ctx->probes[local_idx];

    if (res->jfce != NULL) {
        (void)bondp_worker_del_fd(res->jfce_fd);
    }

    if (res->jetty != NULL) {
        urma_delete_jetty(res->jetty);
        res->jetty = NULL;
    }
    if (res->jfr != NULL) {
        urma_delete_jfr(res->jfr);
        res->jfr = NULL;
    }
    if (res->seg != NULL) {
        urma_unregister_seg(res->seg);
        res->seg = NULL;
    }
    if (res->buf != NULL) {
        free(res->buf);
        res->buf = NULL;
    }
    if (res->jfc != NULL) {
        urma_delete_jfc(res->jfc);
        res->jfc = NULL;
    }
    if (res->jfce != NULL) {
        urma_delete_jfce(res->jfce);
        res->jfce = NULL;
    }
    *res = (bondp_probe_res_t){0};
}

/* Allocate the jfce, jfc and jfr used by one probe path. On success stores
 * them in *jfce_out / *jfc_out / *jfr_out and returns 0; on failure returns -1
 * after freeing whatever was already allocated. Extracted from
 */
static int hc_create_probe_cq_chain(urma_context_t *p_ctx, int local_idx,
                                    urma_jfce_t **jfce_out, urma_jfc_t **jfc_out, urma_jfr_t **jfr_out)
{
    urma_jfce_t *jfce = urma_create_jfce(p_ctx);
    if (jfce == NULL) {
        URMA_LOG_ERR("Failed to create health probe jfce, local_idx=%d.\n", local_idx);
        return -1;
    }

    urma_jfc_cfg_t jfc_cfg = {
        .depth = 4096,
        .jfce = jfce,
    };
    urma_jfc_t *jfc = urma_create_jfc(p_ctx, &jfc_cfg);
    if (jfc == NULL) {
        URMA_LOG_ERR("Failed to create health probe jfc, local_idx=%d.\n", local_idx);
        urma_delete_jfce(jfce);
        return -1;
    }

    urma_jfr_cfg_t jfr_cfg = {
        .depth = 1,
        .trans_mode = URMA_TM_RM,
        .max_sge = 1,
        .jfc = jfc,
    };
    urma_jfr_t *jfr = urma_create_jfr(p_ctx, &jfr_cfg);
    if (jfr == NULL) {
        URMA_LOG_ERR("Failed to create health probe jfr, local_idx=%d.\n", local_idx);
        urma_delete_jfc(jfc);
        urma_delete_jfce(jfce);
        return -1;
    }

    *jfce_out = jfce;
    *jfc_out = jfc;
    *jfr_out = jfr;
    return 0;
}

/* Allocate and register the probe payload buffer + segment. On success stores
 * them in *buf_out / *seg_out and returns 0; on failure returns -1 after
 * freeing whatever was already allocated. Extracted from
 * hc_init_probe_resource (G.FUD.05). */
static int hc_create_probe_seg(urma_context_t *p_ctx, int local_idx, void **buf_out,
                               urma_target_seg_t **seg_out)
{
    void *buf = memalign(getpagesize(), HC_PROBE_SEG_LEN);
    if (buf == NULL) {
        URMA_LOG_ERR("Failed to alloc health probe buf, local_idx=%d.\n", local_idx);
        return -1;
    }

    urma_seg_cfg_t seg_cfg = {
        .va = (uint64_t)buf,
        .len = HC_PROBE_SEG_LEN,
        .flag = {
            .bs.token_policy = URMA_TOKEN_NONE,
            .bs.cacheable = URMA_NON_CACHEABLE,
            .bs.access = URMA_ACCESS_WRITE | URMA_ACCESS_READ,
        },
    };
    urma_target_seg_t *seg = urma_register_seg(p_ctx, &seg_cfg);
    if (seg == NULL) {
        URMA_LOG_ERR("Failed to register health probe seg, local_idx=%d.\n", local_idx);
        free(buf);
        return -1;
    }

    *buf_out = buf;
    *seg_out = seg;
    return 0;
}

static int hc_init_probe_resource(bondp_hc_ctx_t *hc_ctx, urma_context_t *p_ctx, bondp_probe_res_t *res)
{
    int local_idx = res->local_idx;

    urma_jfce_t *jfce = NULL;
    urma_jfc_t *jfc = NULL;
    urma_jfr_t *jfr = NULL;
    if (hc_create_probe_cq_chain(p_ctx, local_idx, &jfce, &jfc, &jfr) != 0) {
        return -1;
    }

    void *buf = NULL;
    urma_target_seg_t *seg = NULL;
    if (hc_create_probe_seg(p_ctx, local_idx, &buf, &seg) != 0) {
        goto DELETE_CQ_CHAIN;
    }

    urma_jetty_t *jetty = hc_create_probe_jetty(p_ctx, jfc, jfr, local_idx);
    if (jetty == NULL) {
        goto FREE_PROBE_SEG;
    }

    res->hc_ctx = hc_ctx;
    if (bondp_worker_add_fd(jfce->fd, hc_jfce_handler, res) != 0) {
        URMA_LOG_ERR("Failed to add health probe jfce fd to worker, local_idx=%d.\n", local_idx);
        goto DELETE_JETTY;
    }

    *res = (bondp_probe_res_t) {
        .hc_ctx = hc_ctx,
        .local_idx = local_idx,
        .buf = buf,
        .jfce = jfce,
        .jfc = jfc,
        .jfr = jfr,
        .seg = seg,
        .jetty = jetty,
        .jfce_fd = jfce->fd,
    };

    return 0;

DELETE_JETTY:
    urma_delete_jetty(jetty);
FREE_PROBE_SEG:
    urma_unregister_seg(seg);
    free(buf);
DELETE_CQ_CHAIN:
    urma_delete_jfr(jfr);
    urma_delete_jfc(jfc);
    urma_delete_jfce(jfce);
    return -1;
}

static void hc_init_cfg(bondp_hc_ctx_t *hc_ctx, const bondp_hc_cfg_t *cfg)
{
    hc_ctx->cfg.probe_interval_ms = BONDP_HC_DEFAULT_PROBE_INTERVAL_MS;
    hc_ctx->cfg.probe_node_num = BONDP_HC_DEFAULT_PROBE_NODE_NUM;

    if (cfg == NULL) {
        return;
    }

    if (cfg->probe_interval_ms != 0) {
        hc_ctx->cfg.probe_interval_ms = cfg->probe_interval_ms;
    }
    if (cfg->probe_node_num != 0) {
        hc_ctx->cfg.probe_node_num = cfg->probe_node_num;
    }
}

static void hc_destroy_probe_resources(bondp_hc_ctx_t *hc_ctx)
{
    for (uint32_t i = 0; i < URMA_UBAGG_DEV_MAX_NUM; ++i) {
        hc_destroy_probe_resource(hc_ctx, (int)i);
    }
}

static int hc_init_probe_resources(bondp_context_t *bdp_ctx, bondp_hc_ctx_t *hc_ctx, bool *has_probe_res)
{
    bool has_res = false;

    *has_probe_res = false;

    for (uint32_t i = 0; i < URMA_UBAGG_DEV_MAX_NUM; ++i) {
        urma_context_t *p_ctx = bdp_ctx->p_ctxs[i];
        if (p_ctx == NULL) {
            continue;
        }
        hc_ctx->probes[i].local_idx = i;
        int ret = hc_init_probe_resource(hc_ctx, p_ctx, &hc_ctx->probes[i]);
        if (ret != 0) {
            URMA_LOG_ERR("Failed to create health probe resources, local_idx=%u.\n", i);
            goto ERR_DESTROY_PROBES;
        }
        has_res = true;
    }

    if (!has_res) {
        URMA_LOG_INFO("No health probe resource, skip probe task.\n");
        hc_destroy_probe_resources(hc_ctx);
        return 0;
    }

    *has_probe_res = true;
    return 0;

ERR_DESTROY_PROBES:
    hc_destroy_probe_resources(hc_ctx);
    return -1;
}

static bondp_hc_ctx_t *hc_create_ctx(bondp_context_t *bdp_ctx, const bondp_hc_cfg_t *cfg)
{
    bondp_hc_ctx_t *hc_ctx = (bondp_hc_ctx_t *)calloc(1, sizeof(bondp_hc_ctx_t));
    if (hc_ctx == NULL) {
        URMA_LOG_ERR("Failed to alloc health check context.\n");
        return NULL;
    }
    hc_init_cfg(hc_ctx, cfg);
    hc_ctx->bdp_ctx = bdp_ctx;
    URMA_LOG_INFO("Health check config, probe_interval=%lums, probe_node_num=%u.\n",
                  hc_ctx->cfg.probe_interval_ms, hc_ctx->cfg.probe_node_num);
    return hc_ctx;
}

int bondp_hc_init(bondp_context_t *bdp_ctx, const bondp_hc_cfg_t *cfg)
{
    int ret;

    if (bdp_ctx == NULL) {
        return -EINVAL;
    }
    if (bdp_ctx->hc_ctx != NULL) {
        return 0;
    }

    bondp_hc_ctx_t *hc_ctx = hc_create_ctx(bdp_ctx, cfg);
    if (hc_ctx == NULL) {
        return -1;
    }

    bool has_nodes = false;
    ret = hc_init_nodes(bdp_ctx, hc_ctx, &has_nodes);
    if (ret != 0) {
        goto ERR_FREE_CTX;
    }
    if (!has_nodes) {
        URMA_LOG_INFO("No topo node for health check, skip mounting context.\n");
        goto ERR_FREE_CTX;
    }

    bool has_probe_res = false;
    ret = hc_init_probe_resources(bdp_ctx, hc_ctx, &has_probe_res);
    if (ret != 0) {
        goto ERR_DESTROY_NODES;
    }
    if (!has_probe_res) {
        URMA_LOG_INFO("No health probe resource, skip mounting context.\n");
        goto ERR_DESTROY_NODES;
    }

    bondp_worker_task_id_t task_id = 0;
    ret = bondp_worker_schedule(hc_ctx->cfg.probe_interval_ms,
                                hc_probe_fn, hc_ctx, &task_id);
    if (ret != 0) {
        URMA_LOG_ERR("Failed to schedule health probe task, ret=%d.\n", ret);
        goto ERR_DESTROY_PROBES;
    }
    hc_ctx->probe_task_id = task_id;
    URMA_LOG_INFO("Health probe task scheduled, interval=%lums, node_num=%u.\n",
                  hc_ctx->cfg.probe_interval_ms, hc_ctx->node_num);

    bdp_ctx->hc_ctx = hc_ctx;
    return 0;

ERR_DESTROY_PROBES:
    hc_destroy_probe_resources(hc_ctx);
ERR_DESTROY_NODES:
    hc_destroy_nodes(hc_ctx);
ERR_FREE_CTX:
    free(hc_ctx);
    return ret;
}

void bondp_hc_uninit(bondp_context_t *bdp_ctx)
{
    if (bdp_ctx == NULL || bdp_ctx->hc_ctx == NULL) {
        return;
    }

    bondp_hc_ctx_t *hc_ctx = bdp_ctx->hc_ctx;
    bdp_ctx->hc_ctx = NULL;

    /* Signal the probe task to stop running/rescheduling before cancelling it.
     * bondp_worker_cancel may invoke a concurrent hc_probe_fn(EXECUTED) to
     * completion (which would otherwise reschedule a new task that escapes
     * the cancel and fires after hc_ctx is freed). */
    atomic_store(&hc_ctx->stopping, true);

    if (hc_ctx->probe_task_id != 0) {
        (void)bondp_worker_cancel(hc_ctx->probe_task_id);
        hc_ctx->probe_task_id = 0;
        URMA_LOG_INFO("Health probe task cancelled.\n");
    }

    hc_destroy_probe_resources(hc_ctx);
    hc_destroy_nodes(hc_ctx);

    free(hc_ctx);
    URMA_LOG_INFO("Health check resources cleaned up.\n");
}

static int hc_find_node_idx_by_eid(topo_map_t *topo_map, const urma_eid_t *eid, uint32_t *node_idx)
{
    if (topo_map == NULL || eid == NULL || node_idx == NULL) {
        return -1;
    }

    for (uint32_t i = 0; i < topo_map->node_num; ++i) {
        bondp_topo_node_t *node = &topo_map->topo_infos[i];

        for (int d = 0; d < DEV_NUM; ++d) {
            bondp_topo_agg_dev_t *dev = &node->agg_devs[d];
            if (is_empty_eid((urma_eid_t *)(dev->agg_eid))) {
                continue;
            }
            if (!memcmp(dev->agg_eid, eid, EID_LEN)) {
                *node_idx = i;
                return 0;
            }
        }
    }

    return -1;
}

static bool hc_tjetty_has_probe_path(const bondp_target_jetty_t *bdp_tjetty)
{
    for (uint32_t i = 0; i < URMA_UBAGG_DEV_MAX_NUM; ++i) {
        for (uint32_t j = 0; j < URMA_UBAGG_DEV_MAX_NUM; ++j) {
            if (bdp_tjetty->p_tjetty[i][j] != NULL) {
                return true;
            }
        }
    }
    return false;
}

static void hc_register_tjetty_path(bondp_hc_node_t *node, bondp_target_jetty_t *bdp_tjetty)
{
    for (uint32_t i = 0; i < URMA_UBAGG_DEV_MAX_NUM; ++i) {
        for (uint32_t j = 0; j < URMA_UBAGG_DEV_MAX_NUM; ++j) {
            if (bdp_tjetty->p_tjetty[i][j] == NULL || node->hc_tjetty[i][j] != NULL) {
                continue;
            }
            node->hc_tjetty[i][j] = bdp_tjetty;
        }
    }
}

static bondp_target_jetty_t *hc_find_tjetty_for_path(bondp_hc_node_t *node, uint32_t local_idx,
                                                     uint32_t target_idx)
{
    bondp_target_jetty_t *tjetty = NULL;

    UB_LIST_FOR_EACH (tjetty, hc_entry, &node->tjetty_list) {
        if (tjetty->p_tjetty[local_idx][target_idx] != NULL) {
            return tjetty;
        }
    }
    return NULL;
}

static void hc_unregister_tjetty_path(bondp_hc_node_t *node, bondp_target_jetty_t *bdp_tjetty)
{
    for (uint32_t i = 0; i < URMA_UBAGG_DEV_MAX_NUM; ++i) {
        for (uint32_t j = 0; j < URMA_UBAGG_DEV_MAX_NUM; ++j) {
            if (node->hc_tjetty[i][j] != bdp_tjetty) {
                continue;
            }
            node->hc_tjetty[i][j] = hc_find_tjetty_for_path(node, i, j);
        }
    }
}

int bondp_hc_register_tjetty(bondp_context_t *bdp_ctx, bondp_target_jetty_t *bdp_tjetty)
{
    if (bdp_ctx == NULL || bdp_tjetty == NULL || bdp_tjetty->hc_registered) {
        return -1;
    }
    if (bdp_ctx->hc_ctx == NULL) {
        return 0;
    }

    bondp_hc_ctx_t *hc_ctx = bdp_ctx->hc_ctx;

    const urma_eid_t *dst_eid = &bdp_tjetty->v_tjetty.id.eid;
    uint32_t node_idx = 0;
    if (hc_find_node_idx_by_eid(bdp_ctx->topo_map, dst_eid, &node_idx) != 0) {
        URMA_LOG_WARN("Failed to resolve node id from eid, skip health check registration.\n");
        return 0;
    }
    bondp_hc_node_t *node = &hc_ctx->nodes[node_idx];

    pthread_rwlock_wrlock(&node->lock);
    if (hc_tjetty_has_probe_path(bdp_tjetty)) {
        bdp_tjetty->hc_registered = true;
        bdp_tjetty->hc_node_idx = node_idx;
        ub_list_push_back(&node->tjetty_list, &bdp_tjetty->hc_entry);
        hc_register_tjetty_path(node, bdp_tjetty);
    }
    pthread_rwlock_unlock(&node->lock);
    URMA_LOG_INFO("Health check tjetty registered, node_id=%u.\n", node->node_id);
    return 0;
}

void bondp_hc_unregister_tjetty(bondp_context_t *bdp_ctx, bondp_target_jetty_t *bdp_tjetty)
{
    if (bdp_ctx == NULL || bdp_ctx->hc_ctx == NULL ||
        bdp_tjetty == NULL || !bdp_tjetty->hc_registered) {
        return;
    }

    bondp_hc_ctx_t *hc_ctx = bdp_ctx->hc_ctx;
    uint32_t node_idx = bdp_tjetty->hc_node_idx;
    if (node_idx >= hc_ctx->node_num) {
        URMA_LOG_WARN("Invalid health check node idx, skip tjetty unregister, node_idx=%u, node_num=%u.\n",
                      node_idx, hc_ctx->node_num);
        bdp_tjetty->hc_registered = false;
        bdp_tjetty->hc_node_idx = 0;
        return;
    }
    bondp_hc_node_t *node = &hc_ctx->nodes[node_idx];

    pthread_rwlock_wrlock(&node->lock);
    ub_list_remove(&bdp_tjetty->hc_entry);
    hc_unregister_tjetty_path(node, bdp_tjetty);
    bdp_tjetty->hc_registered = false;
    bdp_tjetty->hc_node_idx = 0;
    pthread_rwlock_unlock(&node->lock);
    URMA_LOG_INFO("Health check tjetty unregistered, node_id=%u.\n", node->node_id);
}

bool bondp_hc_tjetty_path_valid(const bondp_target_jetty_t *bdp_tjetty,
                                uint32_t local_idx, uint32_t target_idx)
{
    if (bdp_tjetty == NULL ||
        local_idx >= URMA_UBAGG_DEV_MAX_NUM || target_idx >= URMA_UBAGG_DEV_MAX_NUM) {
        return false;
    }

    if (!bdp_tjetty->hc_registered) {
        return true;
    }

    bondp_context_t *bdp_ctx = CONTAINER_OF_FIELD(bdp_tjetty->v_tjetty.urma_ctx, bondp_context_t, v_ctx);
    bondp_hc_ctx_t *hc_ctx = bdp_ctx->hc_ctx;
    if (hc_ctx == NULL) {
        return true;
    }

    uint32_t node_idx = bdp_tjetty->hc_node_idx;
    if (node_idx >= hc_ctx->node_num) {
        return true;
    }

    bondp_hc_node_t *node = &hc_ctx->nodes[node_idx];
    return atomic_load(&node->valid[local_idx][target_idx]);
}

/* Called by the datapath failover CR handler (handle_send_cr_with_store) once it has
 * flipped bdp_comp->valid[send_idx]=false. The datapath has authoritatively observed a
 * failure on this local_idx; we mirror that into the HC state so the recovery gate
 * (hc_port_links_all_recovered) starts from this failure rather than waiting for an
 * independent probe failure — which may never come if the link recovers before the next
 * probe round. For every registered link sharing local_idx we set valid=false (so the
 * scheduler-side path_valid check also reflects the datapath verdict) and link_failed=true
 * (so a subsequent probe SUCCESS is required for each link before the port is revalidated).
 * node->lock is taken because hc_tjetty, valid and link_failed are otherwise only touched
 * on the single worker thread; the datapath calls this from a send-CR path on a different
 * thread. */
void bondp_hc_mark_pctx_failed(bondp_context_t *bdp_ctx, uint32_t local_idx)
{
    if (bdp_ctx == NULL || bdp_ctx->hc_ctx == NULL ||
        local_idx >= URMA_UBAGG_DEV_MAX_NUM) {
        return;
    }
    bondp_hc_ctx_t *hc_ctx = bdp_ctx->hc_ctx;
    for (uint32_t n = 0; n < hc_ctx->node_num; ++n) {
        bondp_hc_node_t *node = &hc_ctx->nodes[n];
        pthread_rwlock_wrlock(&node->lock);
        for (uint32_t t = 0; t < URMA_UBAGG_DEV_MAX_NUM; ++t) {
            if (node->hc_tjetty[local_idx][t] == NULL) {
                continue;
            }
            atomic_store(&node->valid[local_idx][t], false);
            node->link_failed[local_idx][t] = true;
        }
        pthread_rwlock_unlock(&node->lock);
    }
    URMA_LOG_INFO("Datapath failure marked for health recovery gate, local_idx=%u.\n", local_idx);
}
