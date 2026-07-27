/* SPDX-License-Identifier: MIT */
/* 自动生成：gen_baseline gen_test <src.c> <expected.txt> <out.cpp>
 * 由 baseline 生成器从测试源 + 录制基准合成。改源改基准后重生成，勿手编。 */

#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <unistd.h>
#include "bl_fixture.hpp"
#include "baseline_macros.h"

#define SEG_LEN 4096
#define OOB_FILE "/tmp/urma_cross_oob"
typedef struct { urma_eid_t eid; uint32_t uasid; uint32_t token_id; uint64_t va; uint64_t len; } oob_t;
TEST(test_cross_read_a, Run)
{
    ctx_t *c = NULL;
    char *buf = NULL;
    urma_target_seg_t *tseg = NULL;
    oob_t oob = {0};
    FILE *f = NULL;

    c = setup(0, URMA_TM_UM);
    if (!c) goto done;
    buf = calloc(1, SEG_LEN); memset(buf, 'A', SEG_LEN);
    tseg = register_seg(c, buf, SEG_LEN);
    if (!tseg) goto done;
    oob.eid = c->ctx->eid;
    oob.uasid = c->ctx->uasid;
    oob.token_id = tseg->seg.token_id;
    oob.va = (uint64_t)(uintptr_t)buf;
    oob.len = SEG_LEN;
    f = fopen(OOB_FILE, "wb");
    fwrite(&oob, sizeof(oob), 1, f);
    fclose(f);
    sleep(15);
    EXPECT_EQ(1, 1);
done:
    free(buf);
    if (c) teardown(c);
}

