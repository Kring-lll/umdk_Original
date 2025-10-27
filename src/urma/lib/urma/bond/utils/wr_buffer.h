/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: Bonding provider WR buffer header
 * Author: Ma Chuan
 * Create: 2025-02-21
 * Note:
 * History: 2025-02-21
 */
#ifndef WR_BUFFER_H
#define WR_BUFFER_H

#include <stdbool.h>
#include "wr_buf_hmap.h"
#include "urma_types.h"

#ifdef __cplusplus
extern "C" {
#endif
/** A simple WR buffer using hash table.
 * Although jfs wr and jfr wr are different types, they have the same caching process,
 * so we use the same cache buffer.
 * A wr_buf will be either a jfs_wr_buf or a jfr_wr_buf
*/
struct wr_buf {
    wr_buf_hmap_t map;
};
typedef struct wr_buf wr_buf_t;
typedef wr_buf_t jfs_wr_buf_t;
typedef wr_buf_t jfr_wr_buf_t;
/* --- wr_buf --- */
bool wr_buf_contain(wr_buf_t *buf, uint32_t id);

int wr_buf_count(wr_buf_t *buf);
/** Traverse the hmap with function func.
 * Skip current node when ret = WR_BUF_HMAP_FUNC_CONTINUE.
 * End traverse when ret != WR_BUF_HMAP_FUNC_SUCCESS && ret != WR_BUF_HMAP_FUNC_CONTINUE.
 * Visit nodes with ret = WR_BUF_HMAP_FUNC_SUCCESS will be removed from the map and released.
 * Other nodes will be retained in the map.
 * This function will free space of node but will not free wr in the node.
 * User needs to free wr in input function.
*/
void wr_buf_traverse_and_remove(wr_buf_t *buf, wr_buf_hmap_visit_node_func_t func, void *args);
/* --- jfs_wr_buf --- */
jfs_wr_buf_t *jfs_wr_buf_new(size_t size);
/** This function will release all unreleased WR and internal SGEs memory */
void jfs_wr_buf_delete(jfs_wr_buf_t *buf);
/** Take the ownership of WR and all pointers in the WR.
 * The memory of WR and SGEs inside will be released by `remove` and `delete` function.
*/
int jfs_wr_buf_add_wr(jfs_wr_buf_t *buf, uint32_t id, urma_jfs_wr_t *wr, wr_buf_extra_value_t *value);
/** Return a mutable reference of WR.
 * The return value can't be released.
*/
urma_jfs_wr_t *jfs_wr_buf_get_wr(jfs_wr_buf_t *buf, uint32_t id);

int jfs_wr_buf_get_user_ctx(jfs_wr_buf_t *buf, uint32_t id, uint64_t *user_ctx);

/** Return NULL when node doesn't exist */
wr_buf_node_t *jfs_wr_buf_get_node(jfs_wr_buf_t *buf, uint32_t id);
/** Remove WR from buffer and release it with sges inside */
int jfs_wr_buf_remove_wr(jfs_wr_buf_t *buf, uint32_t id);
/** Like a combination of `get` and `remove`.
 * It gives out the ownership of WR and release the corresponding position in the hash map.
 * The return value will always come from `add` and `add_copy`.
 * User need to handle the lifetime and memory of the given WR pointer.
*/
urma_jfs_wr_t *jfs_wr_buf_move_out_wr(jfs_wr_buf_t *buf, uint32_t id);
/* --- jfr_wr_buf --- */
jfr_wr_buf_t *jfr_wr_buf_new(size_t size);
/** This function will release all unreleased WR and internal SGEs memory */
void jfr_wr_buf_delete(jfr_wr_buf_t *buf);
/** Take the ownership of WR and all pointers in the WR.
 * The memory of WR and SGEs inside will be released by `remove` and `delete` function.
*/
int jfr_wr_buf_add_wr(jfr_wr_buf_t *buf, uint32_t id, urma_jfr_wr_t *wr, wr_buf_extra_value_t *value);
/** Return a mutable reference of WR.
 * The return value can't be released.
*/
urma_jfr_wr_t *jfr_wr_buf_get_wr(jfr_wr_buf_t *buf, uint32_t id);

int jfr_wr_buf_get_user_ctx(jfr_wr_buf_t *buf, uint32_t id, uint64_t *user_ctx);
/** Remove WR from buffer and release it with sges inside */
int jfr_wr_buf_remove_wr(jfr_wr_buf_t *buf, uint32_t id);
/** Like a combination of `get` and `remove`.
 * It gives out the ownership of WR and release the corresponding position in the hash map.
 * The return value will always come from `add` and `add_copy`.
 * User need to handle the lifetime and memory of the given WR pointer.
*/
urma_jfr_wr_t *jfr_wr_buf_move_out_wr(jfr_wr_buf_t *buf, uint32_t id);
#ifdef __cplusplus
}
#endif
#endif // __WR_BUFFER_H__