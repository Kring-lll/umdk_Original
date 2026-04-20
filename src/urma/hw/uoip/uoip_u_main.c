/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2025. All rights reserved.
 * Description: uoip — shared library constructor / destructor
 *
 * When liburma loads liburma-uoip.so (via dlopen), the constructor
 * runs automatically and registers this provider with liburma's
 * internal driver list.  The destructor unregisters it on dlclose.
 */

#include "uoip_u_ops.h"
#include "uoip_u_log.h"
#include "urma_provider.h"

static __attribute__((constructor)) void uoip_provider_init(void)
{
    int ret = urma_register_provider_ops(&g_uoip_provider_ops);
    if (ret != 0)
        UOIP_LOG_ERR("urma_register_provider_ops failed (%d)\n", ret);
    else
        UOIP_LOG_INFO("uoip provider registered\n");
}

static __attribute__((destructor)) void uoip_provider_uninit(void)
{
    int ret = urma_unregister_provider_ops(&g_uoip_provider_ops);
    if (ret != 0)
        UOIP_LOG_ERR("urma_unregister_provider_ops failed (%d)\n", ret);
    else
        UOIP_LOG_INFO("uoip provider unregistered\n");
}
