/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: URMA bonding segment, provider and health unit tests.
 */

#include "bond_fixture.h"

using namespace urma_test_bond;

TEST(UrmaBondTest, PublicTargetJettyRefHelpersAndAffinityWrapper)
{
    BondPublicApiFixture fixture;
    urma_ops_t ops = {};
    urma_jfs_t jfs = {};
    urma_jfc_t jfc = {};
    urma_context_t ctx = {};
    urma_target_seg_t dst = {};
    urma_target_seg_t src = {};

    bondp_tjetty_get(&fixture.targetJetty.v_tjetty);
    EXPECT_EQ(3UL, fixture.targetJetty.use_cnt.atomic_cnt.load());
    bondp_tjetty_put(&fixture.targetJetty.v_tjetty);
    EXPECT_EQ(2UL, fixture.targetJetty.use_cnt.atomic_cnt.load());

    bondp_tjetty_get(&fixture.targetJfr.v_tjetty);
    EXPECT_EQ(3UL, fixture.targetJfr.use_cnt.atomic_cnt.load());
    bondp_tjetty_put(&fixture.targetJfr.v_tjetty);
    EXPECT_EQ(2UL, fixture.targetJfr.use_cnt.atomic_cnt.load());

    EXPECT_EQ(URMA_EINVAL, urma_write_affinity(nullptr, &fixture.targetJfr.v_tjetty, &dst, &src,
                                               0, 0, 4, {}, 0, 0, 0));
    jfs.urma_ctx = &ctx;
    jfs.jfs_cfg.jfc = &jfc;
    EXPECT_EQ(URMA_EINVAL, urma_write_affinity(&jfs, &fixture.targetJfr.v_tjetty, &dst, &src,
                                               0, 0, 4, {}, 0, 0, 0));
    ops.post_jfs_wr = MockPostJfsWr;
    ctx.ops = &ops;
    EXPECT_EQ(URMA_EINVAL, urma_write_affinity(&jfs, nullptr, &dst, &src, 0, 0, 4, {}, 0, 0, 0));
    EXPECT_EQ(URMA_EINVAL, urma_write_affinity(&jfs, &fixture.targetJfr.v_tjetty, nullptr, &src,
                                               0, 0, 4, {}, 0, 0, 0));
    EXPECT_EQ(URMA_SUCCESS, urma_write_affinity(&jfs, &fixture.targetJfr.v_tjetty, &dst, &src,
                                                0x1000, 0x2000, 4, {}, 0x55, 1, 2));
}

TEST(UrmaBondTest, PublicJettyBindAndUnimportApisCoverSafeStateTransitions)
{
    BondPublicApiFixture fixture;

    EXPECT_EQ(URMA_FAIL, bondp_bind_jetty(&fixture.jetty.v_jetty, &fixture.targetJetty.v_tjetty));
    fixture.jetty.v_jetty.remote_jetty = &fixture.targetJetty.v_tjetty;
    EXPECT_EQ(URMA_EINVAL, bondp_bind_jetty(&fixture.jetty.v_jetty, &fixture.targetJetty.v_tjetty));

    EXPECT_EQ(URMA_SUCCESS, bondp_unbind_jetty(&fixture.jetty.v_jetty));
    EXPECT_EQ(nullptr, fixture.jetty.v_jetty.remote_jetty);
    EXPECT_EQ(1UL, fixture.targetJetty.use_cnt.atomic_cnt.load());

    SetRefCount(&fixture.targetJetty.use_cnt, 2);
    EXPECT_EQ(URMA_SUCCESS, bondp_unimport_jetty(&fixture.targetJetty.v_tjetty));
    EXPECT_EQ(1UL, fixture.targetJetty.use_cnt.atomic_cnt.load());

    SetRefCount(&fixture.targetJfr.use_cnt, 2);
    EXPECT_EQ(URMA_SUCCESS, bondp_unimport_jfr(&fixture.targetJfr.v_tjetty));
    EXPECT_EQ(1UL, fixture.targetJfr.use_cnt.atomic_cnt.load());
}

