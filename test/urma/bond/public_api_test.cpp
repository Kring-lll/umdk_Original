/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: URMA bonding public control API unit tests.
 */

#include "bond_fixture.h"

using namespace urma_test_bond;

TEST(UrmaBondTest, PublicApiDeletePathsRejectObjectsStillInUse)
{
    BondPublicApiFixture fixture;

    EXPECT_EQ(URMA_EAGAIN, bondp_delete_jfce(&fixture.jfce.v_jfce));
    EXPECT_EQ(URMA_EAGAIN, bondp_delete_jfc(&fixture.jfc.v_jfc));
    EXPECT_EQ(URMA_EAGAIN, bondp_delete_jfs(&fixture.jfs.v_jfs));
    EXPECT_EQ(URMA_EAGAIN, bondp_delete_jfr(&fixture.jfr.v_jfr));
    EXPECT_EQ(URMA_EAGAIN, bondp_delete_jetty(&fixture.jetty.v_jetty));
}

TEST(UrmaBondTest, PublicApiModifyAndQueryHandleEmptyMemberSets)
{
    BondPublicApiFixture fixture;
    urma_jfc_attr_t jfcAttr = {};
    urma_jfs_attr_t jfsAttr = {};
    urma_jfr_attr_t jfrAttr = {};
    urma_jfr_cfg_t queriedCfg = {};
    urma_jetty_attr_t jettyAttr = {};

    jfsAttr.mask = JFS_STATE;
    jfsAttr.state = URMA_JETTY_STATE_ERROR;
    jettyAttr.mask = JETTY_STATE;
    jettyAttr.state = URMA_JETTY_STATE_ERROR;

    EXPECT_EQ(URMA_SUCCESS, bondp_modify_jfc(&fixture.jfc.v_jfc, &jfcAttr));
    EXPECT_EQ(URMA_SUCCESS, bondp_modify_jfs(&fixture.jfs.v_jfs, &jfsAttr));
    EXPECT_TRUE(fixture.jfs.modify_to_error);
    EXPECT_EQ(URMA_SUCCESS, bondp_modify_jfr(&fixture.jfr.v_jfr, &jfrAttr));
    EXPECT_EQ(URMA_SUCCESS, bondp_query_jfr(&fixture.jfr.v_jfr, &queriedCfg, &jfrAttr));
    EXPECT_EQ(JFR_STATE, jfrAttr.mask);
    EXPECT_EQ(URMA_SUCCESS, bondp_modify_jetty(&fixture.jetty.v_jetty, &jettyAttr));
    EXPECT_TRUE(fixture.jetty.modify_to_error);
}

