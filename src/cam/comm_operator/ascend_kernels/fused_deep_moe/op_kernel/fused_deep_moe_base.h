/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: Definition of communication group related structures
 * Create: 2025-07-19
 * Note:
 * History: 2025-07-19 Create a definition file for a distribution group related structure
 */
#ifndef FUSED_DEEP_MOE_BASE_H
#define FUSED_DEEP_MOE_BASE_H

#include "moe_distribute_base.h"

#define ENABLE_MOE_PROFILING 1
#define PROF_SIZE_PER_CORE 1024

#define TemplateMC2TypeClass typename ExpandXType, typename ExpandIdxType, bool IsNeedReduceScatter, uint32_t EXEC_FLAG
#define TemplateMC2TypeFunc ExpandXType, ExpandIdxType, IsNeedReduceScatter, EXEC_FLAG

#define TemplateDispatchTypeClass                                                                                  \
            typename XType, typename ExpandXOutType, bool StaticQuant, bool DynamicQuant, bool IsSmoothScaleExist, \
            bool IsNeedAllgater, uint32_t EXEC_FLAG
#define TemplateDispatchTypeFunc                                                  \
            XType, ExpandXOutType, StaticQuant, DynamicQuant, IsSmoothScaleExist, \
            IsNeedAllgater, EXEC_FLAG

#if __CCE_AICORE__ == 220 || defined(__DAV_C310__) || defined(__DAV_310R6__)
#ifdef SPLIT_CORE_CUBE
__BLOCK_LOCAL__ __inline__ int64_t* g_moeProfilePtrCube;
#elif defined(SPLIT_CORE_VEC)
__BLOCK_LOCAL__ __inline__ int64_t* g_moeProfilePtrVec;
#else
__BLOCK_LOCAL__ __inline__ int64_t* g_moeProfilePtr;
#endif
#else
__BLOCK_LOCAL__ __inline__ int64_t* g_moeProfilePtr;
#endif

__aicore__ inline int64_t* GetMoeProfilePtr(uint32_t idx = 0)
{
#if __CCE_AICORE__ == 220 || defined(__DAV_C310__) || defined(__DAV_310R6__)
#ifdef SPLIT_CORE_CUBE
    return &g_moeProfilePtrCube[idx];
#elif defined(SPLIT_CORE_VEC)
    return &g_moeProfilePtrVec[idx];
#else
    return &g_moeProfilePtr[idx];
#endif
#else
    return &g_moeProfilePtr[idx];
#endif
}

template <bool sync = false>
__aicore__ inline void MoeTracing(int64_t data)
{
#if ENABLE_MOE_PROFILING
    if constexpr (sync) {
        AscendC::PipeBarrier<PIPE_ALL>();
    }
    
    int64_t *profileData = GetMoeProfilePtr();
    profileData[profileData[0]++] = data;
    profileData[PROF_SIZE_PER_CORE - profileData[0]] = AscendC::GetSystemCycle();
#endif
}

template <bool sync = false>
__aicore__ inline void MoeTracing(int64_t data, uint32_t offset)
{
    MoeTracing<sync>(data | (int64_t)(((uint64_t)offset) << 32));
}
#endif  // FUSED_DEEP_MOE_BASE_H
