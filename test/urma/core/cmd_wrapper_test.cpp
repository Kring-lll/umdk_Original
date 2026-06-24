/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: URMA core command wrapper unit tests.
 */

#include "core_fixture.h"

using namespace urma_core_test;

TEST(UrmaCoreTest, CmdTokenAndSegmentWrappersPropagateIoctlFailure)
{
    CmdIoctlFailureFixture fixture;
    urma_token_id_t outToken = {};
    urma_token_id_flag_t tokenFlag = {};
    urma_target_seg_t outSeg = {};
    urma_seg_cfg_t segCfg = {};

    ASSERT_TRUE(fixture.Init());
    SetCoreIoctlResult(-1, ENOTTY);
    tokenFlag.bs.multi_seg = 1;
    segCfg.va = 0x1000;
    segCfg.len = 0x2000;
    segCfg.token_id = &fixture.token;
    segCfg.token_value.token = 0x3333;

    EXPECT_EQ(-1, urma_cmd_alloc_token_id(&fixture.ctx, &outToken, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_alloc_token_id_ex(&fixture.ctx, &outToken, tokenFlag, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_free_token_id(&fixture.token));
    EXPECT_EQ(-1, urma_cmd_register_seg(&fixture.ctx, &outSeg, &segCfg, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_unregister_seg(&fixture.tseg));
    EXPECT_EQ(-1, urma_cmd_import_seg(&fixture.ctx, &outSeg, &fixture.importSegCfg, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_unimport_seg(&fixture.tseg));
}

TEST(UrmaCoreTest, CmdJfcJfsJfrWrappersPropagateIoctlFailure)
{
    CmdIoctlFailureFixture fixture;
    urma_jfc_attr_t jfcAttr = {};
    urma_jfs_attr_t jfsAttr = {};
    urma_jfr_attr_t jfrAttr = {};
    urma_jfs_cfg_t jfsCfgOut = {};
    urma_jfr_cfg_t jfrCfgOut = {};
    uint32_t optValue = 4;
    urma_jfc_t *jfcArr[1] = { nullptr };
    urma_jfs_t *jfsArr[1] = { nullptr };
    urma_jfr_t *jfrArr[1] = { nullptr };
    urma_jfc_t *badJfc = nullptr;
    urma_jfs_t *badJfs = nullptr;
    urma_jfr_t *badJfr = nullptr;

    ASSERT_TRUE(fixture.Init());
    SetCoreIoctlResult(-1, ENOTTY);
    jfcArr[0] = &fixture.jfc;
    jfsArr[0] = &fixture.jfs;
    jfrArr[0] = &fixture.jfr;

    EXPECT_EQ(-1, urma_cmd_create_jfc(&fixture.ctx, &fixture.jfc, &fixture.jfc.jfc_cfg, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_modify_jfc(&fixture.jfc, &jfcAttr, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_delete_jfc(&fixture.jfc));
    EXPECT_EQ(-1, urma_cmd_delete_jfc_batch(jfcArr, 1, &badJfc));
    EXPECT_EQ(&fixture.jfc, badJfc);
    EXPECT_EQ(-1, urma_cmd_alloc_jfc(&fixture.ctx, &fixture.jfc.jfc_cfg, &fixture.jfc, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_set_jfc_opt(&fixture.jfc, URMA_JFC_DEPTH, &optValue, sizeof(optValue), &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_get_jfc_opt(&fixture.jfc, URMA_JFC_DEPTH, &optValue, sizeof(optValue), &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_active_jfc(&fixture.jfc, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_deactive_jfc(&fixture.jfc, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_free_jfc(&fixture.jfc, &fixture.udata));

    EXPECT_EQ(-1, urma_cmd_create_jfs(&fixture.ctx, &fixture.jfs, &fixture.jfs.jfs_cfg, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_modify_jfs(&fixture.jfs, &jfsAttr, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_query_jfs(&fixture.jfs, &jfsCfgOut, &jfsAttr));
    EXPECT_EQ(-1, urma_cmd_delete_jfs(&fixture.jfs));
    EXPECT_EQ(-1, urma_cmd_delete_jfs_batch(jfsArr, 1, &badJfs));
    EXPECT_EQ(&fixture.jfs, badJfs);
    EXPECT_EQ(-1, urma_cmd_alloc_jfs(&fixture.ctx, &fixture.jfs.jfs_cfg, &fixture.jfs, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_set_jfs_opt(&fixture.jfs, URMA_JFS_DEPTH, &optValue, sizeof(optValue), &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_get_jfs_opt(&fixture.jfs, URMA_JFS_DEPTH, &optValue, sizeof(optValue), &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_active_jfs(&fixture.jfs, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_deactive_jfs(&fixture.jfs, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_free_jfs(&fixture.jfs, &fixture.udata));

    EXPECT_EQ(-1, urma_cmd_create_jfr(&fixture.ctx, &fixture.jfr, &fixture.jfr.jfr_cfg, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_modify_jfr(&fixture.jfr, &jfrAttr, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_query_jfr(&fixture.jfr, &jfrCfgOut, &jfrAttr));
    EXPECT_EQ(-1, urma_cmd_delete_jfr(&fixture.jfr));
    EXPECT_EQ(-1, urma_cmd_delete_jfr_batch(jfrArr, 1, &badJfr));
    EXPECT_EQ(&fixture.jfr, badJfr);
    EXPECT_EQ(-1, urma_cmd_alloc_jfr(&fixture.ctx, &fixture.jfr.jfr_cfg, &fixture.jfr, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_set_jfr_opt(&fixture.jfr, URMA_JFR_DEPTH, &optValue, sizeof(optValue), &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_get_jfr_opt(&fixture.jfr, URMA_JFR_DEPTH, &optValue, sizeof(optValue), &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_active_jfr(&fixture.jfr, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_deactive_jfr(&fixture.jfr, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_free_jfr(&fixture.jfr, &fixture.udata));
}

TEST(UrmaCoreTest, CmdJettyNotifierAndControlWrappersPropagateIoctlFailure)
{
    CmdIoctlFailureFixture fixture;
    urma_jetty_cfg_t jettyCfg = {};
    urma_jetty_attr_t jettyAttr = {};
    urma_jetty_grp_t jettyGrp = {};
    urma_jetty_grp_cfg_t jettyGrpCfg = {};
    urma_notify_t notify = {};
    urma_user_ctl_in_t ctlIn = {};
    urma_user_ctl_out_t ctlOut = {};
    urma_udrv_t udrv = {};
    urma_get_tp_cfg_t getTpCfg = {};
    urma_tp_info_t tpInfo = {};
    uint32_t tpCnt = 1;
    uint8_t tpAttrCnt = 1;
    uint32_t tpAttrBitmap = 1;
    urma_tp_attr_value_t tpAttr = {};
    uint64_t peerTpHandle = 0;
    uint32_t rxPsn = 0;
    uint32_t optValue = 4;

    ASSERT_TRUE(fixture.Init());
    SetCoreIoctlResult(-1, ENOTTY);
    jettyCfg = fixture.jetty.jetty_cfg;
    std::snprintf(jettyGrpCfg.name, sizeof(jettyGrpCfg.name), "cmd_grp");

    EXPECT_EQ(-1, urma_cmd_create_jfce(&fixture.ctx));
    EXPECT_EQ(-1, urma_cmd_create_jetty(&fixture.ctx, &fixture.jetty, &jettyCfg, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_modify_jetty(&fixture.jetty, &jettyAttr, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_query_jetty(&fixture.jetty, &jettyCfg, &jettyAttr));
    EXPECT_EQ(-1, urma_cmd_delete_jetty(&fixture.jetty));
    EXPECT_EQ(-1, urma_cmd_alloc_jetty(&fixture.ctx, &jettyCfg, &fixture.jetty, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_set_jetty_opt(&fixture.jetty, URMA_JFS_DEPTH, &optValue, sizeof(optValue),
        &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_get_jetty_opt(&fixture.jetty, URMA_JFS_DEPTH, &optValue, sizeof(optValue),
        &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_active_jetty(&fixture.jetty, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_deactive_jetty(&fixture.jetty, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_free_jetty(&fixture.jetty, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_create_jetty_grp(&fixture.ctx, &jettyGrp, &jettyGrpCfg, &fixture.udata));

    EXPECT_EQ(-1, urma_cmd_import_jfr(&fixture.ctx, &fixture.tjfr, &fixture.tjfrCfg, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_import_jfr_ex(&fixture.ctx, &fixture.tjfr, &fixture.tjfrCfg, &fixture.importJfrExCfg,
        &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_unimport_jfr(&fixture.tjfr));
    EXPECT_EQ(-1, urma_cmd_advise_jfr(&fixture.jfs, &fixture.tjfr, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_unadvise_jfr(&fixture.jfs, &fixture.tjfr));
    EXPECT_EQ(-1, urma_cmd_import_jetty(&fixture.ctx, &fixture.tjfr, &fixture.tjettyCfg, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_import_jetty_ex(&fixture.ctx, &fixture.tjfr, &fixture.tjettyCfg,
        &fixture.importJettyExCfg, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_unimport_jetty(&fixture.tjfr));
    EXPECT_EQ(-1, urma_cmd_advise_jetty(&fixture.jetty, &fixture.tjfr, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_unadvise_jetty(&fixture.jetty, &fixture.tjfr));
    EXPECT_EQ(-1, urma_cmd_bind_jetty(&fixture.jetty, &fixture.tjfr, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_bind_jetty_ex(&fixture.jetty, &fixture.tjfr, &fixture.bindJettyExCfg, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_unbind_jetty(&fixture.jetty));

    EXPECT_EQ(-1, urma_cmd_create_notifier(&fixture.ctx));
    EXPECT_EQ(-1, urma_cmd_wait_notify(&fixture.notifier, 1, &notify, 0));
    EXPECT_EQ(-1, urma_cmd_import_jetty_async(&fixture.notifier, &fixture.tjfr, &fixture.tjettyCfg, 0, 0,
        &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_unimport_jetty_async(&fixture.tjfr));
    EXPECT_EQ(-1, urma_cmd_bind_jetty_async(&fixture.notifier, &fixture.jetty, &fixture.tjfr, 0, 0, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_unbind_jetty_async(&fixture.jetty));

    EXPECT_EQ(-1, urma_cmd_user_ctl(&fixture.ctx, &ctlIn, &ctlOut, &udrv));
    EXPECT_EQ(-1, urma_cmd_get_tp_list(&fixture.ctx, &getTpCfg, &tpCnt, &tpInfo, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_set_tp_attr(&fixture.ctx, 1, tpAttrCnt, tpAttrBitmap, &tpAttr, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_get_tp_attr(&fixture.ctx, 1, &tpAttrCnt, &tpAttrBitmap, &tpAttr, &fixture.udata));
    EXPECT_EQ(-1, urma_cmd_exchange_tp_info(&fixture.ctx, &getTpCfg, 1, 2, &peerTpHandle, &rxPsn));
}

TEST(UrmaCoreTest, CmdWrappersFillObjectsWhenIoctlSucceeds)
{
    CoreApiFixture fixture;
    urma_cmd_udrv_priv_t udata = {};
    urma_token_id_t token = {};
    urma_target_seg_t tseg = {};
    urma_seg_cfg_t segCfg = {};
    urma_import_tseg_cfg_t importSegCfg = {};
    urma_jfc_t jfc = {};
    urma_jfs_t jfs = {};
    urma_jfr_t jfr = {};
    urma_jetty_t jetty = {};
    urma_jetty_cfg_t jettyCfgNoRxJfc = fixture.jetty.jetty_cfg;
    urma_target_jetty_t tjfr = {};
    urma_target_jetty_t tjetty = {};
    urma_notifier_t notifier = {};
    urma_tjfr_cfg_t tjfrCfg = {};
    urma_tjetty_cfg_t tjettyCfg = {};
    urma_import_jfr_ex_cfg_t importJfrExCfg = {};
    urma_import_jetty_ex_cfg_t importJettyExCfg = {};
    urma_bind_jetty_ex_cfg_t bindJettyExCfg = {};
    urma_jetty_grp_t jettyGrp = {};
    urma_jetty_grp_cfg_t jettyGrpCfg = {};
    urma_token_t importToken = {};
    urma_eid_info_t eidList[1] = {};
    uint32_t eidCnt = 1;
    urma_jfc_t *jfcEvents[1] = {};
    uint32_t nevents[1] = { 1 };
    urma_async_event_t asyncEvent = {};

    fixture.ctx.dev_fd = 17;
    fixture.ctx.async_fd = 18;
    importToken.token = 0xaaa;
    segCfg.va = 0x1000;
    segCfg.len = 0x2000;
    segCfg.token_id = &fixture.token;
    segCfg.token_value = importToken;
    importSegCfg.ubva = fixture.tseg.seg.ubva;
    importSegCfg.len = 0x2000;
    importSegCfg.token_id = fixture.token.token_id;
    importSegCfg.token = &importToken;
    importSegCfg.mva = 0x3000;
    tjfrCfg.jfr_id = fixture.tjfr.id;
    tjfrCfg.trans_mode = URMA_TM_RM;
    tjfrCfg.tp_type = URMA_RTP;
    tjfrCfg.token = &importToken;
    tjettyCfg.jetty_id = fixture.tjfr.id;
    tjettyCfg.trans_mode = URMA_TM_RC;
    tjettyCfg.tp_type = URMA_RTP;
    tjettyCfg.type = URMA_JETTY;
    tjettyCfg.token = &importToken;
    notifier.urma_ctx = &fixture.ctx;
    notifier.fd = 19;
    jettyCfgNoRxJfc.shared.jfc = nullptr;
    std::snprintf(jettyGrpCfg.name, sizeof(jettyGrpCfg.name), "cmd_success_grp");

    SetCoreIoctlResult(0, 0);
    EXPECT_EQ(0, urma_cmd_alloc_token_id(&fixture.ctx, &token, &udata));
    EXPECT_EQ(0, urma_cmd_register_seg(&fixture.ctx, &tseg, &segCfg, &udata));
    EXPECT_EQ(0, urma_cmd_import_seg(&fixture.ctx, &tseg, &importSegCfg, &udata));

    EXPECT_EQ(0, urma_cmd_create_jfc(&fixture.ctx, &jfc, &fixture.jfc.jfc_cfg, &udata));
    EXPECT_EQ(0, urma_cmd_create_jfs(&fixture.ctx, &jfs, &fixture.jfs.jfs_cfg, &udata));
    EXPECT_EQ(0, urma_cmd_alloc_jfs(&fixture.ctx, &fixture.jfs.jfs_cfg, &jfs, &udata));
    EXPECT_EQ(0, urma_cmd_active_jfs(&jfs, &udata));
    EXPECT_TRUE(jfs.urma_jfs_opt.is_actived);
    EXPECT_EQ(0, urma_cmd_deactive_jfs(&jfs, &udata));
    EXPECT_FALSE(jfs.urma_jfs_opt.is_actived);
    EXPECT_EQ(0, urma_cmd_create_jfr(&fixture.ctx, &jfr, &fixture.jfr.jfr_cfg, &udata));
    EXPECT_EQ(0, urma_cmd_alloc_jfr(&fixture.ctx, &fixture.jfr.jfr_cfg, &jfr, &udata));
    EXPECT_EQ(0, urma_cmd_active_jfr(&jfr, &udata));
    EXPECT_TRUE(jfr.urma_jfr_opt.is_actived);
    EXPECT_EQ(0, urma_cmd_deactive_jfr(&jfr, &udata));
    EXPECT_FALSE(jfr.urma_jfr_opt.is_actived);

    EXPECT_EQ(0, urma_cmd_create_jetty(&fixture.ctx, &jetty, &jettyCfgNoRxJfc, &udata));
    EXPECT_EQ(&fixture.jfc, jetty.jetty_cfg.shared.jfc);
    jetty.jetty_cfg.shared.jfc = nullptr;
    EXPECT_EQ(0, urma_cmd_alloc_jetty(&fixture.ctx, &jettyCfgNoRxJfc, &jetty, &udata));
    EXPECT_EQ(&fixture.jfc, jetty.jetty_cfg.shared.jfc);
    EXPECT_EQ(0, urma_cmd_active_jetty(&jetty, &udata));
    EXPECT_EQ(0, urma_cmd_deactive_jetty(&jetty, &udata));
    EXPECT_EQ(0, urma_cmd_import_jfr(&fixture.ctx, &tjfr, &tjfrCfg, &udata));
    EXPECT_EQ(0, urma_cmd_import_jfr_ex(&fixture.ctx, &tjfr, &tjfrCfg, &importJfrExCfg, &udata));
    EXPECT_EQ(0, urma_cmd_import_jetty(&fixture.ctx, &tjetty, &tjettyCfg, &udata));
    EXPECT_EQ(0, urma_cmd_import_jetty_ex(&fixture.ctx, &tjetty, &tjettyCfg, &importJettyExCfg, &udata));
    EXPECT_EQ(0, urma_cmd_bind_jetty(&jetty, &tjetty, &udata));
    EXPECT_EQ(&tjetty, jetty.remote_jetty);
    EXPECT_EQ(0, urma_cmd_bind_jetty_ex(&jetty, &tjetty, &bindJettyExCfg, &udata));
    EXPECT_EQ(0, urma_cmd_unbind_jetty(&jetty));
    EXPECT_EQ(0, urma_cmd_create_jetty_grp(&fixture.ctx, &jettyGrp, &jettyGrpCfg, &udata));
    EXPECT_EQ(0, urma_cmd_import_jetty_async(&notifier, &tjetty, &tjettyCfg, 0x123, 0, &udata));
    EXPECT_EQ(0, urma_cmd_bind_jetty_async(&notifier, &jetty, &tjetty, 0x456, 0, &udata));
    EXPECT_EQ(0, urma_cmd_unbind_jetty_async(&jetty));

    EXPECT_EQ(0, urma_cmd_get_eid_list(17, 1, eidList, &eidCnt));
    EXPECT_EQ(0, eidCnt);
    EXPECT_EQ(0, urma_cmd_wait_jfc(17, 1, 0, jfcEvents));
    urma_cmd_ack_jfc(&jfcEvents[0], nevents, 1);
    EXPECT_EQ(URMA_SUCCESS, urma_cmd_get_async_event(&fixture.ctx, &asyncEvent));
    asyncEvent.event_type = URMA_EVENT_JFC_ERR;
    asyncEvent.element.jfc = &jfc;
    urma_cmd_ack_async_event(&asyncEvent);
}
