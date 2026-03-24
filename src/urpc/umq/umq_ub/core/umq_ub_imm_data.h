/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: UB imm data definition
 * Create: 2025-9-16
 */

#ifndef UMQ_UB_IMM_DATA_H
#define UMQ_UB_IMM_DATA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UMQ_UB_IMM_BITS (0xFFFFFFFFFFFFFFFF)
// in host byte order and bit order
#define UMQ_UB_IMM_WITHOUT_PRIVATE_BITS (0xFFFFFFFFFFFFFFFE)
#define UMQ_UB_IMM_PRIVATE 1
#define UMQ_UB_IMM_IN_USER_BUF 1  // user buffer with umq defined imm data

typedef enum umq_ub_imm_type {
    IMM_TYPE_UB_PLUS,               // used for ub plus imm type
    IMM_TYPE_FLOW_CONTROL,          // used for flow control window exchange
    IMM_TYPE_MEM,                   // used for import mem in ub mode
    IMM_TYPE_NOTIFY,                // used for notify

    IMM_TYPE_MAX,                   // max type should not exceed 32, for type is 5 bit
} umq_ub_imm_type_t;

typedef enum umq_ub_plus_imm_sub_type {
    IMM_TYPE_UB_PLUS_DEFAULT,       // used for default sub type in ub plus mode
    IMM_TYPE_REVERSE_PULL_MEM,      // used for reverse pull mem in ub plus mode
    IMM_TYPE_REVERSE_PULL_MEM_FREE, // used for free reverse pull mem in ub plus mode
    IMM_TYPE_REVERSE_PULL_MEM_DONE, // used for reverse pull mem done in ub plus mode

    IMM_TYPE_UB_PLUS_MAX,           // max type should not exceed 32, for type is 5 bit
} umq_ub_plus_imm_sub_type_t;

typedef enum umq_ub_import_mem_sub_type {
    IMM_TYPE_MEM_IMPORT,                // used for notify remote import mem in ub mode
    IMM_TYPE_MEM_IMPORT_DONE,           // used for record remoet import mem done
} umq_ub_import_mem_sub_type_t;

typedef enum umq_ub_flow_control_sub_type {
    IMM_TYPE_FC_CREDIT_INIT,
    IMM_TYPE_FC_CREDIT_REQ,               // request credit
    IMM_TYPE_FC_CREDIT_REP,               // reply credit request
    IMM_TYPE_FC_CREDIT_RETURN_REQ,        // request for credit refund
    IMM_TYPE_FC_CREDIT_RETURN_ACK,        // answer refund credit
} umq_ub_flow_control_sub_type_t;

typedef union umq_ub_imm {
    uint16_t value;
    struct {
        uint16_t type : 2;
        uint16_t rsvd1: 14;
    } bs;
    struct {
        uint16_t type : 2;
        uint16_t sub_type : 3;
        uint16_t msg_id : 5;
        uint16_t msg_num : 5;
    } ub_plus;
    struct {
        uint16_t type : 2;
        uint16_t sub_type : 2;
        uint16_t window : 10;
        uint16_t ratio : 2;     // 0: 10%, 1: 30%, 2: 50%, 3: 70%, min reserved credit: modify to 70%
    } flow_control;
    struct {
        uint16_t type : 2;
        uint16_t sub_type : 2;
        uint16_t mempool_id : 12;
    } mem_import;
    struct {
        uint16_t type : 2;
        uint16_t rsvd1 : 14;
    } notify;
} umq_ub_imm_t;

#ifdef __cplusplus
}
#endif

#endif
