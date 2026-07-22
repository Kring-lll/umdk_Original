// SPDX-License-Identifier: MIT
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
//
// 门禁模板（纯 C + bl_fixture + baseline_macros）：
// 用 fixture 建链 + WRITE_VAL/WRITE_STR 标注验证点。
// goto cleanup 统一释放——RAII 不适用于 C，用 goto 是 C 的 RAII 等价。
// 无 ok/fprintf（门禁靠 gen_test 生成的 EXPECT 断言判定）。
//
// 用法：gcc ... test_baseline_demo2.c bl_fixture.c -lurma_sim -lurma -lurma_common
//       ./a.out 2> out.txt    # BASELINE 行进 stderr
//       gen_baseline gen_test test_baseline_demo2.c expected.txt out/generated.cpp

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

    ret = post_read(c, tj, dst, src, tseg, SEG_LEN, 0x42);
    WRITE_VAL("post_ret", ret);

    n = poll_cr(c, &cr);
    WRITE_VAL("poll_n", n);
    WRITE_VAL("poll_status", cr.status);
    WRITE_HEX("poll_user_ctx", cr.user_ctx);
    WRITE_STR("dst_buf", dst);
    WRITE_VAL("dst_buf_first", dst[0]);

cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
    return 0;
}
