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
static void do_a_side(void)
{
    ctx_t *c = setup(0, URMA_TM_UM);
    if (!c) return;
    char *buf = calloc(1, SEG_LEN); memset(buf, 'A', SEG_LEN);
    urma_target_seg_t *tseg = register_seg(c, buf, SEG_LEN);
    if (!tseg) { free(buf); teardown(c); return; }
    oob_t oob = {0};
    oob.eid = c->ctx->eid;
    oob.uasid = c->ctx->uasid;
    oob.token_id = tseg->seg.token_id;
    oob.va = (uint64_t)(uintptr_t)buf;
    oob.len = SEG_LEN;
    FILE *f = fopen(OOB_FILE, "wb");
    fwrite(&oob, sizeof(oob), 1, f);
    fclose(f);
    sleep(10);
    free(buf);
    teardown(c);
}

TEST(test_cross_read, Run)
{
    ctx_t *c = NULL;
    char *dst = NULL;
    urma_target_seg_t *imp_tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1, n = 0, i;
    urma_cr_t cr = {0};
    oob_t oob = {0};

    unlink(OOB_FILE);
    pid_t pid = fork();
    if (pid == 0) { do_a_side(); _exit(0); }

    sleep(3);
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
    EXPECT_EQ(0, 0);
    tj = import_jetty_remote(c, oob.eid, oob.uasid, c->jfs->jfs_id.id);
    if (!tj) goto done;
    EXPECT_EQ(0, 0);
    dst = calloc(1, SEG_LEN); memset(dst, 'Z', SEG_LEN);
    ret = post_read(c, tj, dst, (void *)(uintptr_t)oob.va, imp_tseg, SEG_LEN, 0x42);
    EXPECT_EQ(0, ret);
    n = poll_cr(c, &cr);
    EXPECT_EQ(1, n);
    EXPECT_EQ(0, cr.status);
    EXPECT_EQ(0x42, cr.user_ctx);
    EXPECT_EQ(65, dst[0]);
    EXPECT_EQ(0, strncmp("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", dst, strlen("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA")));
done:
    if (tj) urma_unimport_jetty(tj);
    if (imp_tseg) urma_unimport_seg(imp_tseg);
    free(dst);
    if (c) teardown(c);
    waitpid(pid, NULL, 0);
    unlink(OOB_FILE);
}

