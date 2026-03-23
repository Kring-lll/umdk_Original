/*
 * uvs_dynsym.c
 * UVS function pointers for dynamic symbol loading
 * Only used by UB transport mode
 */

#include <dlfcn.h>
#include "uvs_dynsym.h"
#include "umq_vlog.h"

// Global UVS dynsym instance
static uvs_dynsym_t g_uvs_dynsym = {0};
static void *g_uvs_dlhandler = NULL;

#define LOAD_SYMBOL(dynsym, handle, type, name) \
    do { \
        dynsym->name = (type)dlsym(handle, #name); \
        if (dynsym->name == NULL) { \
            UMQ_VLOG_WARN(VLOG_UMQ, #name " not found: %s\n", dlerror()); \
            return -1; \
        } \
    } while (0)

uvs_dynsym_t *uvs_dynsym_get(void)
{
    return &g_uvs_dynsym;
}

int uvs_dynsym_load(uvs_dynsym_t *dynsym)
{
    if (dynsym == NULL) {
        return -1;
    }

    if (dynsym->loaded) {
        return 0;  // Already loaded
    }

    // Load libtpsa.so if not already loaded
    if (g_uvs_dlhandler == NULL) {
        g_uvs_dlhandler = dlopen("libtpsa.so", RTLD_LAZY | RTLD_GLOBAL);
        if (g_uvs_dlhandler == NULL) {
            UMQ_VLOG_ERR(VLOG_UMQ, "open libtpsa.so failed, err: %s\n", dlerror());
            return -1;
        }
    }

    LOAD_SYMBOL(dynsym, g_uvs_dlhandler, uvs_get_route_list_t, uvs_get_route_list);

    dynsym->loaded = true;
    UMQ_VLOG_INFO(VLOG_UMQ, "UVS symbols loaded successfully\n");
    return 0;
}