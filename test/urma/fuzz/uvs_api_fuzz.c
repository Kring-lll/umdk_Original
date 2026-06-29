/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * Description: libuvs public API fuzz entry generated from uvs_api.h
 */

#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "uvs_api.h"

typedef int (*fuzz_entry_t)(void);

typedef struct fuzz_case {
    const char *name;
    fuzz_entry_t entry;
} fuzz_case_t;

static int WaitFuzzChild(pid_t pid, const char *case_name)
{
    int status = 0;

    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }

    if (WIFSIGNALED(status)) {
        fprintf(stderr, "%s terminated by signal %d\n", case_name, WTERMSIG(status));
        return 1;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "%s exited with status %d\n", case_name, status);
        return 1;
    }

    return 0;
}

static int RunFuzzCase(const fuzz_case_t *test_case)
{
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        (void)alarm(2);
        (void)test_case->entry();
        _exit(0);
    }

    return WaitFuzzChild(pid, test_case->name);
}

static int Fuzz_uvs_create_agg_dev(void)
{
    (void)uvs_create_agg_dev(NULL, NULL);
    return 0;
}

static int Fuzz_uvs_delete_agg_dev(void)
{
    (void)uvs_delete_agg_dev(NULL);
    return 0;
}

static int Fuzz_uvs_get_device_name_by_eid(void)
{
    (void)uvs_get_device_name_by_eid(NULL, NULL, (size_t)0);
    return 0;
}

static int Fuzz_uvs_set_topo_info(void)
{
    (void)uvs_set_topo_info(NULL, (uint32_t)0, (uint32_t)0);
    return 0;
}

static int Fuzz_uvs_set_share_topo_info(void)
{
    (void)uvs_set_share_topo_info(NULL, (uint32_t)0, (uint32_t)0);
    return 0;
}

static int Fuzz_uvs_insert_main_ue_eid(void)
{
    (void)uvs_insert_main_ue_eid(NULL);
    return 0;
}

static int Fuzz_uvs_insert_main_ue_eid_batch(void)
{
    (void)uvs_insert_main_ue_eid_batch(NULL);
    return 0;
}

static int Fuzz_uvs_delete_main_ue_eid(void)
{
    (void)uvs_delete_main_ue_eid(NULL);
    return 0;
}

static int Fuzz_uvs_lookup_main_ue_eid(void)
{
    (void)uvs_lookup_main_ue_eid(NULL, NULL);
    return 0;
}

static int Fuzz_uvs_flush_main_ue_eid(void)
{
    (void)uvs_flush_main_ue_eid();
    return 0;
}

static int Fuzz_uvs_get_topo_info(void)
{
    (void)uvs_get_topo_info(NULL);
    return 0;
}

static int Fuzz_uvs_get_path_set(void)
{
    (void)uvs_get_path_set(NULL, NULL, (enum uvs_tp_type)0, false, NULL);
    return 0;
}

static const fuzz_case_t g_fuzz_cases[] = {
    {"uvs_create_agg_dev", Fuzz_uvs_create_agg_dev},
    {"uvs_delete_agg_dev", Fuzz_uvs_delete_agg_dev},
    {"uvs_get_device_name_by_eid", Fuzz_uvs_get_device_name_by_eid},
    {"uvs_set_topo_info", Fuzz_uvs_set_topo_info},
    {"uvs_set_share_topo_info", Fuzz_uvs_set_share_topo_info},
    {"uvs_insert_main_ue_eid", Fuzz_uvs_insert_main_ue_eid},
    {"uvs_insert_main_ue_eid_batch", Fuzz_uvs_insert_main_ue_eid_batch},
    {"uvs_delete_main_ue_eid", Fuzz_uvs_delete_main_ue_eid},
    {"uvs_lookup_main_ue_eid", Fuzz_uvs_lookup_main_ue_eid},
    {"uvs_flush_main_ue_eid", Fuzz_uvs_flush_main_ue_eid},
    {"uvs_get_topo_info", Fuzz_uvs_get_topo_info},
    {"uvs_get_path_set", Fuzz_uvs_get_path_set},
};

int main(void)
{
    size_t i;

    for (i = 0; i < sizeof(g_fuzz_cases) / sizeof(g_fuzz_cases[0]); ++i) {
        if (RunFuzzCase(&g_fuzz_cases[i]) != 0) {
            return 1;
        }
    }

    return 0;
}
