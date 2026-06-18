/*
 * SPDX-License-Identifier: MT
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: ums example
 */

#include "../public.h"
#include <string>

using namespace std;

static int run_test(test_ums_ctx_t *ctx)
{
    int ret = 0;
    int rc = TEST_FAILED;
    int check_num_ums;
    int check_num_fallback;
    char ret_buf_0[40];
    char ret_buf_1[40];
    char ret_buf_2[40];
    char ret_buf_3[40];
    char test_ip_str[10]={0};
    char setup_env0[MAX_EXEC_CMD_RET_LEN];
    char setup_env1[MAX_EXEC_CMD_RET_LEN];
    char close_qperf[MAX_EXEC_CMD_RET_LEN];
    char recover_env[MAX_EXEC_CMD_RET_LEN];



    if (ctx->app_id == PROC_1) {
        char buf0[MAX_EXEC_CMD_RET_LEN];
        exec_cmd(setup_env0, MAX_EXEC_CMD_RET_LEN, "service ums_agent restart");
    }
    if (ctx->app_id == PROC_2) {
        char buf0[MAX_EXEC_CMD_RET_LEN];
        exec_cmd(setup_env1, MAX_EXEC_CMD_RET_LEN, "service ums_agent restart");
    }
    sync_time("----------------------------1");

    if (ctx->app_id == PROC_1) {
        char buf0[MAX_EXEC_CMD_RET_LEN];
        exec_cmd(buf0, MAX_EXEC_CMD_RET_LEN, "rmmod ums; modprobe ums ub_token_mode=2");
    }
    if (ctx->app_id == PROC_2) {
        char buf0[MAX_EXEC_CMD_RET_LEN];
        exec_cmd(buf1, MAX_EXEC_CMD_RET_LEN, "rmmod ums; modprobe ums ub_token_mode=2");
    }
    sync_time("----------------------------2");
    if (ctx->app_id == PROC_1) {
        char serv_cmd[MAX_EXEC_CMD_RET_LEN];
        exec_cmd(serv_cmd, MAX_EXEC_CMD_RET_LEN, "nohup ums_run qperf -lp %d &", ctx->test_port);
    }
    sync_time("----------------------------3");
    if (ctx->app_id == PROC_2) {
        char clnt_cmd[MAX_EXEC_CMD_RET_LEN];
        exec_cmd(clnt_cmd, MAX_EXEC_CMD_RET_LEN, "nohup ums_run qperf %s -lp %d -m 8192 -t 0 tcp_bw 2>&1 &", ctx->test_ip, ctx->test_port);
    }
    sync_time("----------------------------4");

    // 校验流量走ums
    if (ctx->app_id == PROC_2) {
        sprintf(test_ip_str, "%d", ctx->test_ip);
        check_num = query_proc_net_ums_detail_stream_num("False", test_ip_str);
        if (check_num != 2) {
            ret = -1;
        }
        CHKERR_JUMP(ret != TEST_SUCCESS, "ums single connect failed", EXIT);
    }
    exec_cmd(close_qperf, MAX_EXEC_CMD_RET_LEN, "pkill -9 qperf");
    exec_cmd(recover_env, MAX_EXEC_CMD_RET_LEN, "rmmod ums; modprobe ums; service ums_agent_start");

    rc = TEST_SUCCESS;
EXIT:
    sync_time("----------------------------2");
    return rc;
}

int main(int argc, char *argv[]) {
    int ret;
    test_ums_ctx_t *ctx = test_ums_ctx_init(argc, argv, 1);
    ret = run_test(ctx);
    destroy_test_ums_ctx(ctx);
    return ret;
}