TEST(UrmaBondTest, PublicApiRejectsInvalidCreateAndControlInputs)
{
    BondPublicApiFixture fixture;
    bondp_jfc_cfg_t jfcCfg = {};
    bondp_jfs_cfg_t jfsCfg = {};
    bondp_jfr_cfg_t jfrCfg = {};
    bondp_port_id_t portId = {};
    urma_jetty_cfg_t jettyCfg = {};
    urma_jfc_t *jfcList[1] = {};
    uint32_t nevents[1] = {1};
    urma_async_event_t asyncEvent = {};
    urma_user_ctl_in_t ctl = {};
    urma_user_ctl_out_t ctlOut = {};

    EXPECT_EQ(nullptr, bondp_create_jetty(&fixture.ctx.v_ctx, &jettyCfg));
    urma_jfce_t *jfce = bondp_create_jfce(&fixture.ctx.v_ctx);
    ASSERT_NE(nullptr, jfce);
    EXPECT_EQ(URMA_SUCCESS, bondp_delete_jfce(jfce));

    jfcCfg.base.flag.bs.has_drv_ext = 1;
    EXPECT_EQ(nullptr, bondp_create_jfc(&fixture.ctx.v_ctx, &jfcCfg.base));
    jfsCfg.base.flag.bs.has_drv_ext = 1;
    EXPECT_EQ(nullptr, bondp_create_jfs(&fixture.ctx.v_ctx, &jfsCfg.base));
    jfrCfg.base.flag.bs.has_drv_ext = 1;
    EXPECT_EQ(nullptr, bondp_create_jfr(&fixture.ctx.v_ctx, &jfrCfg.base));

    jfcCfg.port_ids = &portId;
    jfcCfg.port_count = URMA_UBAGG_DEV_MAX_NUM + 1;
    EXPECT_EQ(nullptr, bondp_create_jfc(&fixture.ctx.v_ctx, &jfcCfg.base));
    jfsCfg.port_ids = &portId;
    jfsCfg.port_count = URMA_UBAGG_DEV_MAX_NUM + 1;
    EXPECT_EQ(nullptr, bondp_create_jfs(&fixture.ctx.v_ctx, &jfsCfg.base));
    jfrCfg.port_ids = &portId;
    jfrCfg.port_count = URMA_UBAGG_DEV_MAX_NUM + 1;
    EXPECT_EQ(nullptr, bondp_create_jfr(&fixture.ctx.v_ctx, &jfrCfg.base));

    EXPECT_EQ(-EINVAL, bondp_user_ctl(&fixture.ctx.v_ctx, nullptr, &ctlOut));
    ctl.opcode = UINT32_MAX;
    EXPECT_EQ(-EINVAL, bondp_user_ctl(&fixture.ctx.v_ctx, &ctl, &ctlOut));
    EXPECT_EQ(URMA_EINVAL, bondp_get_async_event(nullptr, &asyncEvent));
    EXPECT_EQ(URMA_EINVAL, bondp_get_async_event(&fixture.ctx.v_ctx, nullptr));

    bondp_jfc_t noJfceJfc = {};
    EXPECT_EQ(URMA_EINVAL, bondp_rearm_jfc(&noJfceJfc.v_jfc, false));
    EXPECT_EQ(-1, bondp_wait_jfc(&fixture.jfce.v_jfce, 1, 0, jfcList));
    bondp_ack_jfc(jfcList, nevents, 1);
    asyncEvent.priv = nullptr;
    bondp_ack_async_event(&asyncEvent);
}

TEST(UrmaBondTest, PublicCreateApisRejectInvalidPortIdsBeforeProviderAccess)
{
    BondPublicApiFixture fixture;
    bondp_jfc_cfg_t jfcCfg = {};
    bondp_jfs_cfg_t jfsCfg = {};
    bondp_jfr_cfg_t jfrCfg = {};
    bondp_jetty_cfg_t jettyCfg = {};
    bondp_port_id_t portId = {};

    jfcCfg.base.flag.bs.has_drv_ext = 1;
    jfcCfg.base.jfce = &fixture.jfce.v_jfce;
    jfcCfg.port_ids = &portId;
    jfcCfg.port_count = 1;
    portId.chip_id = 0;
    portId.die_id = 1;
    portId.port_idx = UINT8_MAX;
    EXPECT_EQ(nullptr, bondp_create_jfc(&fixture.ctx.v_ctx, &jfcCfg.base));

    jfsCfg.base.flag.bs.has_drv_ext = 1;
    jfsCfg.base.jfc = &fixture.jfc.v_jfc;
    jfsCfg.port_ids = &portId;
    jfsCfg.port_count = 1;
    portId.chip_id = 1;
    portId.die_id = 2;
    portId.port_idx = UINT8_MAX;
    EXPECT_EQ(nullptr, bondp_create_jfs(&fixture.ctx.v_ctx, &jfsCfg.base));

    jfrCfg.base.flag.bs.has_drv_ext = 1;
    jfrCfg.base.jfc = &fixture.jfc.v_jfc;
    jfrCfg.port_ids = &portId;
    jfrCfg.port_count = 1;
    portId.chip_id = 1;
    portId.die_id = 1;
    portId.port_idx = PORT_NUM + 1;
    EXPECT_EQ(nullptr, bondp_create_jfr(&fixture.ctx.v_ctx, &jfrCfg.base));

    jettyCfg.base.flag.bs.has_drv_ext = 1;
    jettyCfg.base.flag.bs.share_jfr = URMA_SHARE_JFR;
    jettyCfg.base.shared.jfr = &fixture.jfr.v_jfr;
    jettyCfg.port_ids = &portId;
    jettyCfg.port_count = 1;
    portId.chip_id = 1;
    portId.die_id = 1;
    portId.port_idx = 0;
    fixture.ctx.dev_num = 1;
    EXPECT_EQ(nullptr, bondp_create_jetty(&fixture.ctx.v_ctx, &jettyCfg.base));

    portId.port_idx = UINT8_MAX;
    fixture.ctx.dev_num = 2;
    EXPECT_EQ(nullptr, bondp_create_jfc(&fixture.ctx.v_ctx, &jfcCfg.base));
}

