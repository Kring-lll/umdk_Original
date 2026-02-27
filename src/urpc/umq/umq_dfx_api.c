/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: realize func for umq dfx api
 * Create: 2026-2-4
 */

#include "umq_errno.h"
#include "umq_qbuf_pool.h"
#include "umq_huge_qbuf_pool.h"
#include "perf.h"
#include "umq_inner.h"
#include <stdarg.h>

#define UMQ_DFX_EQUALS "=================================================================================="
#define UMQ_DFX_UNDERLINE "----------------------------------------------------------------------------------"

// helper function to safely call snprintf
static int umq_safe_snprintf(char *str, size_t size, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int ret = vsnprintf(str, size > 0 ? size : 0, format, args);
    va_end(args);
    return ret;
}

int umq_stats_flow_control_get(uint64_t umqh, umq_flow_control_stats_t *flow_control_stats)
{
    umq_t *umq = (umq_t *)(uintptr_t)umqh;
    if (umq == NULL || umq->umqh_tp == UMQ_INVALID_HANDLE || umq->dfx_tp_ops == NULL ||
        umq->dfx_tp_ops->umq_tp_stats_flow_control_get == NULL || flow_control_stats == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "umqh or flow control stats parameter invalid\n");
        return -UMQ_ERR_EINVAL;
    }
    return umq->dfx_tp_ops->umq_tp_stats_flow_control_get(umq->umqh_tp, flow_control_stats);
}

int umq_flow_control_stats_to_str(const umq_flow_control_stats_t *flow_control_stats, char *buf, size_t buf_len)
{
    if (flow_control_stats == NULL || buf == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "parameters invalid\n");
        return -UMQ_ERR_EINVAL;
    }

    int str_size = 0;

    (void)memset(buf, 0, buf_len);

    // format flow control statistics header
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%s\n", UMQ_DFX_EQUALS);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "                               Flow Control Statistics\n");
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%s\n", UMQ_DFX_EQUALS);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "%-40s %-40s\n", "Type", "Value");

    // format pool credit statistics
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%s\n", UMQ_DFX_UNDERLINE);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "                               Pool Credit Statistics\n");
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%s\n", UMQ_DFX_UNDERLINE);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "%-40s %-40lu\n", "pool_idle", flow_control_stats->pool_credit.pool_idle);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "%-40s %-40lu\n", "pool_be_allocated", flow_control_stats->pool_credit.pool_be_allocated);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "%-40s %-40lu\n", "total_pool_idle", flow_control_stats->pool_credit.total_pool_idle);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "%-40s %-40lu\n", "total_pool_be_allocated", flow_control_stats->pool_credit.total_pool_be_allocated);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "%-40s %-40lu\n", "total_pool_post_rx_err", flow_control_stats->pool_credit.total_pool_post_rx_err);

    // format queue credit statistics
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%s\n", UMQ_DFX_UNDERLINE);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "                              Queue Credit Statistics\n");
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%s\n", UMQ_DFX_UNDERLINE);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "%-40s %-40lu\n", "queue_idle", flow_control_stats->queue_credit.queue_idle);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "%-40s %-40lu\n", "queue_be_allocated", flow_control_stats->queue_credit.queue_be_allocated);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "%-40s %-40lu\n", "queue_acquired", flow_control_stats->queue_credit.queue_acquired);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "%-40s %-40lu\n", "total_queue_idle", flow_control_stats->queue_credit.total_queue_idle);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "%-40s %-40lu\n", "total_queue_acquired", flow_control_stats->queue_credit.total_queue_acquired);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "%-40s %-40lu\n", "total_queue_be_allocated", flow_control_stats->queue_credit.total_queue_be_allocated);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "%-40s %-40lu\n", "total_queue_post_tx_success", flow_control_stats->queue_credit.total_queue_post_tx_success);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "%-40s %-40lu\n", "total_queue_post_tx_err", flow_control_stats->queue_credit.total_queue_post_tx_err);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "%-40s %-40lu\n", "total_queue_acquired_err", flow_control_stats->queue_credit.total_queue_acquired_err);

    // format packet statistics
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%s\n", UMQ_DFX_UNDERLINE);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "                              Packet Statistics\n");
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%s\n", UMQ_DFX_UNDERLINE);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "%-40s %-40lu\n", "send_cnt", flow_control_stats->packet_stats.send_cnt);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "%-40s %-40lu\n", "send_success", flow_control_stats->packet_stats.send_success);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "%-40s %-40lu\n", "recv_cnt", flow_control_stats->packet_stats.recv_cnt);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "%-40s %-40lu\n", "send_error_cnt", flow_control_stats->packet_stats.send_error_cnt);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "%-40s %-40lu\n", "recv_error_cnt", flow_control_stats->packet_stats.recv_error_cnt);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%s\n", UMQ_DFX_UNDERLINE);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%s\n", UMQ_DFX_EQUALS);

    return str_size;
}