TEST(UrmaBondTest, PublicImportApisPropagateCommandFailureWithoutDevices)
{
    BondPublicApiFixture fixture;
    urma_rjetty_t rjetty = {};
    urma_rjfr_t rjfr = {};
    urma_token_t token = {};

    /* dev_fd=-1 keeps the ioctl path deterministic and avoids touching /dev or sysfs. */
    fixture.ctx.v_ctx.dev_fd = -1;
    EXPECT_EQ(nullptr, bondp_import_jetty(&fixture.ctx.v_ctx, &rjetty, &token));
    EXPECT_EQ(nullptr, bondp_import_jfr(&fixture.ctx.v_ctx, &rjfr, &token));
}

TEST(UrmaBondTest, PublicSegmentApisCoverTokenAndRefcountPaths)
{
    BondPublicApiFixture fixture;
    urma_token_id_t token = {};
    urma_seg_cfg_t segCfg = {};
    bondp_tseg_t localSeg = {};
    bondp_import_tseg_t remoteSeg = {};

    urma_token_id_t *allocated = bondp_alloc_token_id(&fixture.ctx.v_ctx);
    ASSERT_NE(nullptr, allocated);
    EXPECT_EQ(&fixture.ctx.v_ctx, allocated->urma_ctx);
    EXPECT_EQ(URMA_SUCCESS, bondp_free_token_id(allocated));
    EXPECT_EQ(URMA_SUCCESS, bondp_free_token_id(nullptr));

    segCfg.token_id = nullptr;
    EXPECT_EQ(nullptr, bondp_register_seg(&fixture.ctx.v_ctx, &segCfg));
    segCfg.token_id = &token;
    segCfg.flag.bs.token_id_valid = URMA_TOKEN_ID_INVALID;
    EXPECT_EQ(nullptr, bondp_register_seg(&fixture.ctx.v_ctx, &segCfg));
    segCfg.flag.bs.token_id_valid = 1;
    fixture.ctx.v_ctx.dev_fd = -1;
    EXPECT_EQ(nullptr, bondp_register_seg(&fixture.ctx.v_ctx, &segCfg));

    urma_seg_t remoteSegInfo = {};
    urma_token_t importToken = {};
    EXPECT_EQ(nullptr, bondp_import_seg(&fixture.ctx.v_ctx, &remoteSegInfo, &importToken, 0x1000, {}));

    localSeg.v_tseg.token_id = &token;
    SetRefCount(&localSeg.use_cnt, 2);
    bondp_tseg_get(&localSeg.v_tseg);
    EXPECT_EQ(3UL, localSeg.use_cnt.atomic_cnt.load());
    EXPECT_EQ(URMA_SUCCESS, bondp_unregister_seg(&localSeg.v_tseg));
    EXPECT_EQ(2UL, localSeg.use_cnt.atomic_cnt.load());
    bondp_tseg_put(&localSeg.v_tseg);
    EXPECT_EQ(1UL, localSeg.use_cnt.atomic_cnt.load());

    remoteSeg.v_tseg.token_id = nullptr;
    SetRefCount(&remoteSeg.use_cnt, 2);
    bondp_tseg_get(&remoteSeg.v_tseg);
    EXPECT_EQ(3UL, remoteSeg.use_cnt.atomic_cnt.load());
    EXPECT_EQ(URMA_SUCCESS, bondp_unimport_seg(&remoteSeg.v_tseg));
    EXPECT_EQ(2UL, remoteSeg.use_cnt.atomic_cnt.load());
    bondp_tseg_put(&remoteSeg.v_tseg);
    EXPECT_EQ(1UL, remoteSeg.use_cnt.atomic_cnt.load());
}

TEST(UrmaBondTest, PublicSegmentRegisterCleansPhysicalSegWhenVirtualRegisterFails)
{
    BondPublicApiFixture fixture;
    urma_token_id_t token = {};
    urma_seg_cfg_t segCfg = {};

    /*
     * Physical member registration is mocked in memory. The virtual segment still
     * stops at dev_fd=-1, so this covers rollback without opening a real device.
     */
    fixture.InitSinglePhysicalMember();
    fixture.ctx.v_ctx.dev_fd = -1;
    token.urma_ctx = &fixture.ctx.v_ctx;
    segCfg.token_id = &token;
    segCfg.flag.bs.token_id_valid = URMA_TOKEN_ID_VALID;
    segCfg.va = 0x100000;
    segCfg.len = 4096;

    EXPECT_EQ(nullptr, bondp_register_seg(&fixture.ctx.v_ctx, &segCfg));
}

