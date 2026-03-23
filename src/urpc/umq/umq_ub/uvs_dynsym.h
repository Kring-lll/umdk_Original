/*
 * uvs_dynsym.h
 * UVS function pointers for dynamic symbol loading
 * Only used by UB transport mode
 */

#ifndef UVS_DYNSYM_H
#define UVS_DYNSYM_H

#include <stdbool.h>
#include <stddef.h>

// UVS function types
typedef int (*uvs_get_route_list_t)(const void *route, void *route_list);

typedef struct uvs_dynsym {
    uvs_get_route_list_t uvs_get_route_list;
    bool loaded;  // Whether symbols are successfully loaded
} uvs_dynsym_t;

// Get the global UVS dynsym instance
uvs_dynsym_t *uvs_dynsym_get(void);

// Check if UVS symbols are available
static inline bool uvs_dynsym_available(uvs_dynsym_t *dynsym)
{
    return dynsym != NULL && dynsym->loaded;
}

// Load UVS symbols (called during initialization)
int uvs_dynsym_load(uvs_dynsym_t *dynsym);

#endif  // UVS_DYNSYM_H