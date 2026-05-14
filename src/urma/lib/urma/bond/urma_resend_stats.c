/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2021-2025. All rights reserved.
 * Description: resend statistics
 * Author: Wang YiQuan
 * Create: 2026-05-13
 * Note:
 * History: 2026-05-13   Create File
 */

#include <stdatomic.h>
#include <stdint.h>

static atomic_uint g_urma_resend_cnt;

void urma_resend_init(void)
{
    atomic_init(&g_urma_resend_cnt, 0);
}

void urma_resend_inc(void)
{
    (void)atomic_fetch_add(&g_urma_resend_cnt, 1);
}

uint32_t urma_resend_get(void)
{
    return atomic_load(&g_urma_resend_cnt);
}