TEST(UrmaBondTest, PublicSegmentApisReleaseLastReferencesThroughStableFailures)
{
    BondPublicApiFixture fixture;
    urma_token_id_t token = {};
    auto *localSeg = static_cast<bondp_tseg_t *>(std::calloc(1, sizeof(bondp_tseg_t)));
    auto *remoteSeg = static_cast<bondp_import_tseg_t *>(std::calloc(1, sizeof(bondp_import_tseg_t)));
    auto *fullRemoteSeg = static_cast<bondp_import_tseg_t *>(std::calloc(1, sizeof(bondp_import_tseg_t)));
    urma_target_seg_t phySeg = {};

    ASSERT_NE(nullptr, localSeg);
    ASSERT_NE(nullptr, remoteSeg);
    ASSERT_NE(nullptr, fullRemoteSeg);
    fixture.ctx.v_ctx.dev_fd = -1;
    localSeg->v_tseg.token_id = &token;
    localSeg->v_tseg.urma_ctx = &fixture.ctx.v_ctx;
    localSeg->v_tseg.handle = reinterpret_cast<uint64_t>(&localSeg->v_tseg);
    localSeg->v_orig_handle = 0x1010;
    SetRefCount(&localSeg->use_cnt, 1);
    EXPECT_EQ(URMA_SUCCESS, bondp_unregister_seg(&localSeg->v_tseg));

    remoteSeg->v_tseg.token_id = nullptr;
    remoteSeg->v_tseg.urma_ctx = &fixture.ctx.v_ctx;
    remoteSeg->v_tseg.handle = reinterpret_cast<uint64_t>(&remoteSeg->v_tseg);
    remoteSeg->is_reused = true;
    SetRefCount(&remoteSeg->use_cnt, 1);
    EXPECT_EQ(URMA_SUCCESS, bondp_unimport_seg(&remoteSeg->v_tseg));

    fullRemoteSeg->v_tseg.token_id = nullptr;
    fullRemoteSeg->v_tseg.urma_ctx = &fixture.ctx.v_ctx;
    fullRemoteSeg->v_tseg.handle = reinterpret_cast<uint64_t>(&fullRemoteSeg->v_tseg);
    fullRemoteSeg->v_orig_handle = 0x2020;
    fullRemoteSeg->is_reused = false;
    phySeg.urma_ctx = &fixture.ctx.v_ctx;
    phySeg.handle = 0x3030;
    fullRemoteSeg->p_tseg[0][0] = &phySeg;
    fullRemoteSeg->p_orig_handle[0][0] = 0x4040;
    SetRefCount(&fullRemoteSeg->use_cnt, 1);
    EXPECT_EQ(URMA_SUCCESS, bondp_unimport_seg(&fullRemoteSeg->v_tseg));
}

