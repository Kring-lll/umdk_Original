/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: URMA bonding public datapath API unit tests.
 */

#include "bond_fixture.h"

using namespace urma_test_bond;

TEST(UrmaBondTest, PublicDatapathApisRejectInvalidWorkRequests)
{
    BondPublicApiFixture fixture;
    urma_jfs_wr_t sendWr = {};
    urma_jfs_wr_t *badSend = nullptr;
    urma_jfr_wr_t recvWr = {};
    urma_jfr_wr_t *badRecv = nullptr;
    urma_cr_t cr = {};

    fixture.jfs.comp_type = BONDP_COMP_JFR;
    fixture.jetty.comp_type = BONDP_COMP_JFR;
    EXPECT_EQ(URMA_EINVAL, bondp_post_jfs_wr(&fixture.jfs.v_jfs, &sendWr, &badSend));
    EXPECT_EQ(&sendWr, badSend);
    badSend = nullptr;
    EXPECT_EQ(URMA_EINVAL, bondp_post_jetty_send_wr(&fixture.jetty.v_jetty, &sendWr, &badSend));
    EXPECT_EQ(&sendWr, badSend);

    fixture.jfr.comp_type = BONDP_COMP_JFS;
    fixture.jetty.comp_type = BONDP_COMP_JFS;
    EXPECT_EQ(URMA_EINVAL, bondp_post_jfr_wr(&fixture.jfr.v_jfr, &recvWr, &badRecv));
    EXPECT_EQ(&recvWr, badRecv);
    badRecv = nullptr;
    EXPECT_EQ(URMA_EINVAL, bondp_post_jetty_recv_wr(&fixture.jetty.v_jetty, &recvWr, &badRecv));
    EXPECT_EQ(&recvWr, badRecv);

    EXPECT_EQ(0, bondp_poll_jfc(&fixture.jfc.v_jfc, 1, &cr));
    EXPECT_EQ(0, bondp_flush_jetty(&fixture.jetty.v_jetty, 1, &cr));
}

TEST(UrmaBondTest, PublicDatapathApisValidateSendWorkRequestFields)
{
    BondPublicApiFixture fixture;
    urma_jfs_wr_t *badSend = nullptr;
    urma_jfs_wr_t writeWr = {};
    urma_jfs_wr_t casWr = {};
    urma_jfs_wr_t faddWr = {};
    bondp_jfs_wr_t affinityWr = {};
    urma_sge_t srcSge = {};
    urma_sge_t dstSge = {};

    writeWr.opcode = URMA_OPC_WRITE;
    writeWr.tjetty = &fixture.targetJetty.v_tjetty;
    EXPECT_EQ(URMA_EINVAL, bondp_post_jfs_wr(&fixture.jfs.v_jfs, &writeWr, &badSend));
    EXPECT_EQ(&writeWr, badSend);

    badSend = nullptr;
    casWr.opcode = URMA_OPC_CAS;
    casWr.tjetty = &fixture.targetJetty.v_tjetty;
    EXPECT_EQ(URMA_EINVAL, bondp_post_jfs_wr(&fixture.jfs.v_jfs, &casWr, &badSend));
    EXPECT_EQ(&casWr, badSend);

    badSend = nullptr;
    faddWr.opcode = URMA_OPC_FADD;
    faddWr.tjetty = &fixture.targetJetty.v_tjetty;
    EXPECT_EQ(URMA_EINVAL, bondp_post_jfs_wr(&fixture.jfs.v_jfs, &faddWr, &badSend));
    EXPECT_EQ(&faddWr, badSend);

    srcSge.tseg = reinterpret_cast<urma_target_seg_t *>(&fixture);
    dstSge.tseg = reinterpret_cast<urma_target_seg_t *>(&fixture.targetJetty);
    affinityWr.base.opcode = URMA_OPC_WRITE;
    affinityWr.base.flag.bs.has_drv_ext = 1;
    affinityWr.base.tjetty = &fixture.targetJetty.v_tjetty;
    affinityWr.base.rw.src.sge = &srcSge;
    affinityWr.base.rw.src.num_sge = 1;
    affinityWr.base.rw.dst.sge = &dstSge;
    affinityWr.base.rw.dst.num_sge = 1;
    affinityWr.src_chip_id = BONDP_CHIP_ID_MAX + 1;
    affinityWr.dst_chip_id = BONDP_CHIP_ID_MIN;
    badSend = nullptr;
    EXPECT_EQ(URMA_EINVAL, bondp_post_jfs_wr(&fixture.jfs.v_jfs, &affinityWr.base, &badSend));
    EXPECT_EQ(&affinityWr.base, badSend);
}