TEST(UrmaBondTest, PublicJfcEventApisDispatchToPhysicalMembers)
{
    BondPublicApiFixture fixture;
    urma_jfc_t *readyJfc[1] = {};
    uint32_t nevents[1] = {1};
    int epollFd = -1;
    int eventFd = -1;
    epoll_event ev = {};

    fixture.InitSinglePhysicalMember();
    fixture.jfc.dev_num = 1;
    fixture.jfce.dev_num = 1;
    fixture.phyOps.rearm_jfc = MockRearmPhysicalJfc;
    EXPECT_EQ(URMA_SUCCESS, bondp_rearm_jfc(&fixture.jfc.v_jfc, true));

    epollFd = epoll_create1(EPOLL_CLOEXEC);
    ASSERT_GE(epollFd, 0);
    eventFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    ASSERT_GE(eventFd, 0);
    fixture.jfce.v_jfce.fd = epollFd;
    fixture.phyJfce[0].fd = eventFd;
    fixture.phyJfce[0].urma_ctx = &fixture.phyCtx;
    fixture.jfce.p_jfce[0] = &fixture.phyJfce[0];
    fixture.phyJfc.jfc_cfg.user_ctx = reinterpret_cast<uint64_t>(&fixture.jfc.v_jfc);
    fixture.phyOps.wait_jfc = MockWaitOnePhysicalJfc;
    fixture.phyOps.ack_jfc = MockAckPhysicalJfc;
    g_mockWaitJfc = &fixture.phyJfc;
    ev.events = EPOLLIN;
    ev.data.fd = eventFd;
    ASSERT_EQ(0, epoll_ctl(epollFd, EPOLL_CTL_ADD, eventFd, &ev));
    ASSERT_EQ(0, eventfd_write(eventFd, 1));

    EXPECT_EQ(1, bondp_wait_jfc(&fixture.jfce.v_jfce, 1, 0, readyJfc));
    EXPECT_EQ(&fixture.jfc.v_jfc, readyJfc[0]);
    bondp_ack_jfc(readyJfc, nevents, 1);

    EXPECT_EQ(0, close(eventFd));
    EXPECT_EQ(0, close(epollFd));
}

