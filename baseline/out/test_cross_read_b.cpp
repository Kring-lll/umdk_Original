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
TEST(test_cross_read_b, Run)
{
    ctx_t *c = NULL;
    char *dst = NULL;
    urma_target_seg_t *imp_tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0, i;
    urma_cr_t cr = {0};
    oob_t oob = {0};

    c = setup(1, URMA_TM_UM);
    if (!c) goto done;
    for (i = 0; i < 100; i++) {
        FILE *f = fopen(OOB_FILE, "rb");
        if (f) { fread(&oob, sizeof(oob), 1, f); fclose(f); break; }
        usleep(50000);
    }
    if (oob.token_id == 0) goto done;
    imp_tseg = import_seg_remote(c, oob.eid, oob.uasid, oob.token_id, oob.va, oob.len);
    if (!imp_tseg) goto done;
    /* WRITE_VAL(import_seg) no baseline */
    tj = import_jetty_remote(c, oob.eid, oob.uasid, c->jfs->jfs_id.id);
    if (!tj) goto done;
    /* WRITE_VAL(import_jetty) no baseline */
    dst = calloc(1, SEG_LEN); memset(dst, 'Z', SEG_LEN);
    ret = post_read(c, tj, dst, (void *)(uintptr_t)oob.va, imp_tseg, SEG_LEN, 0x42);
    /* WRITE_VAL(post_ret) no baseline */
    n = poll_cr(c, &cr);
    /* WRITE_VAL(poll_n) no baseline */
    /* WRITE_VAL(poll_status) no baseline */
    /* WRITE_HEX(poll_user_ctx) no baseline */
    /* WRITE_VAL(dst_first) no baseline */
    /* WRITE_STR(dst_buf) no baseline */
done:
    if (tj) urma_unimport_jetty(tj);
    if (imp_tseg) urma_unimport_seg(imp_tseg);
    free(dst);
    if (c) teardown(c);
}

