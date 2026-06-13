/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: URMA core unit tests.
 */

#include <gtest/gtest.h>

#include "urma_provider.h"
#include "urma_private.h"

typedef struct urma_test_cfg {
    uint32_t depth;
    uint64_t userCtx;
} urma_test_cfg_t;

typedef struct urma_test_opt {
    uint32_t id;
    uint64_t addr;
} urma_test_opt_t;

static const uint64_t URMA_TEST_DEPTH_OPT = 1;
static const uint64_t URMA_TEST_ID_OPT = 2;
static const uint64_t URMA_TEST_JFS_OPT = 3;
static const uint64_t URMA_TEST_ID_MASK = 0x10;
static const uint64_t URMA_TEST_JFS_MASK = 0x20;

static const opt_map_t TEST_OPT_TABLE[] = {
    { URMA_TEST_DEPTH_OPT, URMA_CFG_MASK, TARGET_CFG, offsetof(urma_test_cfg_t, depth), sizeof(uint32_t) },
    { URMA_TEST_ID_OPT, URMA_TEST_ID_MASK, TARGET_OPT, offsetof(urma_test_opt_t, id), sizeof(uint32_t) },
    { URMA_TEST_JFS_OPT, URMA_TEST_JFS_MASK, TARGET_JFS_CFG, offsetof(urma_test_cfg_t, userCtx),
      sizeof(uint64_t) },
};

TEST(UrmaCoreTest, CheckOptValidUpdatesOnlyOptionMask)
{
    uint64_t mask = 0;

    EXPECT_EQ(URMA_SUCCESS, urma_check_opt_valid(&mask, TEST_OPT_TABLE,
        ARRAY_SIZE(TEST_OPT_TABLE), URMA_TEST_DEPTH_OPT, sizeof(uint32_t)));
    EXPECT_EQ(0U, mask);

    EXPECT_EQ(URMA_SUCCESS, urma_check_opt_valid(&mask, TEST_OPT_TABLE,
        ARRAY_SIZE(TEST_OPT_TABLE), URMA_TEST_ID_OPT, sizeof(uint32_t)));
    EXPECT_EQ(URMA_TEST_ID_MASK, mask);
}

TEST(UrmaCoreTest, CheckOptValidRejectsUnknownOrWrongLength)
{
    uint64_t mask = 0;

    EXPECT_EQ(URMA_EINVAL, urma_check_opt_valid(&mask, TEST_OPT_TABLE,
        ARRAY_SIZE(TEST_OPT_TABLE), URMA_TEST_ID_OPT, sizeof(uint64_t)));
    EXPECT_EQ(0U, mask);

    EXPECT_EQ(URMA_EINVAL, urma_check_opt_valid(&mask, TEST_OPT_TABLE,
        ARRAY_SIZE(TEST_OPT_TABLE), 0xff, sizeof(uint32_t)));
    EXPECT_EQ(0U, mask);
}

TEST(UrmaCoreTest, SetOptionsCommonWritesTargetStruct)
{
    urma_test_cfg_t cfg = {};
    urma_test_opt_t opt = {};
    urma_test_cfg_t jfsCfg = {};
    uint32_t depth = 128;
    uint32_t id = 7;
    uint64_t userCtx = 0x12345678;

    EXPECT_EQ(URMA_SUCCESS, urma_set_options_common(nullptr, TEST_OPT_TABLE,
        ARRAY_SIZE(TEST_OPT_TABLE), URMA_TEST_DEPTH_OPT, &depth, sizeof(depth), &cfg, &opt, &jfsCfg));
    EXPECT_EQ(depth, cfg.depth);

    EXPECT_EQ(URMA_SUCCESS, urma_set_options_common(nullptr, TEST_OPT_TABLE,
        ARRAY_SIZE(TEST_OPT_TABLE), URMA_TEST_ID_OPT, &id, sizeof(id), &cfg, &opt, &jfsCfg));
    EXPECT_EQ(id, opt.id);

    EXPECT_EQ(URMA_SUCCESS, urma_set_options_common(nullptr, TEST_OPT_TABLE,
        ARRAY_SIZE(TEST_OPT_TABLE), URMA_TEST_JFS_OPT, &userCtx, sizeof(userCtx), &cfg, &opt, &jfsCfg));
    EXPECT_EQ(userCtx, jfsCfg.userCtx);

    EXPECT_EQ(URMA_EINVAL, urma_set_options_common(nullptr, TEST_OPT_TABLE,
        ARRAY_SIZE(TEST_OPT_TABLE), 0xff, &id, sizeof(id), &cfg, &opt, &jfsCfg));
}

TEST(UrmaCoreTest, UbaggSwitchCounter)
{
    urma_ubagg_switch_init();
    EXPECT_EQ(0U, urma_ubagg_switch_get());
    urma_ubagg_switch_inc();
    urma_ubagg_switch_inc();
    EXPECT_EQ(2U, urma_ubagg_switch_get());
}