int umq_stats_qbuf_pool_get(uint64_t umqh, umq_qbuf_pool_stats_t *qbuf_pool_stats)
{
    if (qbuf_pool_stats == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "qbuf pool stats stats parameter invalid\n");
        return -UMQ_ERR_EINVAL;
    }

    int ret;
    if (umqh == UMQ_INVALID_HANDLE) {
        qbuf_pool_stats->num = 0;
        ret = umq_qbuf_pool_info_get(qbuf_pool_stats);
        if (ret != UMQ_SUCCESS) {
            UMQ_VLOG_ERR(VLOG_UMQ, "umq qbuf pool info get failed\n");
            return ret;
        }

        ret = umq_huge_qbuf_pool_info_get(qbuf_pool_stats);
        if (ret != UMQ_SUCCESS) {
            UMQ_VLOG_ERR(VLOG_UMQ, "umq huge qbuf pool info get failed\n");
            return ret;
        }
        return UMQ_SUCCESS;
    }

    umq_t *umq = (umq_t *)(uintptr_t)umqh;
    if (umq == NULL || umq->umqh_tp == UMQ_INVALID_HANDLE || umq->dfx_tp_ops == NULL ||
        umq->dfx_tp_ops->umq_tp_stats_qbuf_pool_get == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "umqh or qbuf pool stats parameter invalid\n");
        return -UMQ_ERR_EINVAL;
    }

    return umq->dfx_tp_ops->umq_tp_stats_qbuf_pool_get(umq->umqh_tp, qbuf_pool_stats);
}

int umq_qbuf_pool_stats_to_str(const umq_qbuf_pool_stats_t *qbuf_pool_stats, char *buf, size_t buf_len)
{
    if (qbuf_pool_stats == NULL || buf == NULL || buf_len == 0) {
        UMQ_VLOG_ERR(VLOG_UMQ, "parameters invalid\n");
        return -UMQ_ERR_EINVAL;
    }

    int str_size = 0;

    // calculate total size and write to buffer in one pass
    (void)memset(buf, 0, buf_len);

    // format qbuf pool statistics
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%s\n", UMQ_DFX_EQUALS);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "                          Qbuf Pool Statistics\n");
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%s\n", UMQ_DFX_EQUALS);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%-40s %-40s\n", "Type", "Value");
    
    for (uint32_t i = 0; i < qbuf_pool_stats->num; i++) {
        const umq_qbuf_pool_info_t *info = &qbuf_pool_stats->qbuf_pool_info[i];
        
        // Add Pool Index as a subheading
        str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%s\n", UMQ_DFX_UNDERLINE);
        str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
            "                              Pool Index: %u\n", i);
        str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%s\n", UMQ_DFX_UNDERLINE);
        str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%-40s %-40s\n",
            "Mode", info->mode == UMQ_BUF_SPLIT ? "SPLIT" : "COMBINE");
        str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%-40s %-40lu\n",
            "Total Size", info->total_size);
        str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%-40s %-40lu\n",
            "Total Block Num", info->total_block_num);
        str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%-40s %-40u\n",
            "Block Size", info->block_size);
        str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%-40s %-40u\n",
            "Headroom Size", info->headroom_size);
        str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%-40s %-40u\n",
            "Data Size", info->data_size);
        str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%-40s %-40u\n",
            "Buf Size", info->buf_size);
        str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%-40s %-40u\n",
            "UMQ Buf T Size", info->umq_buf_t_size);
        
        if (info->mode == UMQ_BUF_SPLIT) {
            str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,"%-40s %lu blocks, %lu size\n", 
                "Available Mem - With Data",
                info->available_mem.split.block_num_with_data, info->available_mem.split.size_with_data);
            str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%-40s %lu blocks, %lu size\n", 
                "Available Mem - Without Data",
                info->available_mem.split.block_num_without_data, info->available_mem.split.size_without_data);
        } else {
            str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%-40s %lu blocks, %lu size\n", 
                "Available Mem - With Data",
                info->available_mem.combine.block_num_with_data, info->available_mem.combine.size_with_data);
        }
    }
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%s\n", UMQ_DFX_UNDERLINE);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%s\n", UMQ_DFX_EQUALS);

    return str_size;
}

