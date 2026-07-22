// SPDX-License-Identifier: MIT
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// 多 post 模板：连续 post N 个 READ，验 sim pi 回绕 + 全 poll 到。
// post 数 > 默认 depth 验证环形回绕不越界。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "bl_fixture.h"
#include "baseline_macros.h"

#define SEG_LEN 4096
#define NUM_POST 8

int main(void)
{
    ctx_t *c = NULL;
    char *src = NULL, *dst = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, i, got = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_UM);
    if (!c) goto cleanup;
    src = calloc(1, SEG_LEN); memset(src, 'A', SEG_LEN);
    tseg = register_seg(c, src, SEG_LEN);
    if (!tseg) goto cleanup;
    dst = calloc(1, SEG_LEN); memset(dst, 'Z', SEG_LEN);
    tj = import_self(c);
    if (!tj) goto cleanup;

    /* 连续 post NUM_POST 个 READ，每个 user_ctx 不同 */
    for (i = 0; i < NUM_POST; i++) {
        ret = post_read(c, tj, dst, src, tseg, SEG_LEN, 0x10 + i);
        if (ret != 0) break;
    }
    WRITE_VAL("post_all_ret", ret);
    WRITE_VAL("post_count", NUM_POST);

    /* 逐个 poll，验全拿到 */
    for (i = 0; i < NUM_POST; i++) {
        int n = poll_cr(c, &cr);
        if (n > 0 && cr.status == 0) got++;
        else break;
    }
    WRITE_VAL("poll_got", got);
    WRITE_VAL("poll_got_expect", NUM_POST);
    WRITE_VAL("dst_first", dst[0]);

cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
    return 0;
}