TEST(UrmaBondTest, PublicAsyncEventMapsPhysicalJfsAndAckReleasesPrivateEvent)
{
    BondPublicApiFixture fixture;
    urma_jfc_t physicalJfc = {};
    urma_jfs_t physicalJfs = {};
    urma_jfr_t physicalJfr = {};
    urma_jetty_t physicalJetty = {};
    urma_async_event_t event = {};
    int epollFd = -1;
    int eventFd = -1;
    epoll_event ev = {};

    fixture.InitSinglePhysicalMember();
    epollFd = epoll_create1(EPOLL_CLOEXEC);
    ASSERT_GE(epollFd, 0);
    eventFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    ASSERT_GE(eventFd, 0);
    fixture.ctx.v_ctx.async_fd = epollFd;
    physicalJfc.urma_ctx = &fixture.phyCtx;
    physicalJfc.jfc_cfg.user_ctx = reinterpret_cast<uint64_t>(&fixture.jfc.v_jfc);
    physicalJfs.urma_ctx = &fixture.phyCtx;
    physicalJfs.jfs_cfg.user_ctx = reinterpret_cast<uint64_t>(&fixture.jfs.v_jfs);
    physicalJfr.urma_ctx = &fixture.phyCtx;
    physicalJfr.jfr_cfg.user_ctx = reinterpret_cast<uint64_t>(&fixture.jfr.v_jfr);
    physicalJetty.urma_ctx = &fixture.phyCtx;
    physicalJetty.jetty_cfg.user_ctx = reinterpret_cast<uint64_t>(&fixture.jetty.v_jetty);
    fixture.phyOps.get_async_event = MockGetAsyncEvent;
    fixture.phyOps.ack_async_event = MockAckAsyncEvent;
    g_mockAckAsyncCount = 0;
    ev.events = EPOLLIN;
    ev.data.ptr = &fixture.phyCtx;
    ASSERT_EQ(0, epoll_ctl(epollFd, EPOLL_CTL_ADD, eventFd, &ev));
    ASSERT_EQ(0, eventfd_write(eventFd, 1));

    g_mockAsyncEvent = {};
    g_mockAsyncEvent.event_type = URMA_EVENT_JFC_ERR;
    g_mockAsyncEvent.element.jfc = &physicalJfc;
    EXPECT_EQ(URMA_SUCCESS, bondp_get_async_event(&fixture.ctx.v_ctx, &event));
    EXPECT_EQ(&fixture.jfc.v_jfc, event.element.jfc);
    bondp_ack_async_event(&event);

    g_mockAsyncEvent = {};
    g_mockAsyncEvent.event_type = URMA_EVENT_JFR_LIMIT;
    g_mockAsyncEvent.element.jfr = &physicalJfr;
    EXPECT_EQ(URMA_SUCCESS, bondp_get_async_event(&fixture.ctx.v_ctx, &event));
    EXPECT_EQ(&fixture.jfr.v_jfr, event.element.jfr);
    bondp_ack_async_event(&event);

    g_mockAsyncEvent = {};
    g_mockAsyncEvent.event_type = URMA_EVENT_JETTY_ERR;
    g_mockAsyncEvent.element.jetty = &physicalJetty;
    EXPECT_EQ(URMA_SUCCESS, bondp_get_async_event(&fixture.ctx.v_ctx, &event));
    EXPECT_EQ(&fixture.jetty.v_jetty, event.element.jetty);
    bondp_ack_async_event(&event);

    g_mockAsyncEvent = {};
    g_mockAsyncEvent.event_type = URMA_EVENT_EID_CHANGE;
    EXPECT_EQ(URMA_SUCCESS, bondp_get_async_event(&fixture.ctx.v_ctx, &event));
    EXPECT_EQ(0U, event.element.eid_idx);
    bondp_ack_async_event(&event);

    g_mockAsyncEvent = {};
    g_mockAsyncEvent.event_type = URMA_EVENT_JFR_ERR;
    g_mockAsyncEvent.element.jfr = &physicalJfr;
    physicalJfr.jfr_cfg.user_ctx = 0;
    EXPECT_EQ(URMA_EINVAL, bondp_get_async_event(&fixture.ctx.v_ctx, &event));
    physicalJfr.jfr_cfg.user_ctx = reinterpret_cast<uint64_t>(&fixture.jfr.v_jfr);

    g_mockAsyncEvent = {};
    g_mockAsyncEvent.event_type = URMA_EVENT_JFS_ERR;
    g_mockAsyncEvent.element.jfs = &physicalJfs;
    EXPECT_EQ(URMA_SUCCESS, bondp_get_async_event(&fixture.ctx.v_ctx, &event));
    EXPECT_EQ(&fixture.ctx.v_ctx, event.urma_ctx);
    EXPECT_EQ(URMA_EVENT_JFS_ERR, event.event_type);
    EXPECT_EQ(&fixture.jfs.v_jfs, event.element.jfs);
    ASSERT_NE(nullptr, event.priv);
    bondp_ack_async_event(&event);
    EXPECT_EQ(nullptr, event.priv);
    EXPECT_EQ(5, g_mockAckAsyncCount);

    EXPECT_EQ(0, close(eventFd));
    EXPECT_EQ(0, close(epollFd));
}

