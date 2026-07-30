// SPDX-License-Identifier: MIT
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// 多 POST 模板。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "bl_fixture.h"
#include "baseline_macros.h"

#define SEG_LEN 4096

void test_multi_post(void)
{
    ctx_t *c = NULL;
    char *src = NULL, *dst = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, i, got = 0;
    urma_cr_t cr = {0};

    c = setup_default(0);
    if (!c) goto cleanup;
    src = calloc(1, SEG_LEN); memset(src, 'A', SEG_LEN);
    tseg = register_seg(c, src, SEG_LEN);
    if (!tseg) goto cleanup;
    dst = calloc(1, SEG_LEN); memset(dst, 'Z', SEG_LEN);
    tj = import_self(c);
    if (!tj) goto cleanup;
    for (i = 0; i < 8; i++) {
        ret = post_read(c, tj, dst, src, tseg, SEG_LEN, 0x10 + i);
        if (ret != 0) break;
    }
    WRITE_VAL("post_all_ret", ret);
    WRITE_VAL("post_count", 8);
    for (i = 0; i < 8; i++) {
        int n = poll_cr(c, &cr);
        if (n > 0 && cr.status == 0) got++;
        else break;
    }
    WRITE_VAL("poll_got", got);
    WRITE_VAL("dst_first", dst[0]);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
}

int main(void)
{
    test_multi_post();
    return 0;
}
