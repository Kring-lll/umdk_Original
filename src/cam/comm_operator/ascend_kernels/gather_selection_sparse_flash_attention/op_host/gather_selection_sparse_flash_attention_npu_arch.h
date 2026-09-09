/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * Provide NpuArch for recipes open builds that may lack platform/soc_spec.h
 * on the compiler include path.
 */
#ifndef GATHER_SELECTION_SPARSE_FLASH_ATTENTION_NPU_ARCH_H
#define GATHER_SELECTION_SPARSE_FLASH_ATTENTION_NPU_ARCH_H

#if defined(__has_include)
#  if __has_include("platform/soc_spec.h")
#    include "platform/soc_spec.h"
#  elif __has_include(<platform/soc_spec.h>)
#    include <platform/soc_spec.h>
#  endif
#endif

#ifndef __SOC_SPEC_H__
#define __SOC_SPEC_H__

#include <cstdint>

// Minimal subset used by GatherSelectionSparseFlashAttention tiling.
// Numeric values match CANN platform/soc_spec.h.
enum class NpuArch : uint32_t {
    DAV_2002 = 2002,
    DAV_2201 = 2201,
    DAV_3510 = 3510,
};

#endif // __SOC_SPEC_H__

#endif // GATHER_SELECTION_SPARSE_FLASH_ATTENTION_NPU_ARCH_H