TEST(UrmaBondTest, LinkRecoveryRebuildsLocalPjettyWithMockProvider)
{
    BondPathFixture fixture;
    bondp_jfc_t vJfsJfc = {};
    bondp_comp_t vJfr = {};
    bondp_health_task_t task = {};
    urma_jetty_id_t oldId = MakeJettyId(0x610);
    urma_jetty_t *oldJetty = nullptr;

    EXPECT_EQ(-1, bondp_rebuild_local_pjetty(nullptr, 0));
    task.bondp_jetty = nullptr;
    EXPECT_EQ(-1, bondp_rebuild_local_pjetty(&task, 0));

    oldId.uasid = 0;
    ASSERT_EQ(0, bdp_p_vjetty_id_table_create(&fixture.ctx.p_vjetty_id_table, 8));
    oldJetty = static_cast<urma_jetty_t *>(std::calloc(1, sizeof(*oldJetty)));
    ASSERT_NE(nullptr, oldJetty);
    oldJetty->urma_ctx = &fixture.phyCtx;
    oldJetty->jetty_id = oldId;
    oldJetty->jetty_cfg.jfs_cfg.trans_mode = URMA_TM_RC;
    oldJetty->urma_jetty_opt.is_actived = true;

    fixture.ctx.p_ctxs[0] = &fixture.phyCtx;
    fixture.phyOps.create_jetty = MockCreatePhysicalJetty;
    fixture.phyOps.delete_jetty = MockDeletePhysicalJetty;
    fixture.phyCtx.ref.atomic_cnt.store(2);
    vJfsJfc.p_jfc[0] = &fixture.phyJfc;
    vJfr.comp_type = BONDP_COMP_JFR;
    vJfr.p_jfr[0] = &fixture.phyJfr[0];
    fixture.phyJfr[0].jfr_cfg.jfc = &fixture.phyJfc;
    fixture.phyJfr[0].jfr_cfg.depth = 4;
    fixture.phyJfr[0].jfr_cfg.max_sge = 1;
    fixture.phyJfr[0].jfr_cfg.trans_mode = URMA_TM_RC;
    fixture.phyJfr[0].jfr_cfg.flag.bs.order_type = URMA_OL;
    fixture.comp.bondp_ctx = &fixture.ctx;
    fixture.comp.comp_type = BONDP_COMP_JETTY;
    fixture.comp.v_jetty.urma_ctx = &fixture.ctx.v_ctx;
    fixture.comp.v_jetty.jetty_id.id = 0x620;
    fixture.comp.v_jetty.jetty_cfg.flag.bs.share_jfr = URMA_SHARE_JFR;
    fixture.comp.v_jetty.jetty_cfg.jfs_cfg.jfc = &vJfsJfc.v_jfc;
    fixture.comp.v_jetty.jetty_cfg.jfs_cfg.depth = 4;
    fixture.comp.v_jetty.jetty_cfg.jfs_cfg.max_sge = 1;
    fixture.comp.v_jetty.jetty_cfg.jfs_cfg.max_rsge = 1;
    fixture.comp.v_jetty.jetty_cfg.jfs_cfg.trans_mode = URMA_TM_RC;
    fixture.comp.v_jetty.jetty_cfg.shared.jfr = &vJfr.v_jfr;
    fixture.comp.p_jetty[0] = oldJetty;
    fixture.comp.valid[0] = true;
    ASSERT_EQ(0, bdp_p_vjetty_id_table_add_without_lock(
        &fixture.ctx.p_vjetty_id_table, oldId, JETTY, fixture.comp.v_jetty.jetty_id.id, &fixture.comp));
    task.bondp_jetty = &fixture.comp;
    task.sub_tasks[0][1].valid = true;
    task.sub_tasks[0][1].local_idx = 0;
    task.sub_tasks[0][1].target_idx = 1;
    task.sub_tasks[0][1].probe_pending = true;
    atomic_store(&task.sub_tasks[0][1].link_ok, false);

    EXPECT_EQ(-1, bondp_rebuild_local_pjetty(&task, -1));
    EXPECT_EQ(-1, bondp_rebuild_local_pjetty(&task, URMA_UBAGG_DEV_MAX_NUM));
    EXPECT_EQ(0, bondp_rebuild_local_pjetty(&task, 0));
    ASSERT_NE(nullptr, fixture.comp.p_jetty[0]);
    EXPECT_NE(oldJetty, fixture.comp.p_jetty[0]);
    EXPECT_FALSE(fixture.comp.valid[0]);
    EXPECT_TRUE(task.sub_tasks[0][1].valid);
    EXPECT_FALSE(task.sub_tasks[0][1].probe_pending);
    EXPECT_FALSE(atomic_load(&task.sub_tasks[0][1].link_ok));
    EXPECT_EQ(nullptr, bdp_p_vjetty_id_table_lookup_comp_without_lock(&fixture.ctx.p_vjetty_id_table, oldId, JETTY));
    EXPECT_EQ(&fixture.comp, bdp_p_vjetty_id_table_lookup_comp_without_lock(
        &fixture.ctx.p_vjetty_id_table, fixture.comp.p_jetty[0]->jetty_id, JETTY));

    std::free(fixture.comp.p_jetty[0]);
    fixture.comp.p_jetty[0] = nullptr;
    EXPECT_EQ(0, bdp_p_vjetty_id_table_destroy(&fixture.ctx.p_vjetty_id_table));
}