int umq_info_get(uint64_t umqh, umq_info_t *umq_info)
{
    umq_t *umq = (umq_t *)(uintptr_t)umqh;
    if (umq == NULL || umq->umqh_tp == UMQ_INVALID_HANDLE || umq->dfx_tp_ops == NULL ||
        umq->dfx_tp_ops->umq_tp_info_get == NULL || umq_info == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "umqh or umq info parameter invalid\n");
        return -UMQ_ERR_EINVAL;
    }

    return umq->dfx_tp_ops->umq_tp_info_get(umq->umqh_tp, umq_info);
}

int umq_info_to_str(const umq_info_t *umq_info, char *buf, size_t buf_len)
{
    if (umq_info == NULL || buf == NULL || buf_len == 0) {
        UMQ_VLOG_ERR(VLOG_UMQ, "parameters invalid\n");
        return -UMQ_ERR_EINVAL;
    }

    int str_size = 0;

    // calculate total size and write to buffer in one pass
    (void)memset(buf, 0, buf_len);

    static const char trans_mode_map[UMQ_TRANS_MODE_MAX][20] = {
        [UMQ_TRANS_MODE_UB]        = "UB",
        [UMQ_TRANS_MODE_IB]        = "IB",
        [UMQ_TRANS_MODE_UCP]       = "UCP",
        [UMQ_TRANS_MODE_IPC]       = "IPC",
        [UMQ_TRANS_MODE_UBMM]      = "UBMM",
        [UMQ_TRANS_MODE_UB_PLUS]   = "UB_PLUS",
        [UMQ_TRANS_MODE_IB_PLUS]   = "IB_PLUS",
        [UMQ_TRANS_MODE_UBMM_PLUS] = "UB_PLUS",
    };

    // Format UMQ Info
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%s\n", UMQ_DFX_EQUALS);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "                               UMQ Info\n");
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%s\n", UMQ_DFX_EQUALS);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%-40s %-40s\n", "Info", "Value");
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%s\n", UMQ_DFX_UNDERLINE);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "%-40s %-40s\n", "Trans Mode", trans_mode_map[umq_info->trans_mode]);
    
    if (umq_info->trans_mode == UMQ_TRANS_MODE_UB || umq_info->trans_mode == UMQ_TRANS_MODE_UB_PLUS) {
        str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
            "%-40s %-40u\n", "UMQ ID", umq_info->ub.umq_id);
        str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%-40s %-40lu\n", "EID", umq_info->ub.eid);
        str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
            "%-40s %-40s\n", "Dev Name", umq_info->ub.dev_name);
        str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
            "%-40s %-40u\n", "Local IO Jetty ID", umq_info->ub.local_io_jetty_id);
        str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
            "%-40s %-40u\n", "Local FC Jetty ID", umq_info->ub.local_fc_jetty_id);
        str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
            "%-40s %-40u\n", "Remote IO Jetty ID", umq_info->ub.remote_io_jetty_id);
        str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
            "%-40s %-40u\n", "Remote FC Jetty ID", umq_info->ub.remote_fc_jetty_id);
    }

    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%s\n", UMQ_DFX_UNDERLINE);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%s\n", UMQ_DFX_EQUALS);
    return str_size;
}

int umq_stats_io_get(uint64_t umqh, umq_packet_stats_t *packet_stats)
{
    return UMQ_SUCCESS;
}

int umq_stats_io_reset(uint64_t umqh)
{
    return UMQ_SUCCESS;
}

int umq_stats_perf_get(umq_perf_stats_t *umq_perf_stats)
{
    return UMQ_SUCCESS;
}

int umq_stats_perf_reset(umq_perf_stats_cfg_t *perf_stats_cfg)
{
    return UMQ_SUCCESS;
}
