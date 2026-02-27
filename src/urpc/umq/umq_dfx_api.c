/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: realize func for umq dfx api
 * Create: 2026-2-4
 */

#include "umq_errno.h"
#include "umq_qbuf_pool.h"
#include "umq_huge_qbuf_pool.h"
#include "umq_inner.h"
#include <stdarg.h>

#define UMQ_DFX_EQUALS "=================================================================================="
#define UMQ_DFX_UNDERLINE "----------------------------------------------------------------------------------"
#define TRANS_MODE_NAME_SIZE 20

static int umq_safe_snprintf(char *str, int size, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int ret = vsnprintf(size > 0 ? str : NULL, size > 0 ? size : 0, format, args);
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

int umq_flow_control_stats_to_str(const umq_flow_control_stats_t *flow_control_stats, char *buf, int max_buf_len)
{
    if (flow_control_stats == NULL || buf == NULL || max_buf_len <= 0) {
        UMQ_VLOG_ERR(VLOG_UMQ, "parameters invalid\n");
        return -UMQ_ERR_EINVAL;
    }

    int str_size = 0;
    (void)memset(buf, 0, max_buf_len);

    // format flow control statistics header
    int ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%s\n", UMQ_DFX_EQUALS);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "                               Flow Control Statistics\n");
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%s\n", UMQ_DFX_EQUALS);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "%-40s %-40s\n", "Type", "Value");
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    // format pool credit statistics
    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%s\n", UMQ_DFX_UNDERLINE);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "                               Pool Credit Statistics\n");
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%s\n", UMQ_DFX_UNDERLINE);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "%-40s %-40lu\n", "Pool Idle", flow_control_stats->pool_credit.pool_idle);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "%-40s %-40lu\n", "Pool Allocated", flow_control_stats->pool_credit.pool_be_allocated);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "%-40s %-40lu\n", "Total Pool Idle", flow_control_stats->pool_credit.total_pool_idle);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "%-40s %-40lu\n", "Total Pool Allocated", flow_control_stats->pool_credit.total_pool_be_allocated);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "%-40s %-40lu\n", "Total Pool Post RX Err", flow_control_stats->pool_credit.total_pool_post_rx_err);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    // format queue credit statistics
    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%s\n", UMQ_DFX_UNDERLINE);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "                              Queue Credit Statistics\n");
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%s\n", UMQ_DFX_UNDERLINE);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "%-40s %-40lu\n", "Queue Idle", flow_control_stats->queue_credit.queue_idle);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "%-40s %-40lu\n", "Queue Allocated", flow_control_stats->queue_credit.queue_be_allocated);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "%-40s %-40lu\n", "Queue Acquired", flow_control_stats->queue_credit.queue_acquired);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "%-40s %-40lu\n", "Total Queue Idle", flow_control_stats->queue_credit.total_queue_idle);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "%-40s %-40lu\n", "Total Queue Acquired", flow_control_stats->queue_credit.total_queue_acquired);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "%-40s %-40lu\n", "Total Queue Allocated", flow_control_stats->queue_credit.total_queue_be_allocated);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "%-40s %-40lu\n", "Total Queue Post TX Success", flow_control_stats->queue_credit.total_queue_post_tx_success);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "%-40s %-40lu\n", "Total Queue Post TX Err", flow_control_stats->queue_credit.total_queue_post_tx_err);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "%-40s %-40lu\n", "Total Queue Acquired Err", flow_control_stats->queue_credit.total_queue_acquired_err);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    // format packet statistics
    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%s\n", UMQ_DFX_UNDERLINE);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "                              Packet Statistics\n");
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%s\n", UMQ_DFX_UNDERLINE);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "%-40s %-40lu\n", "Send Cnt", flow_control_stats->packet_stats.send_cnt);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "%-40s %-40lu\n", "Send Success", flow_control_stats->packet_stats.send_success);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "%-40s %-40lu\n", "Recv Cnt", flow_control_stats->packet_stats.recv_cnt);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "%-40s %-40lu\n", "Send Error Cnt", flow_control_stats->packet_stats.send_error_cnt);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "%-40s %-40lu\n", "Recv Error Cnt", flow_control_stats->packet_stats.recv_error_cnt);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%s\n", UMQ_DFX_UNDERLINE);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%s\n", UMQ_DFX_EQUALS);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    if (str_size >= max_buf_len) {
        // str_size does not include '\0'
        UMQ_VLOG_ERR(VLOG_UMQ, "flow control stats buf len insufficient, max_buf_len %d, expectation len %d\n",
            max_buf_len, str_size + 1);
        return -UMQ_ERR_EINVAL;
    }
    return str_size;

