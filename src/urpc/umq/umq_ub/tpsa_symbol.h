/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: TPSA function pointers for dynamic symbol loading
 * Create: 2025-8-4
 * Note: Only used by UB transport mode
 */

#ifndef TPSA_SYMBOL_H
#define TPSA_SYMBOL_H

#include <stdbool.h>
#include <stddef.h>

// TPSA(UVS) function types
typedef int (*uvs_get_route_list_t)(const void *route, void *route_list);

typedef struct tpsa_symbol {
    uvs_get_route_list_t uvs_get_route_list;
    bool loaded;  // Whether symbols are successfully loaded
} tpsa_symbol_t;

// Get the global TPSA symbol instance
tpsa_symbol_t *tpsa_symbol_get(void);

// Check if TPSA symbols are available
static inline bool tpsa_symbol_available(tpsa_symbol_t *sym)
{
    return sym != NULL && sym->loaded;
}

// Load TPSA symbols (called during initialization)
int tpsa_symbol_load(tpsa_symbol_t *sym);

#endif  // TPSA_SYMBOL_H