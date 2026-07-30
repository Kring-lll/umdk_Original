// SPDX-License-Identifier: MIT
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// NOP 模板。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "bl_fixture.h"
#include "baseline_macros.h"

#define SEG_LEN 4096

void test_nop(void)
{
    ctx_t *c = NULL;
    char *buf = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    c = setup_default(0);
    if (!c) goto cleanup;
    buf = calloc(1, SEG_LEN); memset(buf, 'X', SEG_LEN);
    tseg = register_seg(c, buf, SEG_LEN);
    if (!tseg) goto cleanup;
    tj = import_self(c);
    if (!tj) goto cleanup;
    ret = post_rw(c, tj, URMA_OPC_NOP, buf, buf, tseg, SEG_LEN, 0x4E, 0, 0);
    WRITE_VAL("post_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("poll_n", n);
    WRITE_VAL("poll_status", cr.status);
    WRITE_HEX("poll_user_ctx", cr.user_ctx);
    WRITE_VAL("buf_first", buf[0]);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(buf);
    if (c) teardown(c);
}

int main(void)
{
    test_nop();
    return 0;
}
