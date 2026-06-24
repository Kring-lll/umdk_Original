/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: URMA bonding datapath conversion and schedule unit tests.
 */

#include "bond_fixture.h"

using namespace urma_test_bond;

TEST(UrmaBondTest, DatapathCopyAndFreeWorkRequests)
{
    BondPathFixture fixture;
    urma_jfs_wr_t src = fixture.MakeRwWr(URMA_OPC_WRITE);
    urma_jfs_wr_t dst = {};
    urma_sge_t preallocSrc[1] = {};
    urma_sge_t preallocDst[1] = {};
    urma_jfr_wr_t recvSrc = {};
    urma_jfr_wr_t recvDst = {};
    urma_sge_t atomicSrc = {};
    urma_sge_t atomicDst = {};
    urma_jfs_wr_t atomicWr = {};

    EXPECT_EQ(URMA_SUCCESS, copy_jfs_wr(&src, &dst, preallocSrc, preallocDst));
    EXPECT_EQ(preallocSrc, dst.rw.src.sge);
    EXPECT_EQ(preallocDst, dst.rw.dst.sge);

    src.opcode = URMA_OPC_SEND;
    src.send.src.sge = fixture.srcSge;
    src.send.src.num_sge = 1;
    EXPECT_EQ(URMA_SUCCESS, copy_jfs_wr(&src, &dst, preallocSrc, nullptr));
    EXPECT_EQ(preallocSrc, dst.send.src.sge);
    src.opcode = URMA_OPC_NOP;
    EXPECT_EQ(URMA_EINVAL, copy_jfs_wr(&src, &dst, nullptr, nullptr));

    atomicSrc.tseg = fixture.srcSge[0].tseg;
    atomicDst.tseg = fixture.dstSge[0].tseg;
    atomicWr.opcode = URMA_OPC_CAS;
    atomicWr.cas.src = &atomicSrc;
    atomicWr.cas.dst = &atomicDst;
    EXPECT_EQ(URMA_SUCCESS, copy_jfs_wr(&atomicWr, &dst, preallocSrc, preallocDst));
    EXPECT_EQ(preallocSrc, dst.cas.src);
    EXPECT_EQ(preallocDst, dst.cas.dst);

    recvSrc.src.sge = fixture.srcSge;
    recvSrc.src.num_sge = 1;
    EXPECT_EQ(URMA_SUCCESS, copy_jfr_wr(&recvSrc, &recvDst, preallocSrc));
    EXPECT_EQ(preallocSrc, recvDst.src.sge);
    dst = {};
    dst.opcode = URMA_OPC_SEND;
    dst.send.src.sge = static_cast<urma_sge_t *>(std::calloc(1, sizeof(urma_sge_t)));
    ASSERT_NE(nullptr, dst.send.src.sge);
    free_jfs_wr(&dst);
    EXPECT_EQ(nullptr, dst.send.src.sge);

    recvDst.src.sge = static_cast<urma_sge_t *>(std::calloc(1, sizeof(urma_sge_t)));
    ASSERT_NE(nullptr, recvDst.src.sge);
    free_jfr_wr(&recvDst);
    EXPECT_EQ(nullptr, recvDst.src.sge);
}

