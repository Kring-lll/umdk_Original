#ifndef SHMEM_DISPATCH_LAYOUT_TILING_H
#define SHMEM_DISPATCH_LAYOUT_TILING_H

#include "kernel_tiling/kernel_tiling.h"

struct ShmemDispatchLayoutInfo {
    uint32_t numTokens;
    uint32_t numRanks;
    uint32_t numExperts;
    uint32_t numTopk;
    uint32_t localRankSize;
    uint64_t totalUbSize;
};

struct ShmemDispatchLayoutTilingData {
    Mc2InitTiling mc2InitTiling;
    Mc2CcTiling mc2CcTiling1;
    ShmemDispatchLayoutInfo shmemDispatchLayoutInfo;
};

#endif
