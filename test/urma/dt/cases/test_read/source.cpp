/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 */

// case 源样例：用 C++ fixture 建链，用 WRITE_* 宏标注需要固化的验证点。
// 该源文件用于生成 cases/test_read/case.cpp。

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "baseline_macros.h"
#include "bl_fixture.hpp"

#define SEG_LEN 4096

int main(void)
{
    ctx_t *c = NULL;
    char *src = NULL;
    char *dst = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1;
    int n = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_UM);
    if (c == NULL) {
        goto cleanup;
    }

    src = static_cast<char *>(calloc(1, SEG_LEN));
    memset(src, 'A', SEG_LEN);
    tseg = register_seg(c, src, SEG_LEN);
    if (tseg == NULL) {
        goto cleanup;
    }

    dst = static_cast<char *>(calloc(1, SEG_LEN));
    memset(dst, 'Z', SEG_LEN);
    tj = import_self(c);
    if (tj == NULL) {
        goto cleanup;
    }

    ret = post_read(c, tj, dst, src, tseg, SEG_LEN, 0x42);
    WRITE_VAL("post_ret", ret);

    n = poll_cr(c, &cr);
    WRITE_VAL("poll_n", n);
    WRITE_VAL("poll_status", cr.status);
    WRITE_HEX("poll_user_ctx", cr.user_ctx);
    WRITE_STR("dst_buf", dst);
    WRITE_VAL("dst_buf_first", dst[0]);

cleanup:
    if (tj) {
        urma_unimport_jetty(tj);
    }
    free(dst);
    free(src);
    if (c) {
        teardown(c);
    }
    return 0;
}
