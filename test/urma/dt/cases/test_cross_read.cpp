/* SPDX-License-Identifier: MIT */

#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <unistd.h>
#include <sys/wait.h>
#include "dt_fixture.hpp"
#include "baseline_macros.h"

#define SEG_LEN 4096
#define OOB_FILE "/tmp/urma_cross_oob"
typedef struct {
    urma_eid_t eid;
    uint32_t uasid;
    uint32_t token_id;
    uint32_t jetty_id;
    uint64_t va;
    uint64_t len;
} oob_t;

static void do_a_side(void)
{
    dt_ctx_t *c = dt_setup_default(0);
    if (!c) return;
    char *buf = static_cast<char *>(calloc(1, SEG_LEN));
    memset(buf, 'A', SEG_LEN);
    urma_target_seg_t *tseg = dt_register_seg(c, buf, SEG_LEN);
    if (!tseg) {
        free(buf);
        dt_teardown(c);
        return;
    }
    oob_t oob = {};
    oob.eid = c->ctx->eid;
    oob.uasid = c->ctx->uasid;
    oob.token_id = tseg->seg.token_id;
    oob.jetty_id = dt_get_jetty_id(c);
    oob.va = (uint64_t)(uintptr_t)buf;
    oob.len = SEG_LEN;
    FILE *f = fopen(OOB_FILE, "wb");
    if (f) {
        fwrite(&oob, sizeof(oob), 1, f);
        fclose(f);
    }
    sleep(10);
    free(buf);
    dt_teardown(c);
}

TEST(test_cross_read, Run)
{
    dt_ctx_t *c = NULL;
    char *dst = NULL;
    urma_target_seg_t *dst_tseg = NULL;
    urma_target_seg_t *imp_tseg = NULL;
    urma_target_jetty_t *tj = NULL;
    int ret = -1;
    int n = 0;
    int i = 0;
    urma_cr_t cr = {};
    oob_t oob = {};

    unlink(OOB_FILE);
    pid_t pid = fork();
    if (pid == 0) {
        do_a_side();
        _exit(0);
    }

    sleep(1);
    c = dt_setup_default(1);
    ASSERT_NE(nullptr, c);
    if (!c) goto done;

    for (i = 0; i < 100; i++) {
        FILE *f = fopen(OOB_FILE, "rb");
        if (f) {
            fread(&oob, sizeof(oob), 1, f);
            fclose(f);
            break;
        }
        usleep(50000);
    }
    ASSERT_NE(0u, oob.token_id);
    if (oob.token_id == 0) goto done;

    imp_tseg = dt_import_seg_remote(c, oob.eid, oob.uasid, oob.token_id, oob.va, oob.len);
    ASSERT_NE(nullptr, imp_tseg);
    if (!imp_tseg) goto done;

    tj = dt_import_jetty_remote(c, oob.eid, oob.uasid, oob.jetty_id);
    ASSERT_NE(nullptr, tj);
    if (!tj) goto done;

    dst = static_cast<char *>(calloc(1, SEG_LEN));
    ASSERT_NE(nullptr, dst);
    memset(dst, 'Z', SEG_LEN);
    dst_tseg = dt_register_seg(c, dst, SEG_LEN);
    ASSERT_NE(nullptr, dst_tseg);

    ret = dt_post_read(c, tj, dst, dst_tseg, (void *)(uintptr_t)oob.va, imp_tseg, SEG_LEN, 0x42);
    EXPECT_EQ(0, ret);
    n = dt_poll_cr(c, &cr);
    EXPECT_EQ(1, n);
    EXPECT_EQ(0, cr.status);
    EXPECT_EQ(0x42, cr.user_ctx);
    EXPECT_EQ(65, dst[0]);
    EXPECT_EQ(0, strncmp("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", dst, strlen("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA")));

done:
    if (tj) urma_unimport_jetty(tj);
    if (imp_tseg) urma_unimport_seg(imp_tseg);
    free(dst);
    if (c) dt_teardown(c);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    unlink(OOB_FILE);
}
