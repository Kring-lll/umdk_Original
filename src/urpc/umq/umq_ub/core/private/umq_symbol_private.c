/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: UMQ dynamic symbol loading for URMA and UVS
 * Create: 2026-3-23
 * Note: Only used by UB transport mode
 */

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include "umq_vlog.h"
#include "umq_symbol_private.h"

static urma_status_t stub_urma_get_rjetty(urma_jetty_t *jetty, urma_rjetty_t **rjetty, uint32_t *length)
{
    if (jetty == NULL || rjetty == NULL || length == NULL) {
        return URMA_ERRNO_INVAL;
    }
    
    uint32_t extra_data_size = 32;
    uint32_t total_size = sizeof(urma_rjetty_t) + extra_data_size;
    
    urma_rjetty_t *stub_rjetty = (urma_rjetty_t *)malloc(total_size);
    if (stub_rjetty == NULL) {
        return URMA_ERRNO_NOMEM;
    }
    
    memset(stub_rjetty, 0, total_size);
    stub_rjetty->jetty_id = jetty->jetty_id;
    stub_rjetty->trans_mode = jetty->jetty_cfg.trans_mode;
    stub_rjetty->policy = jetty->jetty_cfg.shared.policy;
    stub_rjetty->type = URMA_TARGET_JETTY;
    stub_rjetty->flag.value = 0;
    stub_rjetty->tp_type = jetty->tp_type;
    
    char *extra_data = (char *)stub_rjetty + sizeof(urma_rjetty_t);
    for (uint32_t i = 0; i < extra_data_size; i++) {
        extra_data[i] = (char)(i + jetty->jetty_id.id);
    }
    
    *rjetty = stub_rjetty;
    *length = total_size;
    
    UMQ_VLOG_INFO(VLOG_UMQ, "stub: get rjetty for jetty_id %u, base_size %u, extra_size %u, total_length %u\n",
        jetty->jetty_id.id, (uint32_t)sizeof(urma_rjetty_t), extra_data_size, *length);
    return URMA_SUCCESS;
}

static void stub_urma_put_rjetty(urma_rjetty_t *rjetty)
{
    if (rjetty != NULL) {
        UMQ_VLOG_INFO(VLOG_UMQ, "stub: put rjetty\n");
        free(rjetty);
    }
}

// Global URMA/UVS symbol instance
static umq_symbol_urma_t g_umq_symbol_urma = {0};
static void *g_umq_urma_dlhandler = NULL;
static void *g_umq_tpsa_dlhandler = NULL;