TEST(UrmaBondTest, PublicCreateApisCleanupPhysicalMembersWhenVirtualCreateFails)
{
    BondPublicApiFixture fixture;
    urma_jfc_cfg_t jfcCfg = {};
    urma_jfs_cfg_t jfsCfg = {};
    urma_jfr_cfg_t jfrCfg = {};
    urma_jetty_cfg_t jettyCfg = {};
    bondp_global_context_t fakeGlobal = {};

    fixture.InitSinglePhysicalMember();
    fixture.InitJfceFdList();
    jfcCfg.jfce = &fixture.jfce.v_jfce;
    EXPECT_EQ(nullptr, bondp_create_jfc(&fixture.ctx.v_ctx, &jfcCfg));

    jfsCfg.jfc = &fixture.jfc.v_jfc;
    jfsCfg.depth = 4;
    jfsCfg.max_sge = 1;
    jfsCfg.max_rsge = 1;
    jfsCfg.trans_mode = URMA_TM_RC;
    EXPECT_EQ(nullptr, bondp_create_jfs(&fixture.ctx.v_ctx, &jfsCfg));

    jfrCfg.jfc = &fixture.jfc.v_jfc;
    jfrCfg.depth = 4;
    jfrCfg.max_sge = 1;
    jfrCfg.trans_mode = URMA_TM_RC;
    EXPECT_EQ(nullptr, bondp_create_jfr(&fixture.ctx.v_ctx, &jfrCfg));

    fixture.jfr.v_jfr.jfr_cfg.jfc = &fixture.jfc.v_jfc;
    fixture.jfr.v_jfr.jfr_cfg.depth = 4;
    fixture.jfr.v_jfr.jfr_cfg.max_sge = 1;
    fixture.jfr.v_jfr.jfr_cfg.trans_mode = URMA_TM_RC;
    fixture.phyJfr.jfr_cfg.depth = 4;
    fixture.phyJfr.jfr_cfg.max_sge = 1;
    fixture.phyJfr.jfr_cfg.trans_mode = URMA_TM_RC;
    jettyCfg.flag.bs.share_jfr = URMA_SHARE_JFR;
    jettyCfg.jfs_cfg.jfc = &fixture.jfc.v_jfc;
    jettyCfg.jfs_cfg.depth = 4;
    jettyCfg.jfs_cfg.max_sge = 1;
    jettyCfg.jfs_cfg.max_rsge = 1;
    jettyCfg.jfs_cfg.trans_mode = URMA_TM_RC;
    jettyCfg.shared.jfr = &fixture.jfr.v_jfr;
    jettyCfg.shared.jfc = &fixture.jfc.v_jfc;
    g_bondp_global_ctx = &fakeGlobal;
    EXPECT_EQ(nullptr, bondp_create_jetty(&fixture.ctx.v_ctx, &jettyCfg));
    g_bondp_global_ctx = nullptr;
}

