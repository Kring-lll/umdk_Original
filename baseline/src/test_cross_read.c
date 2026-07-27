// SPDX-License-Identifier: MIT
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// 跨进程 READ 模板：A(udma_sim0) 注册 seg，B(udma_sim1) 跨进程读。
// 用 fork 在函数内部起 A，主进程跑 B。子函数帮 A 干活。
// gen_test 提取 void test_cross_read(void) 包成 TEST。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>
#include "bl_fixture.h"
#include "baseline_macros.h"

#define SEG_LEN 4096
#define OOB_FILE "/tmp/urma_cross_oob"

typedef struct { urma_eid_t eid; uint32_t uasid; uint32_t token_id; uint32_t jetty_id; uint64_t va; uint64_t len; } oob_t;

static void do_a_side(void)
{
    ctx_t *c = setup_default(0);
    if (!c) return;
    char *buf = calloc(1, SEG_LEN); memset(buf, 'A', SEG_LEN);
    urma_target_seg_t *tseg = register_seg(c, buf, SEG_LEN);
    if (!tseg) { free(buf); teardown(c); return; }
    oob_t oob = {0};
    oob.eid = c->ctx->eid;
    oob.uasid = c->ctx->uasid;
    oob.token_id = tseg->seg.token_id;
    oob.jetty_id = get_jetty_id(c);
    oob.va = (uint64_t)(uintptr_t)buf;
    oob.len = SEG_LEN;
    FILE *f = fopen(OOB_FILE, "wb");
    fwrite(&oob, sizeof(oob), 1, f);
    fclose(f);
    sleep(10);
    free(buf);
    teardown(c);
}

void test_cross_read(void)
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
    c = setup_default(1);
    if (!c) goto done;
    for (i = 0; i < 100; i++) {
        FILE *f = fopen(OOB_FILE, "rb");
        if (f) { fread(&oob, sizeof(oob), 1, f); fclose(f); break; }
        usleep(50000);
    }
    if (oob.token_id == 0) goto done;
    imp_tseg = import_seg_remote(c, oob.eid, oob.uasid, oob.token_id, oob.va, oob.len);
    if (!imp_tseg) goto done;
    WRITE_VAL("import_seg", 0);
    tj = import_jetty_remote(c, oob.eid, oob.uasid, oob.jetty_id);
    if (!tj) goto done;
    WRITE_VAL("import_jetty", 0);
    dst = calloc(1, SEG_LEN); memset(dst, 'Z', SEG_LEN);
    ret = post_read(c, tj, dst, (void *)(uintptr_t)oob.va, imp_tseg, SEG_LEN, 0x42);
    WRITE_VAL("post_ret", ret);
    n = poll_cr(c, &cr);
    WRITE_VAL("poll_n", n);
    WRITE_VAL("poll_status", cr.status);
    WRITE_HEX("poll_user_ctx", cr.user_ctx);
    WRITE_VAL("dst_first", dst[0]);
    WRITE_STR("dst_buf", dst);
done:
    if (tj) urma_unimport_jetty(tj);
    if (imp_tseg) urma_unimport_seg(imp_tseg);
    free(dst);
    if (c) teardown(c);
    waitpid(pid, NULL, 0);
    unlink(OOB_FILE);
}

int main(void)
{
    test_cross_read();
    return 0;
}