#define LOAD_SYMBOL(sym, handle, type, name) \
    do { \
        sym->name = (type)dlsym(handle, #name); \
        if (sym->name == NULL) { \
            UMQ_VLOG_WARN(VLOG_UMQ, #name " not found: %s\n", dlerror()); \
            return -1; \
        } \
    } while (0)

#define LOAD_OPTIONAL_SYMBOL_WITH_STUB(sym, handle, type, name, stub_func) \
    do { \
        sym->name = (type)dlsym(handle, #name); \
        if (sym->name == NULL) { \
            UMQ_VLOG_INFO(VLOG_UMQ, #name " not found (optional), using stub: %s\n", dlerror()); \
            sym->name = stub_func; \
        } \
    } while (0)

umq_symbol_urma_t *umq_symbol_urma(void)
{
    return &g_umq_symbol_urma;
}

int umq_symbol_urma_load(umq_symbol_urma_t *sym)
{
    if (sym == NULL) {
        return -1;
    }

    // Load URMA symbols if not already loaded
    if (g_umq_urma_dlhandler == NULL) {
        g_umq_urma_dlhandler = dlopen("liburma.so", RTLD_LAZY | RTLD_GLOBAL);
        if (g_umq_urma_dlhandler == NULL) {
            UMQ_VLOG_ERR(VLOG_UMQ, "open liburma.so failed, err: %s\n", dlerror());
            return -1;
        }
    }

    // Load UVS symbols if not already loaded
    if (g_umq_tpsa_dlhandler == NULL) {
        g_umq_tpsa_dlhandler = dlopen("libtpsa.so", RTLD_LAZY | RTLD_GLOBAL);
        if (g_umq_tpsa_dlhandler == NULL) {
            UMQ_VLOG_ERR(VLOG_UMQ, "open libtpsa.so failed, err: %s\n", dlerror());
            return -1;
        }
    }

    // Device/Init functions
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_init_t, urma_init);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_uninit_t, urma_uninit);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_get_device_list_t, urma_get_device_list);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_free_device_list_t, urma_free_device_list);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_get_eid_list_t, urma_get_eid_list);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_free_eid_list_t, urma_free_eid_list);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_query_device_t, urma_query_device);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_create_context_t, urma_create_context);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_delete_context_t, urma_delete_context);

    // JFC functions
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_create_jfc_t, urma_create_jfc);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_delete_jfc_t, urma_delete_jfc);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_rearm_jfc_t, urma_rearm_jfc);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_poll_jfc_t, urma_poll_jfc);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_wait_jfc_t, urma_wait_jfc);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_ack_jfc_t, urma_ack_jfc);

    // JFCE functions
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_create_jfce_t, urma_create_jfce);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_delete_jfce_t, urma_delete_jfce);

    // JFR functions
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_create_jfr_t, urma_create_jfr);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_delete_jfr_t, urma_delete_jfr);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_modify_jfr_t, urma_modify_jfr);

    // Jetty functions
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_create_jetty_t, urma_create_jetty);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_delete_jetty_t, urma_delete_jetty);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_modify_jetty_t, urma_modify_jetty);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_bind_jetty_t, urma_bind_jetty);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_unbind_jetty_t, urma_unbind_jetty);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_import_jetty_t, urma_import_jetty);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_unimport_jetty_t, urma_unimport_jetty);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_flush_jetty_t, urma_flush_jetty);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_post_jetty_send_wr_t, urma_post_jetty_send_wr);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_post_jetty_recv_wr_t, urma_post_jetty_recv_wr);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_post_jfr_wr_t, urma_post_jfr_wr);
    LOAD_OPTIONAL_SYMBOL_WITH_STUB(sym, g_umq_urma_dlhandler, urma_get_rjetty_t, urma_get_rjetty, stub_urma_get_rjetty);
    LOAD_OPTIONAL_SYMBOL_WITH_STUB(sym, g_umq_urma_dlhandler, urma_put_rjetty_t, urma_put_rjetty, stub_urma_put_rjetty);

    // Segment functions
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_register_seg_t, urma_register_seg);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_unregister_seg_t, urma_unregister_seg);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_import_seg_t, urma_import_seg);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_unimport_seg_t, urma_unimport_seg);

    // Async Event functions
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_get_async_event_t, urma_get_async_event);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_ack_async_event_t, urma_ack_async_event);

    // Log functions
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_log_set_level_t, urma_log_set_level);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_register_log_func_t, urma_register_log_func);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_register_loc_log_func_t, urma_register_loc_log_func);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_unregister_log_func_t, urma_unregister_log_func);

    // UserCtl
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_user_ctl_t, urma_user_ctl);

    // Utility functions
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_str_to_eid_t, urma_str_to_eid);

    // UVS functions
    LOAD_SYMBOL(sym, g_umq_tpsa_dlhandler, uvs_get_path_set_t, uvs_get_path_set);

    // DFX
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_start_perf_t, urma_start_perf);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_stop_perf_t, urma_stop_perf);
    LOAD_SYMBOL(sym, g_umq_urma_dlhandler, urma_get_perf_info_t, urma_get_perf_info);

    UMQ_VLOG_INFO(VLOG_UMQ, "URMA and UVS symbols loaded successfully\n");
    return 0;
}
