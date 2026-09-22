/**
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description:
 * Shared layered addressing primitive for cam_async _layered operators.
 *
 * Resolves the current layer's data pointer from an all-layer TensorList
 * descriptor. The descriptor layout matches AscendC ListTensorDesc /
 * GetTensorAddr: the first uint64 holds the byte offset from the descriptor
 * head to the data-pointer array; element i of that array is tensor i's
 * GM data address. Index semantics here is the LAYER number (one list
 * element per layer, each element carrying all experts of that layer).
 *
 * Two forms of GetLayerTensorAddr are provided:
 *   - (layerIndex, allTensorPtr): unbounded; the caller knows the index is
 *     in range (or accepts the descriptor's own layout).
 *   - (layerIndex, allTensorLen, allTensorPtr): the layer count is available
 *     on the device, so an out-of-range index is clamped instead of reading
 *     past the descriptor's pointer array.
 *
 * Note: ASCENDC_ASSERT compiles to a no-op in device translation units
 * (see platform_ascendc.h), so the range check clamps instead of trapping -
 * a wrong layer result is observable in tests, a wild pointer is not.
 *
 * Create: 2026-09-18
 * History: 2026-09-18 create file
 *          2026-09-22 add the bounded overload and the layer-index read
 */
#ifndef CAM_UTILS_OP_KERNEL_LAYERED_TENSOR_ADDR_H
#define CAM_UTILS_OP_KERNEL_LAYERED_TENSOR_ADDR_H

#include "kernel_operator.h"

// Reads the device-side layer index. Shared by the layered kernel entries so
// the read has one definition. No cache maintenance is needed here: the
// ops-transformer original and the CANN built-in GroupedMatmul both read these
// descriptors bare, and an A/B test of a Barrier + DataCacheCleanAndInvalid +
// dsb sequence showed no difference in behaviour.
__aicore__ inline int64_t LayeredReadLayerIndex(GM_ADDR layerIndexPtr)
{
    AscendC::GlobalTensor<int64_t> layerIndexGM;
    layerIndexGM.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t *>(layerIndexPtr));
    return layerIndexGM.GetValue(0);
}

template <typename T>
__aicore__ inline __gm__ T* GetLayerTensorAddr(int64_t layerIndex, GM_ADDR allTensorPtr)
{
    __gm__ uint64_t* dataAddr = reinterpret_cast<__gm__ uint64_t*>(allTensorPtr);
    uint64_t tensorPtrOffset = *dataAddr;

    __gm__ uint64_t* retPtr = dataAddr + (tensorPtrOffset >> 3);
    return reinterpret_cast<__gm__ T*>(*(retPtr + layerIndex));
}

template <typename T>
__aicore__ inline __gm__ T* GetLayerTensorAddr(int64_t layerIndex, uint32_t allTensorLen, GM_ADDR allTensorPtr)
{
    __gm__ uint64_t* dataAddr = reinterpret_cast<__gm__ uint64_t*>(allTensorPtr);
    // device-side guard: clamp an out-of-range index into the valid layer range
    // rather than dereferencing the descriptor's pointer array out of bounds
    if (unlikely(layerIndex < 0 || layerIndex >= static_cast<int64_t>(allTensorLen))) {
        layerIndex = 0;
    }
    uint64_t tensorPtrOffset = *dataAddr;
    __gm__ uint64_t* retPtr = dataAddr + (tensorPtrOffset >> 3);
    return reinterpret_cast<__gm__ T*>(*(retPtr + layerIndex));
}

#endif // CAM_UTILS_OP_KERNEL_LAYERED_TENSOR_ADDR_H