TEST(UrmaBondTest, DatapathConvertMapsAndRestoresWorkRequests)
{
    BondPathFixture fixture;
    urma_jfs_wr_t sendWr = fixture.MakeSendWr(URMA_OPC_SEND);
    urma_jfs_wr_t writeWr = fixture.MakeRwWr(URMA_OPC_WRITE_IMM);
    urma_sge_t casSrc = {};
    urma_sge_t casDst = {};
    urma_sge_t faddSrc = {};
    urma_sge_t faddDst = {};
    urma_jfs_wr_t casWr = {};
    urma_jfs_wr_t faddWr = {};
    urma_jfr_wr_t recvWr = {};
    urma_cr_t cr = {};
    uint32_t msn = 0;

    casSrc.tseg = &fixture.localSeg.v_tseg;
    casDst.tseg = &fixture.remoteSeg.v_tseg;
    faddSrc.tseg = &fixture.localSeg.v_tseg;
    faddDst.tseg = &fixture.remoteSeg.v_tseg;

    EXPECT_EQ(URMA_SUCCESS, convert_jfs_vwr_to_pwr(&sendWr, 0, 0, &fixture.comp, true));
    EXPECT_EQ(URMA_OPC_SEND_IMM, sendWr.opcode);
    EXPECT_EQ(&fixture.phyTarget[0][0], sendWr.tjetty);
    convert_jfs_pwr_to_vwr_resend(&sendWr, &fixture.target.v_tjetty);
    EXPECT_EQ(&fixture.target.v_tjetty, sendWr.tjetty);

    EXPECT_EQ(URMA_SUCCESS, convert_jfs_vwr_to_pwr(&writeWr, 1, 1, &fixture.comp, true));
    EXPECT_EQ(&fixture.phyTarget[1][1], writeWr.tjetty);
    convert_jfs_pwr_to_vwr_resend(&writeWr, &fixture.target.v_tjetty);
    convert_jfs_vwr_to_pwr_for_resend(&writeWr, 0, 0);

    casWr.opcode = URMA_OPC_CAS;
    casWr.tjetty = &fixture.target.v_tjetty;
    casWr.cas.src = &casSrc;
    casWr.cas.dst = &casDst;
    EXPECT_EQ(URMA_SUCCESS, convert_jfs_vwr_to_pwr(&casWr, 0, 0, &fixture.comp, true));
    convert_jfs_pwr_to_vwr_resend(&casWr, &fixture.target.v_tjetty);
    EXPECT_EQ(&fixture.localSeg.v_tseg, casWr.cas.src->tseg);
    EXPECT_EQ(&fixture.remoteSeg.v_tseg, casWr.cas.dst->tseg);

    faddWr.opcode = URMA_OPC_FADD;
    faddWr.tjetty = &fixture.target.v_tjetty;
    faddWr.faa.src = &faddSrc;
    faddWr.faa.dst = &faddDst;
    EXPECT_EQ(URMA_SUCCESS, convert_jfs_vwr_to_pwr(&faddWr, 0, 0, &fixture.comp, true));
    convert_jfs_pwr_to_vwr_resend(&faddWr, &fixture.target.v_tjetty);
    EXPECT_EQ(&fixture.localSeg.v_tseg, faddWr.faa.src->tseg);
    EXPECT_EQ(&fixture.remoteSeg.v_tseg, faddWr.faa.dst->tseg);

    sendWr.opcode = URMA_OPC_NOP;
    sendWr.tjetty = nullptr;
    EXPECT_EQ(URMA_EINVAL, convert_jfs_vwr_to_pwr(&sendWr, 0, 0, &fixture.comp, true));
    convert_jfs_pwr_to_vwr_resend(&sendWr, &fixture.target.v_tjetty);
    convert_jfs_vwr_to_pwr_for_resend(&sendWr, 0, 0);
    add_vwr_use_cnt(&sendWr);
    release_vwr_use_cnt(&sendWr);

    recvWr.src.sge = fixture.srcSge;
    recvWr.src.num_sge = 1;
    EXPECT_EQ(URMA_SUCCESS, convert_jfr_vwr_to_pwr(&recvWr, 0));

    cr.flag.bs.s_r = 0;
    cr.imm_data = 0x123456789ULL;
    convert_pcr_to_vcr(&cr, &fixture.ctx, &msn);
}

TEST(UrmaBondTest, DatapathPublicPostSendPropagatesSingleDeviceNoStoreFailure)
{
    BondPathFixture fixture;
    urma_jfs_wr_t wr = fixture.MakeRwWr(URMA_OPC_WRITE);
    urma_jfs_wr_t *badWr = nullptr;

    fixture.ctx.bonding_mode = BONDP_BONDING_MODE_STANDALONE;
    fixture.comp.comp_type = BONDP_COMP_JFS;
    fixture.comp.active_count = 1;
    fixture.target.active_count = 1;
    fixture.phyOps.post_jfs_wr = MockPostAnyJfsWr;

    EXPECT_EQ(URMA_EINVAL, bondp_post_jfs_wr(&fixture.comp.v_jfs, &wr, &badWr));
    EXPECT_EQ(nullptr, badWr);
    EXPECT_EQ(0U, fixture.comp.sqe_cnt[0][0].load());
}

TEST(UrmaBondTest, DatapathPublicPostRecvPropagatesSingleDeviceNoStoreFailure)
{
    BondPathFixture fixture;
    urma_sge_t recvSge[1] = {};
    urma_jfr_wr_t wr = {};
    urma_jfr_wr_t *badWr = nullptr;

    recvSge[0].tseg = &fixture.localSeg.v_tseg;
    wr.src.sge = recvSge;
    wr.src.num_sge = 1;
    fixture.ctx.bonding_mode = BONDP_BONDING_MODE_STANDALONE;
    fixture.comp.comp_type = BONDP_COMP_JFR;
    fixture.comp.active_count = 1;
    fixture.phyOps.post_jfs_wr = MockPostAnyJfsWr;
    fixture.phyOps.post_jfr_wr = MockPostAnyJfrWr;

    EXPECT_EQ(URMA_EINVAL, bondp_post_jfr_wr(&fixture.comp.v_jfr, &wr, &badWr));
    EXPECT_EQ(nullptr, badWr);
    EXPECT_EQ(0U, fixture.comp.rqe_cnt[0]);
}