TEST(UrmaBondTest, PublicUserCtlUpdatesStableContextFlags)
{
    BondPublicApiFixture fixture;
    urma_user_ctl_out_t unusedOut = {};
    bondp_set_bonding_mode_in_t modeIn = {};
    urma_context_aggr_mode_t legacyMode = URMA_AGGR_MODE_BALANCE;

    fixture.ctx.v_ctx.ref.atomic_cnt.store(1);
    fixture.ctx.seg_cache_enable = false;
    EXPECT_EQ(0, CallBondUserCtl(&fixture.ctx.v_ctx, BONDP_USER_CTL_ENABLE_SEG_CACHE, nullptr, 0, &unusedOut));
    EXPECT_TRUE(fixture.ctx.seg_cache_enable);

    fixture.ctx.msn_enable = true;
    EXPECT_EQ(0, CallBondUserCtl(&fixture.ctx.v_ctx, BONDP_USER_CTL_DISABLE_MSN, nullptr, 0, &unusedOut));
    EXPECT_FALSE(fixture.ctx.msn_enable);

    fixture.ctx.v_ctx.ref.atomic_cnt.store(2);
    fixture.ctx.seg_cache_enable = false;
    EXPECT_EQ(URMA_EAGAIN,
        CallBondUserCtl(&fixture.ctx.v_ctx, BONDP_USER_CTL_ENABLE_SEG_CACHE, nullptr, 0, &unusedOut));
    EXPECT_FALSE(fixture.ctx.seg_cache_enable);

    fixture.ctx.v_ctx.ref.atomic_cnt.store(1);
    EXPECT_EQ(-EINVAL, CallBondUserCtl(nullptr, BONDP_USER_CTL_ENABLE_SEG_CACHE, nullptr, 0, &unusedOut));
    EXPECT_EQ(-EINVAL, CallBondUserCtl(&fixture.ctx.v_ctx, BONDP_USER_CTL_SET_BONDING_MODE_LEGACY, nullptr,
        sizeof(legacyMode), &unusedOut));
    EXPECT_EQ(-EINVAL, CallBondUserCtl(&fixture.ctx.v_ctx, BONDP_USER_CTL_SET_BONDING_MODE_LEGACY, &legacyMode,
        sizeof(legacyMode) - 1, &unusedOut));
    EXPECT_EQ(0, CallBondUserCtl(&fixture.ctx.v_ctx, BONDP_USER_CTL_SET_BONDING_MODE_LEGACY, &legacyMode,
        sizeof(legacyMode), &unusedOut));

    modeIn.bonding_mode = BONDP_BONDING_MODE_BALANCE;
    modeIn.bonding_level = BONDP_BONDING_LEVEL_IODIE;
    EXPECT_EQ(-EINVAL, CallBondUserCtl(&fixture.ctx.v_ctx, BONDP_USER_CTL_SET_BONDING_MODE, nullptr,
        sizeof(modeIn), &unusedOut));
    EXPECT_EQ(-EINVAL, CallBondUserCtl(&fixture.ctx.v_ctx, BONDP_USER_CTL_SET_BONDING_MODE, &modeIn,
        sizeof(modeIn) - 1, &unusedOut));
    EXPECT_EQ(0, CallBondUserCtl(&fixture.ctx.v_ctx, BONDP_USER_CTL_SET_BONDING_MODE, &modeIn, sizeof(modeIn),
        &unusedOut));
}

