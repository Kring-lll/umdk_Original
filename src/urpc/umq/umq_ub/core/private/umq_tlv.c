/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: UMQ tlv utils
 */

#include "umq_vlog.h"
#include "umq_tlv.h"

static bool umq_tlv_buf_validation(const char *buf, uint32_t buf_size)
{
    if (buf == NULL || buf_size < sizeof(umq_tlv_head_t)) {
        UMQ_VLOG_ERR("invalid arguments\n");
        return false;
    }

    if ((uint64_t)(uintptr_t)buf > UINT64_MAX - buf_size) {
        UMQ_VLOG_ERR("buffer range exceeds the upper limit\n");
        return false;
    }

    return true;
}

umq_tlv_head_t *umq_tlv_search_element(char *buf, uint32_t buf_size, umq_tlv_type_t type)
{
    if (!umq_tlv_buf_validation(buf, buf_size)) {
        return NULL;
    }

    uint32_t offset = 0;
    /* Assure next tlv head not exceeds buf_size */
    while (offset <= buf_size - sizeof(umq_tlv_head_t)) {
        umq_tlv_head_t *tlv_head = (umq_tlv_head_t *)(uintptr_t)(buf + offset);
        /* Assure current tlv element not exceeds buf_size:
         * 1. this tlv element is valid at least in length and user can parse this tlv element safely;
         * 2. offset can move safely by adding total length of current tlv element; */
        if (umq_tlv_get_total_len(tlv_head) > buf_size - offset) {
            return NULL;
        }

        if (tlv_head->type == (uint32_t)type) {
            return tlv_head;
        }

        offset += umq_tlv_get_total_len(tlv_head);
    }

    return NULL;
}

umq_tlv_head_t *umq_tlv_search_next_element(umq_tlv_head_t *cur, uint32_t left_size, umq_tlv_type_t type)
{
    if (!umq_tlv_buf_validation((char *)(uintptr_t)cur, left_size)) {
        return NULL;
    }

    /* Assure the next tlv head is within the range of provided buffer */
    uint32_t total_len = umq_tlv_get_total_len(cur);
    if (left_size - sizeof(umq_tlv_head_t) < total_len) {
        return NULL;
    }

    return umq_tlv_search_element((char *)(uintptr_t)umq_tlv_get_next_element(cur), left_size - total_len, type);
}