FORMAT_ERR:
    UMQ_VLOG_ERR(VLOG_UMQ, "format output failed, %d\n", ret);
    return ret;
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

int umq_qbuf_pool_stats_to_str(const umq_qbuf_pool_stats_t *qbuf_pool_stats, char *buf, int max_buf_len)
{
    if (qbuf_pool_stats == NULL || buf == NULL || max_buf_len <= 0) {
        UMQ_VLOG_ERR(VLOG_UMQ, "parameters invalid\n");
        return -UMQ_ERR_EINVAL;
    }

    int str_size = 0;
    (void)memset(buf, 0, max_buf_len);

    // format qbuf pool statistics
    int ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%s\n", UMQ_DFX_EQUALS);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "                          Qbuf Pool Statistics\n");
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%s\n", UMQ_DFX_EQUALS);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%-40s %-40s\n", "Type", "Value");
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;
    
    for (uint32_t i = 0; i < qbuf_pool_stats->num; i++) {
        const umq_qbuf_pool_info_t *info = &qbuf_pool_stats->qbuf_pool_info[i];
        
        // Add Pool Index as a subheading
        ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%s\n", UMQ_DFX_UNDERLINE);
        if (ret < 0) {
            goto FORMAT_ERR;
        }
        str_size += ret;

        ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
            "                              Pool Index: %u\n", i);
        if (ret < 0) {
            goto FORMAT_ERR;
        }
        str_size += ret;

        ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%s\n", UMQ_DFX_UNDERLINE);
        if (ret < 0) {
            goto FORMAT_ERR;
        }
        str_size += ret;

        ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%-40s %-40s\n",
            "Mode", info->mode == UMQ_BUF_SPLIT ? "SPLIT" : "COMBINE");
        if (ret < 0) {
            goto FORMAT_ERR;
        }
        str_size += ret;

        ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%-40s %-40lu\n",
            "Total Size", info->total_size);
        if (ret < 0) {
            goto FORMAT_ERR;
        }
        str_size += ret;

        ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%-40s %-40lu\n",
            "Total Block Num", info->total_block_num);
        if (ret < 0) {
            goto FORMAT_ERR;
        }
        str_size += ret;

        ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%-40s %-40u\n",
            "Block Size", info->block_size);
        if (ret < 0) {
            goto FORMAT_ERR;
        }
        str_size += ret;

        ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%-40s %-40u\n",
            "Headroom Size", info->headroom_size);
        if (ret < 0) {
            goto FORMAT_ERR;
        }
        str_size += ret;
    
        ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%-40s %-40u\n",
            "Data Size", info->data_size);
        if (ret < 0) {
            goto FORMAT_ERR;
        }
        str_size += ret;

        ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%-40s %-40u\n",
            "Buf Size", info->buf_size);
        if (ret < 0) {
            goto FORMAT_ERR;
        }
        str_size += ret;

        ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%-40s %-40u\n",
            "UMQ Buf Size", info->umq_buf_t_size);
        if (ret < 0) {
            goto FORMAT_ERR;
        }
        str_size += ret;

        if (info->mode == UMQ_BUF_SPLIT) {
            ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,"%-40s %lu blocks, %lu size\n", 
                "Available Mem - With Data",
                info->available_mem.split.block_num_with_data, info->available_mem.split.size_with_data);
            if (ret < 0) {
                goto FORMAT_ERR;
            }
            str_size += ret;

            ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%-40s %lu blocks, %lu size\n", 
                "Available Mem - Without Data",
                info->available_mem.split.block_num_without_data, info->available_mem.split.size_without_data);
            if (ret < 0) {
                goto FORMAT_ERR;
            }
            str_size += ret;
        } else {
            ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%-40s %lu blocks, %lu size\n", 
                "Available Mem - With Data",
                info->available_mem.combine.block_num_with_data, info->available_mem.combine.size_with_data);
            if (ret < 0) {
                goto FORMAT_ERR;
            }
            str_size += ret;
        }
    }

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%s\n", UMQ_DFX_UNDERLINE);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%s\n", UMQ_DFX_EQUALS);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    if (str_size >= max_buf_len) {
        // str_size does not include '\0'
        UMQ_VLOG_ERR(VLOG_UMQ, "qbuf pool stats buf len insufficientmax_buf_len %d, expectation len %d\n",
            max_buf_len, str_size + 1);
        return -UMQ_ERR_EINVAL;
    }
    return str_size;

