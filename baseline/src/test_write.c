// SPDX-License-Identifier: MIT
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// WRITE 模板：void test_write_fn(void) + 简单 main。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "bl_fixture.h"
#include "baseline_macros.h"

#define SEG_LEN 4096

void test_write_fn(void)
{
    ctx_t *c = NULL;
    char *src = NULL, *dst = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_UM);
    if (!c) goto cleanup;
    dst = calloc(1, SEG_LEN); memset(dst, 'Z', SEG_LEN);
    tseg = register_seg(c, dst, SEG_LEN);
    if (!tseg) goto cleanup;
    src = calloc(1, SEG_LEN); memset(src, 'B', SEG_LEN);
    tj = import_self(c);
    if (!tj) goto cleanup;
    ret = post_write(c, tj, src, tseg, dst, SEG_LEN, 0x43);
    WRITE_VAL("post_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("poll_n", n);
    WRITE_VAL("poll_status", cr.status);
    WRITE_HEX("poll_user_ctx", cr.user_ctx);
    WRITE_VAL("dst_first", dst[0]);
    WRITE_STR("dst_buf", dst);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
}

int main(void)
{
    test_write_fn();
    return 0;
}