TEST(UrmaBondTest, PublicProviderOpsRejectInvalidOrUninitializedState)
{
    BondPublicApiFixture fixture;
    bondp_global_context_t fakeGlobal = {};
    urma_device_t dev = {};

    std::snprintf(dev.name, sizeof(dev.name), "bond_ut");
    fixture.ctx.v_ctx.dev = &dev;
    fixture.ctx.v_ctx.ref.atomic_cnt.store(0);

    EXPECT_EQ(nullptr, bondp_create_context(&dev, 0, -1));
    EXPECT_EQ(URMA_SUCCESS, bondp_uninit());
    EXPECT_EQ(-EINVAL, bondp_set_bonding_mode(nullptr, BONDP_BONDING_MODE_BALANCE,
                                              BONDP_BONDING_LEVEL_IODIE));
    EXPECT_EQ(-EINVAL, bondp_set_bonding_mode(&fixture.ctx.v_ctx, BONDP_BONDING_MODE_MAX,
                                              BONDP_BONDING_LEVEL_IODIE));
    EXPECT_EQ(-EINVAL, bondp_set_bonding_mode(&fixture.ctx.v_ctx, BONDP_BONDING_MODE_BALANCE,
                                              BONDP_BONDING_LEVEL_MAX));

    fixture.ctx.v_ctx.ref.atomic_cnt.store(2);
    EXPECT_EQ(URMA_EAGAIN, bondp_set_bonding_mode(&fixture.ctx.v_ctx, BONDP_BONDING_MODE_BALANCE,
                                                  BONDP_BONDING_LEVEL_IODIE));

    fixture.ctx.v_ctx.ref.atomic_cnt.store(1);
    fixture.ctx.bonding_mode = BONDP_BONDING_MODE_BALANCE;
    fixture.ctx.bonding_level = BONDP_BONDING_LEVEL_IODIE;
    EXPECT_EQ(0, bondp_set_bonding_mode(&fixture.ctx.v_ctx, BONDP_BONDING_MODE_BALANCE,
                                        BONDP_BONDING_LEVEL_IODIE));

    fixture.ctx.bonding_mode = BONDP_BONDING_MODE_BALANCE;
    fixture.ctx.bonding_level = BONDP_BONDING_LEVEL_IODIE;
    fixture.ctx.v_ctx.dev_fd = -1;
    fixture.ctx.v_ctx.ref.atomic_cnt.store(1);
    EXPECT_EQ(-1, bondp_set_bonding_mode(&fixture.ctx.v_ctx, BONDP_BONDING_MODE_STANDALONE,
                                         BONDP_BONDING_LEVEL_IODIE));

    g_bondp_global_ctx = &fakeGlobal;
    EXPECT_EQ(URMA_FAIL, bondp_init(nullptr));
    EXPECT_EQ(nullptr, bondp_create_context(&dev, 0, -1));
    g_bondp_global_ctx = nullptr;
}

TEST(UrmaBondTest, PublicProviderDeleteContextPropagatesVirtualDeleteFailure)
{
    auto *ctx = static_cast<bondp_context_t *>(std::calloc(1, sizeof(bondp_context_t)));
    ASSERT_NE(nullptr, ctx);
    bondp_global_context_t fakeGlobal = {};
    urma_device_t dev = {};

    std::snprintf(dev.name, sizeof(dev.name), "bond_delete_ut");
    ctx->v_ctx.dev = &dev;
    ctx->v_ctx.dev_fd = -1;
    ctx->v_ctx.async_fd = -1;
    ctx->real_async_fd = -1;
    ASSERT_EQ(0, bdp_p_vjetty_id_table_create(&ctx->p_vjetty_id_table, 4));
    ASSERT_EQ(0, bdp_r_v2p_token_id_table_create(&ctx->remote_v2p_token_id_table, 4));

    g_bondp_global_ctx = &fakeGlobal;
    fakeGlobal.health_thread_ctx.enable_health_check = false;
    EXPECT_EQ(URMA_FAIL, bondp_delete_context(&ctx->v_ctx));
    g_bondp_global_ctx = nullptr;
}

TEST(UrmaBondTest, PublicProviderUninitReleasesHeapGlobalContext)
{
    auto *global = static_cast<bondp_global_context_t *>(std::calloc(1, sizeof(bondp_global_context_t)));
    ASSERT_NE(nullptr, global);

    /*
     * bondp_uninit owns and frees the global context. Keep this fixture heap-backed
     * and health-check disabled so the test covers cleanup without starting threads.
     */
    bondp_health_check_global_ctx_init(global);
    global->health_thread_ctx.enable_health_check = false;
    g_bondp_global_ctx = global;

    EXPECT_EQ(URMA_SUCCESS, bondp_uninit());
    EXPECT_EQ(nullptr, g_bondp_global_ctx);
}

