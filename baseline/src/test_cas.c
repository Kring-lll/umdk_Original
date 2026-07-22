// SPDX-License-Identifier: MIT
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// 单进程 CAS 模板：匹配 swap + 不匹配不变。

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
    char *abuf = NULL, *obuf = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_UM);
    if (!c) goto cleanup;
    abuf = calloc(1, SEG_LEN); *(uint64_t *)abuf = 0x100;
    tseg = register_seg(c, abuf, SEG_LEN);
    if (!tseg) goto cleanup;
    obuf = calloc(1, 8);
    tj = import_self(c);
    if (!tj) goto cleanup;

    /* case 1: CAS 匹配 (0x100→0x200) */
    ret = post_cas(c, tj, abuf, tseg, obuf, 0x100, 0x200, 8, 0xC1);
    WRITE_VAL("cas1_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("cas1_n", n);
    WRITE_VAL("cas1_status", cr.status);
    WRITE_HEX("cas1_atomic", *(uint64_t *)abuf);
    WRITE_HEX("cas1_orig", *(uint64_t *)obuf);

    /* case 2: CAS 不匹配 (atomic=0x200, cmp=0xFFF) */
    *(uint64_t *)obuf = 0;
    ret = post_cas(c, tj, abuf, tseg, obuf, 0xFFF, 0xAAA, 8, 0xC2);
    WRITE_VAL("cas2_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("cas2_n", n);
    WRITE_VAL("cas2_status", cr.status);
    WRITE_HEX("cas2_atomic", *(uint64_t *)abuf);
    WRITE_HEX("cas2_orig", *(uint64_t *)obuf);

cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(abuf); free(obuf);
    if (c) teardown(c);
    return 0;
}