TEST(UrmaBondTest, PublicDatapathApisRejectRecvStateBeforeProviderAccess)
{
    BondPublicApiFixture fixture;
    urma_jfr_wr_t recvWr = {};
    urma_jfr_wr_t *badRecv = nullptr;
    jfr_wr_entry_t *heldEntry = nullptr;

    fixture.jetty.v_jetty.jetty_cfg.shared.jfr = nullptr;
    fixture.jetty.comp_type = BONDP_COMP_JETTY;
    fixture.ctx.bonding_mode = BONDP_BONDING_MODE_BALANCE;
    fixture.ctx.msn_enable = true;
    EXPECT_EQ(URMA_EINVAL, bondp_post_jetty_recv_wr(&fixture.jetty.v_jetty, &recvWr, &badRecv));

    ASSERT_EQ(0, wr_buf_init(&fixture.jfr.recv_wr_buf, 1));
    heldEntry = jfr_wr_buf_alloc(&fixture.jfr.recv_wr_buf);
    ASSERT_NE(nullptr, heldEntry);
    fixture.jfr.active_count = 1;
    fixture.jfr.active_indices[0] = 0;
    EXPECT_EQ(URMA_ENOMEM, bondp_post_jfr_wr(&fixture.jfr.v_jfr, &recvWr, &badRecv));
    jfr_wr_buf_release(&fixture.jfr.recv_wr_buf, heldEntry);
    wr_buf_uninit(&fixture.jfr.recv_wr_buf);
}

TEST(UrmaBondTest, PublicDatapathRecvWithoutBackupRejectsStableScheduleFailures)
{
    BondPublicApiFixture fixture;
    urma_jfr_wr_t recvWr = {};
    urma_jfr_wr_t *badRecv = nullptr;

    fixture.ctx.msn_enable = false;
    fixture.ctx.bonding_mode = BONDP_BONDING_MODE_BALANCE;
    fixture.jfr.active_count = 0;
    EXPECT_EQ(URMA_FAIL, bondp_post_jfr_wr(&fixture.jfr.v_jfr, &recvWr, &badRecv));

    fixture.jfr.active_count = 1;
    fixture.jfr.active_indices[0] = 0;
    fixture.ctx.bonding_mode = static_cast<bondp_bonding_mode_t>(0xff);
    EXPECT_EQ(URMA_EINVAL, bondp_post_jfr_wr(&fixture.jfr.v_jfr, &recvWr, &badRecv));
}

TEST(UrmaBondTest, PublicDatapathRecvWithoutBackupRejectsOversizedWrList)
{
    BondPublicApiFixture fixture;
    urma_jfr_wr_t recvWr[BOND_TEST_RECV_BATCH_POST_MAX_NUM + 1] = {};
    urma_jfr_wr_t *badRecv = nullptr;

    for (uint32_t i = 0; i < BOND_TEST_RECV_BATCH_POST_MAX_NUM; i++) {
        recvWr[i].next = &recvWr[i + 1];
    }
    fixture.ctx.msn_enable = false;
    fixture.InitActiveComp(&fixture.jfr, 0);
    ASSERT_EQ(0, wr_buf_init(&fixture.jfr.recv_wr_buf, 1));
    EXPECT_EQ(URMA_EINVAL, bondp_post_jfr_wr(&fixture.jfr.v_jfr, &recvWr[0], &badRecv));
    wr_buf_uninit(&fixture.jfr.recv_wr_buf);
}

TEST(UrmaBondTest, PublicDatapathSendWrListReturnsFirstBadWr)
{
    BondPublicApiFixture fixture;
    urma_jfs_wr_t firstWr = {};
    urma_jfs_wr_t secondWr = {};
    urma_jfs_wr_t *badSend = nullptr;

    firstWr.opcode = URMA_OPC_SEND;
    firstWr.tjetty = &fixture.targetJetty.v_tjetty;
    firstWr.next = &secondWr;
    secondWr.opcode = URMA_OPC_WRITE;
    secondWr.tjetty = &fixture.targetJetty.v_tjetty;

    EXPECT_EQ(URMA_EINVAL, bondp_post_jfs_wr(&fixture.jfs.v_jfs, &firstWr, &badSend));
    EXPECT_EQ(&secondWr, badSend);
}
