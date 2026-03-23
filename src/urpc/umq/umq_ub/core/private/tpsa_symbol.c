/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: TPSA function pointers for dynamic symbol loading
 * Create: 2025-8-4
 * Note: Only used by UB transport mode
 */

#include <dlfcn.h>
#include "tpsa_symbol.h"
#include "umq_vlog.h"

// Global TPSA symbol instance
static tpsa_symbol_t g_tpsa_symbol = {0};
static void *g_tpsa_dlhandler = NULL;

#define LOAD_SYMBOL(sym, handle, type, name) \
    do { \
        sym->name = (type)dlsym(handle, #name); \
        if (sym->name == NULL) { \
            UMQ_VLOG_WARN(VLOG_UMQ, #name " not found: %s\n", dlerror()); \
            return -1; \
        } \
    } while (0)

tpsa_symbol_t *tpsa_symbol_get(void)
{
    return &g_tpsa_symbol;
}

int tpsa_symbol_load(tpsa_symbol_t *sym)
{
    if (sym == NULL) {
        return -1;
    }

    if (sym->loaded) {
        return 0;  // Already loaded
    }

    // Load libtpsa.so if not already loaded
    if (g_tpsa_dlhandler == NULL) {
        g_tpsa_dlhandler = dlopen("libtpsa.so", RTLD_LAZY | RTLD_GLOBAL);
        if (g_tpsa_dlhandler == NULL) {
            UMQ_VLOG_ERR(VLOG_UMQ, "open libtpsa.so failed, err: %s\n", dlerror());
            return -1;
        }
    }

    LOAD_SYMBOL(sym, g_tpsa_dlhandler, uvs_get_route_list_t, uvs_get_route_list);

    sym->loaded = true;
    UMQ_VLOG_INFO(VLOG_UMQ, "UVS symbols loaded successfully\n");
    return 0;
}