TEST(UrmaBondTest, HealthCheckPublicApisHonorDisabledContract)
{
    BondPublicApiFixture fixture;
    bondp_global_context_t fakeGlobal = {};
    urma_bond_seg_info_out_t segOut = {};
    urma_bond_id_info_out_t idOut = {};
    urma_rjetty_t rjetty = {};
    urma_cr_t cr = {};
    bool enabled = true;

    /*
     * Health check normal operation owns eventfd, epoll, netlink and provider resources.
     * The public contract is still stable when disabled, so keep this UT at that outer boundary.
     */
    g_bondp_global_ctx = &fakeGlobal;
    bondp_health_check_global_ctx_init(&fakeGlobal);
    EXPECT_FALSE(bondp_health_check_enabled());

    bondp_health_check_ctx_init(&fixture.ctx);
    EXPECT_EQ(4096U, fixture.ctx.bondp_heath_check_ctx.check_buf_len);
    EXPECT_EQ(-1, fixture.ctx.bondp_heath_check_ctx.health_check_fd);

    EXPECT_EQ(0, bondp_create_health_check_ctx(&fixture.ctx));
    EXPECT_EQ(0, bondp_register_health_check_seg_for_jetty(&fixture.ctx, &fixture.jetty));
    EXPECT_EQ(0, bondp_fill_vjetty_health_info(&fixture.ctx, &fixture.jetty, &segOut, &enabled));
    EXPECT_FALSE(enabled);

    idOut.is_health_check_enable = true;
    EXPECT_EQ(0, bondp_import_health_check_tseg(&fixture.ctx, &fixture.targetJetty, &idOut, &rjetty));
    EXPECT_EQ(URMA_SUCCESS, bondp_unimport_health_check_tseg(&fixture.targetJetty));
    EXPECT_EQ(0, bondp_register_health_check_task(&fixture.ctx, &fixture.targetJetty, &fixture.jetty));

    bondp_health_update_active_idx(&fixture.ctx, &fixture.targetJetty, 0);
    bondp_health_kick_fallback_task(&fixture.ctx, &fixture.targetJetty);
    bondp_health_notify_fallback_ctrl_rx(&fixture.ctx, 0, 1, 0, 0);
    bondp_health_notify_datapath_link_fail(&fixture.ctx, &fixture.targetJetty, 0, 0);
    bondp_notify_health_event(&fixture.ctx, BONDP_HEALTH_EVENT_ACTIVE_IDX_UPDATE, nullptr);
    EXPECT_FALSE(bondp_try_handle_health_check_cr(&fixture.ctx, 0, &cr));

    bondp_unregister_health_check_task(&fixture.ctx, &fixture.targetJetty);
    bondp_unregister_health_check_seg_for_jetty(&fixture.jetty);
    bondp_destroy_health_check_ctx(&fixture.ctx);
    bondp_health_check_global_ctx_uninit(&fakeGlobal);
    g_bondp_global_ctx = nullptr;
}

TEST(UrmaBondTest, HealthCheckContextCreateAndDestroyUseLocalEventFds)
{
    BondPublicApiFixture fixture;
    bondp_global_context_t fakeGlobal = {};

    /*
     * This covers enabled context lifecycle without starting the health thread or touching
     * provider resources. The epoll fd is local to the test and closed by global uninit.
     */
    g_bondp_global_ctx = &fakeGlobal;
    bondp_health_check_global_ctx_init(&fakeGlobal);
    fakeGlobal.health_thread_ctx.enable_health_check = true;
    fakeGlobal.health_thread_ctx.health_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    ASSERT_GE(fakeGlobal.health_thread_ctx.health_epoll_fd, 0);

    bondp_health_check_ctx_init(&fixture.ctx);
    ASSERT_EQ(0, bondp_create_health_check_ctx(&fixture.ctx));
    EXPECT_GE(fixture.ctx.bondp_heath_check_ctx.health_check_fd, 0);
    bondp_destroy_health_check_ctx(&fixture.ctx);
    EXPECT_EQ(-1, fixture.ctx.bondp_heath_check_ctx.health_check_fd);

    bondp_health_check_global_ctx_uninit(&fakeGlobal);
    g_bondp_global_ctx = nullptr;
}
