/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * Description: bitmap gtest source file
 * Author: Chen Wen
 * Create: 2025-07-10
 * Note:
 * History:
 */

#include "bondp_gtest_basic.h"

extern "C" {
#include "bondp_bitmap.h"
}

class BondpBitmapGTest : public BondpBasicGTest {
public:
protected:
};

TEST_F(BondpBitmapGTest, Bondp_BitmapAllocTest){
    uint32_t bitmap_size = 32;
    bondp_bitmap_t *bitmap = bondp_bitmap_alloc(bitmap_size);
    EXPECT_NE((void *)NULL, (void *)bitmap);
}

TEST_F(BondpBitmapGTest, Bondp_BitmapUsedIdTest){
    uint32_t bitmap_size = 32;
    bondp_bitmap_t *bitmap = bondp_bitmap_alloc(bitmap_size);
    uint32_t id=16;
    int ret;
    ret = bondp_bitmap_use_id(bitmap,id);
    EXPECT_EQ(0,ret);
    ret = bondp_bitmap_use_id(bitmap,id);
    EXPECT_EQ(-1,ret);
    ret = bondp_bitmap_free_idx(bitmap,id);
    EXPECT_EQ(0,ret);
    ret = bondp_bitmap_free_idx(bitmap,id);
    EXPECT_NE(0,ret);
    bondp_bitmap_free(bitmap);
}