FORMAT_ERR:
    UMQ_VLOG_ERR(VLOG_UMQ, "format output failed, %d\n", ret);
    return ret;
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

int umq_info_to_str(const umq_info_t *umq_info, char *buf, int max_buf_len)
{
    if (umq_info == NULL || buf == NULL || max_buf_len <= 0) {
        UMQ_VLOG_ERR(VLOG_UMQ, "parameters invalid\n");
        return -UMQ_ERR_EINVAL;
    }

    int str_size = 0;
    (void)memset(buf, 0, max_buf_len);

    static const char trans_mode_map[UMQ_TRANS_MODE_MAX][TRANS_MODE_NAME_SIZE] = {
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
    int ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%s\n", UMQ_DFX_EQUALS);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "                               UMQ Info\n");
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%s\n", UMQ_DFX_EQUALS);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%-40s %-40s\n", "Info", "Value");
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%s\n", UMQ_DFX_UNDERLINE);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
        "%-40s %-40s\n", "Trans Mode", trans_mode_map[umq_info->trans_mode]);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    if (umq_info->trans_mode == UMQ_TRANS_MODE_UB || umq_info->trans_mode == UMQ_TRANS_MODE_UB_PLUS) {
        ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
            "%-40s %-40u\n", "UMQ ID", umq_info->ub.umq_id);
        if (ret < 0) {
            goto FORMAT_ERR;
        }
        str_size += ret;

        ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
            "%-40s %-40lu\n", "EID", umq_info->ub.eid);
        if (ret < 0) {
            goto FORMAT_ERR;
        }
        str_size += ret;

        ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
            "%-40s %-40s\n", "Dev Name", umq_info->ub.dev_name);
        if (ret < 0) {
            goto FORMAT_ERR;
        }
        str_size += ret;

        ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
            "%-40s %-40u\n", "Local IO Jetty ID", umq_info->ub.local_io_jetty_id);
        if (ret < 0) {
            goto FORMAT_ERR;
        }
        str_size += ret;

        ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
            "%-40s %-40u\n", "Local FC Jetty ID", umq_info->ub.local_fc_jetty_id);
        if (ret < 0) {
            goto FORMAT_ERR;
        }
        str_size += ret;

        ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
            "%-40s %-40u\n", "Remote IO Jetty ID", umq_info->ub.remote_io_jetty_id);
        if (ret < 0) {
            goto FORMAT_ERR;
        }
        str_size += ret;

        ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size,
            "%-40s %-40u\n", "Remote FC Jetty ID", umq_info->ub.remote_fc_jetty_id);
        if (ret < 0) {
            goto FORMAT_ERR;
        }
        str_size += ret;
    }

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%s\n", UMQ_DFX_UNDERLINE);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    ret = umq_safe_snprintf(buf + str_size, max_buf_len - str_size, "%s\n", UMQ_DFX_EQUALS);
    if (ret < 0) {
        goto FORMAT_ERR;
    }
    str_size += ret;

    if (str_size >= max_buf_len) {
        // str_size does not include '\0'
        UMQ_VLOG_ERR(VLOG_UMQ, "umq info buf len insufficientmax_buf_len %d, expectation len %d\n",
            max_buf_len, str_size + 1);
        return -UMQ_ERR_EINVAL;
    }
    return str_size;

FORMAT_ERR:
    UMQ_VLOG_ERR(VLOG_UMQ, "format output failed, %d\n", ret);
    return ret;
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