TEST(UrmaBondTest, DatapathPublicPostRecvWithoutBackupSplitsAcrossActivePaths)
{
    BondPathFixture fixture;
    urma_sge_t firstSge[1] = {};
    urma_sge_t secondSge[1] = {};
    urma_jfr_wr_t firstWr = {};
    urma_jfr_wr_t secondWr = {};
    urma_jfr_wr_t *badWr = nullptr;

    firstSge[0].tseg = &fixture.localSeg.v_tseg;
    secondSge[0].tseg = &fixture.localSeg.v_tseg;
    firstWr.src.sge = firstSge;
    firstWr.src.num_sge = 1;
    firstWr.next = &secondWr;
    secondWr.src.sge = secondSge;
    secondWr.src.num_sge = 1;
    fixture.ctx.bonding_mode = BONDP_BONDING_MODE_BALANCE;
    fixture.ctx.msn_enable = false;
    fixture.comp.comp_type = BONDP_COMP_JFR;
    fixture.phyOps.post_jfs_wr = MockPostAnyJfsWr;
    fixture.phyOps.post_jfr_wr = MockPostAnyJfrWr;
    ASSERT_EQ(0, wr_buf_init(&fixture.comp.recv_wr_buf, 2));

    EXPECT_EQ(URMA_EINVAL, bondp_post_jfr_wr(&fixture.comp.v_jfr, &firstWr, &badWr));
    EXPECT_EQ(nullptr, badWr);
    EXPECT_EQ(1U, fixture.comp.rqe_cnt[0] + fixture.comp.rqe_cnt[1]);
    wr_buf_uninit(&fixture.comp.recv_wr_buf);
}

TEST(UrmaBondTest, DatapathPollJfcConvertsSingleDeviceSendCr)
{
    BondPathFixture fixture;
    bondp_jfc_t vJfc = {};
    urma_cr_t outCr = {};
    urma_jetty_id_t physicalJfsId = MakeJettyId(0x88);

    physicalJfsId.uasid = 0;
    ASSERT_EQ(0, bdp_p_vjetty_id_table_create(&fixture.ctx.p_vjetty_id_table, 8));
    fixture.ctx.bonding_mode = BONDP_BONDING_MODE_STANDALONE;
    fixture.ctx.p_ctxs[0] = &fixture.phyCtx;
    fixture.phyCtx.eid = physicalJfsId.eid;
    fixture.phyOps.poll_jfc = MockPollOneCr;
    fixture.comp.comp_type = BONDP_COMP_JFS;
    fixture.comp.v_jetty.jetty_id.id = 0x220;
    fixture.comp.sqe_cnt[0][0].store(1);
    ASSERT_EQ(0, bdp_p_vjetty_id_table_add_without_lock(
        &fixture.ctx.p_vjetty_id_table, physicalJfsId, JFS, fixture.comp.v_jfs.jfs_id.id, &fixture.comp));

    vJfc.v_jfc.urma_ctx = &fixture.ctx.v_ctx;
    vJfc.dev_num = 1;
    vJfc.lasted_polled_jfc_idx = -1;
    vJfc.p_jfc[0] = &fixture.phyJfc;

    g_mockDatapathCr = {};
    g_mockDatapathCrCount = 1;
    g_mockDatapathCr.status = URMA_CR_SUCCESS;
    g_mockDatapathCr.local_id = physicalJfsId.id;
    g_mockDatapathCr.flag.bs.s_r = 0;
    g_mockDatapathCr.flag.bs.jetty = 0;

    EXPECT_EQ(1, bondp_poll_jfc(&vJfc.v_jfc, 1, &outCr));
    EXPECT_EQ(fixture.comp.v_jetty.jetty_id.id, outCr.local_id);
    EXPECT_EQ(0U, fixture.comp.sqe_cnt[0][0].load());
    EXPECT_EQ(0, vJfc.lasted_polled_jfc_idx);

    EXPECT_EQ(0, bdp_p_vjetty_id_table_destroy(&fixture.ctx.p_vjetty_id_table));
}

