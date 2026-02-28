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

int umq_stats_io_get(uint64_t umqh, umq_packet_stats_t *packet_stats)
{
    umq_t *umq = (umq_t *)(uintptr_t)umqh;
    if (umq == NULL || umq->umqh_tp == UMQ_INVALID_HANDLE || umq->dfx_tp_ops == NULL ||
        umq->dfx_tp_ops->umq_tp_stats_io_get == NULL || packet_stats == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "umqh or packet stats parameter invalid\n");
        return -UMQ_ERR_EINVAL;
    }
    return umq->dfx_tp_ops->umq_tp_stats_io_get(umq->umqh_tp, packet_stats);
}

int umq_stats_io_reset(uint64_t umqh)
{
    umq_t *umq = (umq_t *)(uintptr_t)umqh;
    if (umq == NULL || umq->umqh_tp == UMQ_INVALID_HANDLE || umq->dfx_tp_ops == NULL ||
        umq->dfx_tp_ops->umq_tp_stats_io_reset == NULL) {
        UMQ_VLOG_ERR(VLOG_UMQ, "umqh parameter invalid\n");
        return -UMQ_ERR_EINVAL;
    }
    return umq->dfx_tp_ops->umq_tp_stats_io_reset(umq->umqh_tp);
}

int umq_io_stats_to_str(const umq_packet_stats_t *packet_stats, char *buf, size_t buf_len)
{
    if (packet_stats == NULL || buf == NULL || buf_len == 0) {
        UMQ_VLOG_ERR(VLOG_UMQ, "parameters invalid\n");
        return -UMQ_ERR_EINVAL;
    }

    int str_size = 0;

    (void)memset(buf, 0, buf_len);

    // Format IO Packet Statistics
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%s\n", UMQ_DFX_EQUALS);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size,
        "                              IO Packet Statistics\n");
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%s\n", UMQ_DFX_EQUALS);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%-40s %-40s\n", "Type", "Value");
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%s\n", UMQ_DFX_UNDERLINE);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%-40s %-40lu\n",
        "send_cnt", packet_stats->send_cnt);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%-40s %-40lu\n",
        "send_success", packet_stats->send_success);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%-40s %-40lu\n",
        "recv_cnt", packet_stats->recv_cnt);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%-40s %-40lu\n",
        "send_error_cnt", packet_stats->send_error_cnt);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%-40s %-40lu\n",
        "recv_error_cnt", packet_stats->recv_error_cnt);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%s\n", UMQ_DFX_UNDERLINE);
    str_size += umq_safe_snprintf(buf + str_size, buf_len - str_size, "%s\n", UMQ_DFX_EQUALS);

    return str_size;
}

int umq_stats_perf_get(umq_perf_stats_t *umq_perf_stats)
{
    return UMQ_SUCCESS;
}

int umq_stats_perf_reset(umq_perf_stats_cfg_t *perf_stats_cfg)
{
    return UMQ_SUCCESS;
}