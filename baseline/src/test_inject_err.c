// SPDX-License-Identifier: MIT
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
//
// 异常注入模板：注入 ACK_TIMEOUT error CQE，验真 udma 错误转换链。
// 跑时设 URMA_SIM_INJECT_STATUS=5（ACK_TIMEOUT=5）。
// 正常路径（不设注入）post_ret=0/poll_status=0；注入后 poll_status=9（ACK_TIMEOUT_ERR）。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "bl_fixture.h"
#include "baseline_macros.h"

#define SEG_LEN 4096

int main(void)
{
    ctx_t *c = NULL;
    char *src = NULL, *dst = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_UM);
    if (c == NULL) goto cleanup;

    src = calloc(1, SEG_LEN); memset(src, 'A', SEG_LEN);
    tseg = register_seg(c, src, SEG_LEN);
    if (tseg == NULL) goto cleanup;

    dst = calloc(1, SEG_LEN); memset(dst, 'Z', SEG_LEN);
    tj = import_self(c);
    if (tj == NULL) goto cleanup;

    ret = post_read(c, tj, dst, src, tseg, SEG_LEN, 0xE1);
    WRITE_VAL("post_ret", ret);

    n = poll_cr(c, &cr);
    WRITE_VAL("poll_n", n);
    WRITE_VAL("poll_status", cr.status);
    WRITE_HEX("poll_user_ctx", cr.user_ctx);

cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
    return 0;
}
