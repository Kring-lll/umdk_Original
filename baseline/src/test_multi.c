// SPDX-License-Identifier: MIT
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// 多函数模板：一个文件里多个 test 函数，gen_test 给每个生成独立 TEST。
// main 里只有纯调用。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "bl_fixture.h"
#include "baseline_macros.h"

#define SEG_LEN 4096

void test_read(void)
{
    ctx_t *c = NULL;
    char *src = NULL, *dst = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_UM);
    if (!c) goto cleanup;
    src = calloc(1, SEG_LEN); memset(src, 'A', SEG_LEN);
    tseg = register_seg(c, src, SEG_LEN);
    if (!tseg) goto cleanup;
    dst = calloc(1, SEG_LEN); memset(dst, 'Z', SEG_LEN);
    tj = import_self(c);
    if (!tj) goto cleanup;
    ret = post_read(c, tj, dst, src, tseg, SEG_LEN, 0x42);
    WRITE_VAL("read_post_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("read_poll_n", n);
    WRITE_VAL("read_poll_status", cr.status);
    WRITE_HEX("read_user_ctx", cr.user_ctx);
    WRITE_VAL("read_dst_first", dst[0]);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
}

void test_write(void)
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
    WRITE_VAL("write_post_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("write_poll_n", n);
    WRITE_VAL("write_poll_status", cr.status);
    WRITE_HEX("write_user_ctx", cr.user_ctx);
    WRITE_VAL("write_dst_first", dst[0]);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(dst); free(src);
    if (c) teardown(c);
}

void test_nop(void)
{
    ctx_t *c = NULL;
    char *buf = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_UM);
    if (!c) goto cleanup;
    buf = calloc(1, SEG_LEN); memset(buf, 'X', SEG_LEN);
    tseg = register_seg(c, buf, SEG_LEN);
    if (!tseg) goto cleanup;
    tj = import_self(c);
    if (!tj) goto cleanup;
    ret = post_rw(c, tj, URMA_OPC_NOP, buf, buf, tseg, SEG_LEN, 0x4E, 0, 0);
    WRITE_VAL("nop_post_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("nop_poll_n", n);
    WRITE_VAL("nop_poll_status", cr.status);
    WRITE_HEX("nop_user_ctx", cr.user_ctx);
    WRITE_VAL("nop_buf_first", buf[0]);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(buf);
    if (c) teardown(c);
}

int main(void)
{
    test_read();
    test_write();
    test_nop();
    return 0;
}