TEST(UrmaBondTest, PublicUserCtlQueriesPortsAndJfceFds)
{
    BondPublicApiFixture fixture;
    bondp_query_port_in_t queryIn = {};
    bondp_query_port_out_t queryOut = {};
    bondp_get_jfce_fd_list_in_t fdIn = {};
    bondp_get_jfce_fd_list_out_t fdOut = {};
    urma_user_ctl_in_t in = {};
    urma_user_ctl_out_t out = {};

    fixture.InitActiveComp(&fixture.jfr, 3);
    queryIn.jfr = &fixture.jfr.v_jfr;
    in = MakeUserCtl(BONDP_USER_CTL_QUERY_PORT, &queryIn, sizeof(queryIn));
    out = MakeUserCtlOut(&queryOut, sizeof(queryOut));
    EXPECT_EQ(0, bondp_user_ctl(&fixture.ctx.v_ctx, &in, &out));
    EXPECT_EQ(1U, queryOut.enabled_count);
    EXPECT_EQ(3U, queryOut.enabled_indices[0]);
    EXPECT_EQ(1U, queryOut.active_count);
    EXPECT_EQ(3U, queryOut.active_indices[0]);

    EXPECT_EQ(-EINVAL,
        CallBondUserCtl(&fixture.ctx.v_ctx, BONDP_USER_CTL_QUERY_PORT, nullptr, sizeof(queryIn), &out));
    out = MakeUserCtlOut(nullptr, sizeof(queryOut));
    EXPECT_EQ(-EINVAL, bondp_user_ctl(&fixture.ctx.v_ctx, &in, &out));
    out = MakeUserCtlOut(&queryOut, sizeof(queryOut));
    EXPECT_EQ(-EINVAL,
        CallBondUserCtl(&fixture.ctx.v_ctx, BONDP_USER_CTL_QUERY_PORT, &queryIn, sizeof(queryIn) - 1, &out));
    queryIn.jfr = nullptr;
    EXPECT_EQ(-EINVAL,
        CallBondUserCtl(&fixture.ctx.v_ctx, BONDP_USER_CTL_QUERY_PORT, &queryIn, sizeof(queryIn), &out));
    fixture.jfr.bondp_ctx = &fixture.otherBondCtx;
    queryIn.jfr = &fixture.jfr.v_jfr;
    EXPECT_EQ(-EINVAL,
        CallBondUserCtl(&fixture.ctx.v_ctx, BONDP_USER_CTL_QUERY_PORT, &queryIn, sizeof(queryIn), &out));
    fixture.jfr.bondp_ctx = &fixture.ctx;

    fixture.InitJfceFdList();
    fdIn.jfce = &fixture.jfce.v_jfce;
    in = MakeUserCtl(BONDP_USER_CTL_GET_JFCE_FD_LIST, &fdIn, sizeof(fdIn));
    out = MakeUserCtlOut(&fdOut, sizeof(fdOut));
    EXPECT_EQ(0, bondp_user_ctl(&fixture.ctx.v_ctx, &in, &out));
    EXPECT_EQ(2U, fdOut.count);
    EXPECT_EQ(10, fdOut.fd_list[0]);
    EXPECT_EQ(11, fdOut.fd_list[1]);

    fdIn.jfce = nullptr;
    EXPECT_EQ(-EINVAL,
        CallBondUserCtl(&fixture.ctx.v_ctx, BONDP_USER_CTL_GET_JFCE_FD_LIST, &fdIn, sizeof(fdIn), &out));
    fixture.jfce.bondp_ctx = &fixture.otherBondCtx;
    fdIn.jfce = &fixture.jfce.v_jfce;
    EXPECT_EQ(-EINVAL,
        CallBondUserCtl(&fixture.ctx.v_ctx, BONDP_USER_CTL_GET_JFCE_FD_LIST, &fdIn, sizeof(fdIn), &out));
}

TEST(UrmaBondTest, PublicApiModifyDoesNotMarkErrorForNonErrorState)
{
    BondPublicApiFixture fixture;
    urma_jfs_attr_t jfsAttr = {};
    urma_jetty_attr_t jettyAttr = {};

    jfsAttr.mask = JFS_STATE;
    jfsAttr.state = URMA_JETTY_STATE_READY;
    jettyAttr.mask = JETTY_RX_THRESHOLD;
    jettyAttr.state = URMA_JETTY_STATE_ERROR;

    EXPECT_EQ(URMA_SUCCESS, bondp_modify_jfs(&fixture.jfs.v_jfs, &jfsAttr));
    EXPECT_FALSE(fixture.jfs.modify_to_error);
    EXPECT_EQ(URMA_SUCCESS, bondp_modify_jetty(&fixture.jetty.v_jetty, &jettyAttr));
    EXPECT_FALSE(fixture.jetty.modify_to_error);
}