TEST(UrmaBondTest, DatapathFlushJettyConvertsFakeCrAndPropagatesProviderError)
{
    BondPathFixture fixture;
    urma_jetty_t physicalJetty = {};
    urma_cr_t outCr = {};
    urma_jetty_id_t physicalJettyId = MakeJettyId(0x99);

    physicalJettyId.uasid = 0;
    ASSERT_EQ(0, bdp_p_vjetty_id_table_create(&fixture.ctx.p_vjetty_id_table, 8));
    fixture.ctx.bonding_mode = BONDP_BONDING_MODE_STANDALONE;
    fixture.ctx.p_ctxs[0] = &fixture.phyCtx;
    fixture.phyCtx.eid = physicalJettyId.eid;
    fixture.phyOps.flush_jetty = MockFlushOneCr;
    fixture.comp.comp_type = BONDP_COMP_JETTY;
    fixture.comp.v_jetty.urma_ctx = &fixture.ctx.v_ctx;
    fixture.comp.v_jetty.jetty_id.id = 0x330;
    physicalJetty.urma_ctx = &fixture.phyCtx;
    physicalJetty.jetty_id = physicalJettyId;
    physicalJetty.jetty_cfg.jfs_cfg.depth = 1;
    fixture.comp.p_jetty[0] = &physicalJetty;
    ASSERT_EQ(0, bdp_p_vjetty_id_table_add_without_lock(
        &fixture.ctx.p_vjetty_id_table, physicalJettyId, JETTY, fixture.comp.v_jetty.jetty_id.id, &fixture.comp));

    g_mockDatapathCr = {};
    g_mockDatapathCrCount = 1;
    g_mockDatapathCr.status = URMA_CR_WR_SUSPEND_DONE;
    g_mockDatapathCr.local_id = physicalJettyId.id;
    g_mockDatapathCr.flag.bs.jetty = 1;

    EXPECT_EQ(1, bondp_flush_jetty(&fixture.comp.v_jetty, 1, &outCr));
    EXPECT_EQ(fixture.comp.v_jetty.jetty_id.id, outCr.local_id);

    fixture.phyOps.flush_jetty = [](urma_jetty_t *, int, urma_cr_t *) -> int { return -EIO; };
    EXPECT_EQ(-EIO, bondp_flush_jetty(&fixture.comp.v_jetty, 1, &outCr));

    EXPECT_EQ(0, bdp_p_vjetty_id_table_destroy(&fixture.ctx.p_vjetty_id_table));
}

TEST(UrmaBondTest, DatapathScheduleCoversModesAndErrors)
{
    BondPathFixture fixture;
    int sendIdx = -1;
    int targetIdx = -1;
    int recvIdx = -1;
    bondp_chip_id_info_t chipInfo = { BONDP_CHIP_ID_MIN, BONDP_CHIP_ID_MIN };
    bondp_global_context_t fakeGlobal = {};

    fixture.ctx.bonding_mode = BONDP_BONDING_MODE_STANDALONE;
    EXPECT_EQ(0, schedule_send(&fixture.target.v_tjetty, &fixture.comp, &sendIdx, &targetIdx, nullptr));
    EXPECT_EQ(0, schedule_recv(&fixture.comp, &recvIdx));

    fixture.ctx.bonding_mode = BONDP_BONDING_MODE_ACTIVE_BACKUP;
    EXPECT_EQ(0, schedule_send(&fixture.target.v_tjetty, &fixture.comp, &sendIdx, &targetIdx, nullptr));
    EXPECT_EQ(0, schedule_recv(&fixture.comp, &recvIdx));

    fixture.ctx.bonding_mode = BONDP_BONDING_MODE_BALANCE;
    /* Affinity scheduling reads the global failover switch before fallback selection. */
    fakeGlobal.enable_failover = true;
    g_bondp_global_ctx = &fakeGlobal;
    fixture.comp.sqe_cnt[0][0].store(2);
    fixture.comp.sqe_cnt[1][1].store(1);
    EXPECT_EQ(0, schedule_send(&fixture.target.v_tjetty, &fixture.comp, &sendIdx, &targetIdx, &chipInfo));
    EXPECT_EQ(0, schedule_send(&fixture.target.v_tjetty, &fixture.comp, &sendIdx, &targetIdx, nullptr));
    g_bondp_global_ctx = nullptr;

    fixture.comp.active_count = 0;
    EXPECT_EQ(-1, schedule_send(&fixture.target.v_tjetty, &fixture.comp, &sendIdx, &targetIdx, nullptr));
    EXPECT_EQ(-1, schedule_recv(&fixture.comp, &recvIdx));

    fixture.comp.active_count = 1;
    fixture.target.active_count = 0;
    EXPECT_EQ(-1, schedule_send(&fixture.target.v_tjetty, &fixture.comp, &sendIdx, &targetIdx, nullptr));

    fixture.target.active_count = 1;
    fixture.ctx.bonding_mode = static_cast<bondp_bonding_mode_t>(0xff);
    EXPECT_EQ(-1, schedule_send(&fixture.target.v_tjetty, &fixture.comp, &sendIdx, &targetIdx, nullptr));
    EXPECT_EQ(-1, schedule_recv(&fixture.comp, &recvIdx));
}
