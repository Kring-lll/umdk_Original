/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bounds checks for peer-provided URMA contexts.
 */
#include <sys/mman.h>
#include <unistd.h>
#include <gtest/gtest.h>
#include "urma_types.h"
#include "umq_ub_context.h"

TEST(UmqUbContextTest, RejectsTruncatedExtensionsAtTheBufferBoundary)
{
    const auto pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void *mapping = mmap(nullptr, pageSize * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(MAP_FAILED, mapping);
    ASSERT_EQ(0, mprotect(static_cast<char *>(mapping) + pageSize, pageSize, PROT_NONE));
    for (uint32_t base : {sizeof(urma_seg_t), sizeof(urma_rjetty_t)}) {
        for (uint32_t tail = 0; tail < sizeof(uint32_t); ++tail) {
            char *blob = static_cast<char *>(mapping) + pageSize - base - tail;
            EXPECT_FALSE(umq_ub_user_info_valid(blob, base + tail, base, true));
        }
        char *blob = static_cast<char *>(mapping) + pageSize - base - sizeof(uint32_t) - 8;
        uint32_t declared = 16;
        memcpy(blob + base, &declared, sizeof(declared));
        EXPECT_FALSE(umq_ub_user_info_valid(blob, base + sizeof(uint32_t) + 8, base, true));
        declared = UINT32_MAX;
        memcpy(blob + base, &declared, sizeof(declared));
        EXPECT_FALSE(umq_ub_user_info_valid(blob, base + sizeof(uint32_t) + 8, base, true));
        declared = 8;
        memcpy(blob + base, &declared, sizeof(declared));
        EXPECT_TRUE(umq_ub_user_info_valid(blob, base + sizeof(uint32_t) + 8, base, true));
        EXPECT_TRUE(umq_ub_user_info_valid(blob, base, base, false));
        EXPECT_FALSE(umq_ub_user_info_valid(blob, base - 1, base, false));
    }
    EXPECT_FALSE(umq_ub_user_info_valid(nullptr, 0, 0, false));
    EXPECT_EQ(0, munmap(mapping, pageSize * 2));
}
