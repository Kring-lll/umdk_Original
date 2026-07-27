// SPDX-License-Identifier: MIT
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// FAA 模板。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "bl_fixture.h"
#include "baseline_macros.h"

#define SEG_LEN 4096

void test_faa(void)
{
    ctx_t *c = NULL;
    char *abuf = NULL, *obuf = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0;
    urma_cr_t cr = {0};

    c = setup(0, URMA_TM_UM);
    if (!c) goto cleanup;
    abuf = calloc(1, SEG_LEN); *(uint64_t *)abuf = 0x200;
    tseg = register_seg(c, abuf, SEG_LEN);
    if (!tseg) goto cleanup;
    obuf = calloc(1, 8);
    tj = import_self(c);
    if (!tj) goto cleanup;
    ret = post_faa(c, tj, abuf, tseg, obuf, 5, 8, 0xF1);
    WRITE_VAL("faa_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("faa_n", n);
    WRITE_VAL("faa_status", cr.status);
    WRITE_HEX("faa_atomic", *(uint64_t *)abuf);
    WRITE_HEX("faa_orig", *(uint64_t *)obuf);
cleanup:
    if (tj) urma_unimport_jetty(tj);
    free(abuf); free(obuf);
    if (c) teardown(c);
}

int main(void)
{
    test_faa();
    return 0;
}
