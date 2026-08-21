/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 HiSilicon Technologies Co., Ltd. All rights reserved.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 *
 * Description: user space topology path_set calculation
 * Note: UVS already owns the topology source, so the path set can be derived
 *       locally instead of pushing the map into the kernel and querying it back.
 *       During migration both results are compared before the kernel command
 *       is retired.
 */

#ifndef UVS_TOPO_PATH_H
#define UVS_TOPO_PATH_H

#include <stdbool.h>
#include <stdint.h>

#include "uvs_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Save a private copy of the topology map. Called on the set_topo path, where
 * UVS receives the map it is about to push into the kernel.
 * Returns 0 on success, negative errno otherwise.
 */
int uvs_topo_path_update(const struct urma_topo_node *nodes, uint32_t node_num);

/* Release the private copy. */
void uvs_topo_path_clear(void);

/* True when a topology map has been saved. */
bool uvs_topo_path_ready(void);

/*
 * Derive the path set from the local topology map.
 * Mirrors the kernel selection semantics of ubcore_get_path_set():
 *   iodie_level && RTP -> empty path set
 *   iodie_level && CTP -> primary eid paths, one per connected iodie pair
 *   otherwise          -> port eid paths, one per connected port pair
 */
int uvs_topo_local_get_path_set(const uvs_eid_t *src_bonding_eid,
                                const uvs_eid_t *dst_bonding_eid,
                                enum uvs_tp_type tp_type, bool iodie_level,
                                uvs_path_set_t *path_set);

/* Compare two path sets field by field, including path order. */
bool uvs_path_set_equal(const uvs_path_set_t *a, const uvs_path_set_t *b);

/*
 * Shadow mode: the local result is computed and compared against the kernel
 * result, but the kernel result is still what callers receive.
 * Controlled by UVS_TOPO_PATH_SHADOW (default: enabled).
 */
bool uvs_topo_path_shadow_enabled(void);

/* Record one shadow comparison result. */
void uvs_topo_path_count_cmp(bool matched);

/* Mismatch counter, for deciding whether the local implementation is ready. */
uint64_t uvs_topo_path_diff_count(void);
uint64_t uvs_topo_path_cmp_count(void);

#ifdef __cplusplus
}
#endif

#endif /* UVS_TOPO_PATH_H */
