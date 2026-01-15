/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: UMQ tlv utils
 * Create: 2025-01-09
 */
#ifndef UMQ_TLV
#define UMQ_TLV

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* umq tlv basic type ranges from 0 to 15(0xf)
 * umq tlv complex type ranges from 16 to 65535(0xffff) */
typedef enum umq_tlv_type {
    UMQ_TLV_TYPE_ARRAY = 0xf,
    UMQ_TLV_TYPE_BIND_INFO = 0x10,
} umq_tlv_type_t;

/** TLV head for general type
 * +--+--+--+--+--+--+--+--+
 * |Type(4B)               |
 * +--+--+--+--+--+--+--+--+
 * |Length(4B)             |
 * +--+--+--+--+--+--+--+--+
 * |Value                  |
 * +--+--+--+--+--+--+--+--+
 */
typedef struct umq_tlv_head {
    uint32_t type;
    uint32_t len;   // the length of 'value', not including 'umq_tlv_head_t'
    char value[0];
} umq_tlv_head_t;

/** TLV head for the array type(type: UMQ_TLV_TYPE_ARRAY)
 * +--+--+--+--+--+--+--+--+
 * |Type(4B)               |
 * +--+--+--+--+--+--+--+--+
 * |Length(4B)             | Length: Array Num + User Data
 * +--+--+--+--+--+--+--+--+
 * |[Value] Arr Num(4B)    |
 * +--+--+--+--+--+--+--+--+
 * |[Value] User Data      |
 * +--+--+--+--+--+--+--+--+
 */
typedef struct umq_tlv_arr_head {
    uint32_t type;
    uint32_t len;   // the length of 'value'(array num(4B) + user data), not including 'umq_tlv_head_t'.
    struct {
        uint32_t arr_num;
        char user_data[0];
    } value;
} umq_tlv_arr_head_t;

static inline umq_tlv_head_t *umq_tlv_get_next_element(umq_tlv_head_t *head)
{
    return (umq_tlv_head_t *)(head->value + head->len);
}

/* total length = sizeof(umq_tlv_head_t) + value length */
static inline uint32_t umq_tlv_get_total_len(umq_tlv_head_t *head)
{
    return sizeof(umq_tlv_head_t) + head->len;
}

/* Caller ensures the validity of 'type' */
umq_tlv_head_t *umq_tlv_search_element(char *buf, uint32_t buf_size, umq_tlv_type_t type);
umq_tlv_head_t *umq_tlv_search_next_element(umq_tlv_head_t *cur, uint32_t left_size, umq_tlv_type_t type);

/* tlv head obtained from 'umq_tlv_search_element' or 'umq_tlv_search_next_element' can call this function directly.
 * these two function have checked the tlv head and assure it's within the range of provided buffer. */
static inline uint32_t umq_tlv_get_left_len(char *buf, uint32_t buf_size, umq_tlv_head_t *cur)
{
    return buf_size - (uint32_t)(uintptr_t)((char *)(uintptr_t)cur - buf);
}

static inline uint32_t umq_tlv_arr_get_user_data_len(umq_tlv_arr_head_t *head)
{
    return head->len - sizeof(head->value);
}

static inline uint32_t umq_tlv_arr_get_value_len_by_user_data_len(uint32_t user_data_len)
{
    return user_data_len + sizeof(umq_tlv_arr_head_t) - sizeof(umq_tlv_head_t);
}

#ifdef __cplusplus
}
#endif

#endif