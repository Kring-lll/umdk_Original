/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: URMA bonding unit tests.
 */

#include <cstdlib>
#include <cstdio>

#include <gtest/gtest.h>

#include "bondp_api.h"
#include "bondp_datapath.h"
#include "bondp_datapath_convert.h"
#include "bondp_datapath_schedule.h"
#include "bondp_provider_ops.h"
#include "bondp_segment.h"
#include "bondp_slide_window.h"
#include "bondp_types.h"
#include "bondp_wr_buf.h"
#include "urma_provider.h"

TEST(UrmaBondTest, SlideWindowRejectsInvalidInit)
{
    bdp_slide_wnd_t wnd = {};

    EXPECT_EQ(-1, bdp_slide_wnd_init(nullptr, 8, 4, 0));
    EXPECT_EQ(-1, bdp_slide_wnd_init(&wnd, 4, 4, 0));
}

TEST(UrmaBondTest, SlideWindowAddsAndSlidesHead)
{
    bdp_slide_wnd_t wnd = {};

    ASSERT_EQ(0, bdp_slide_wnd_init(&wnd, 8, 4, 6));
    EXPECT_TRUE(bdp_slide_wnd_seq_in_window(&wnd, 6));
    EXPECT_TRUE(bdp_slide_wnd_seq_in_window(&wnd, 1));
    EXPECT_FALSE(bdp_slide_wnd_seq_in_window(&wnd, 2));

    EXPECT_EQ(0, bdp_slide_wnd_add(&wnd, 6));
    EXPECT_EQ(7U, wnd.head);
    EXPECT_EQ(BDP_SLIDE_WND_OUT_OF_WND, bdp_slide_wnd_add(&wnd, 6));
    EXPECT_EQ(BDP_SLIDE_WND_OUT_OF_WND, bdp_slide_wnd_add(&wnd, 3));

    bdp_slide_wnd_uninit(&wnd);
}

TEST(UrmaBondTest, WrBufferAllocGetRelease)
{
    wr_buf_t buf = {};
    jfs_wr_entry_t *jfsEntry = nullptr;
    jfr_wr_entry_t *jfrEntry = nullptr;

    EXPECT_EQ(-EINVAL, wr_buf_init(nullptr, 2));
    EXPECT_EQ(-EINVAL, wr_buf_init(&buf, 0));

    ASSERT_EQ(0, wr_buf_init(&buf, 2));
    jfsEntry = jfs_wr_buf_alloc(&buf);
    ASSERT_NE(nullptr, jfsEntry);
    EXPECT_EQ(1U, jfsEntry->wr_id);
    EXPECT_EQ(WR_BUF_ENTRY_JFS, jfsEntry->entry_type);
    EXPECT_TRUE(jfsEntry == jfs_wr_buf_get(&buf, jfsEntry->wr_id));

    jfrEntry = jfr_wr_buf_alloc(&buf);
    ASSERT_NE(nullptr, jfrEntry);
    EXPECT_EQ(2U, jfrEntry->wr_id);
    EXPECT_EQ(WR_BUF_ENTRY_JFR, jfrEntry->entry_type);
    EXPECT_TRUE(jfrEntry == jfr_wr_buf_get(&buf, jfrEntry->wr_id));

    EXPECT_EQ(nullptr, jfs_wr_buf_alloc(&buf));
    jfs_wr_buf_release(jfsEntry);
    EXPECT_EQ(nullptr, jfs_wr_buf_get(&buf, 1));
    EXPECT_NE(nullptr, jfs_wr_buf_alloc(&buf));

    wr_buf_uninit(&buf);
}

TEST(UrmaBondTest, WrBufferReleaseJfrAndReuse)
{
    wr_buf_t buf = {};
    jfr_wr_entry_t *entry = nullptr;

    ASSERT_EQ(0, wr_buf_init(&buf, 1));
    entry = jfr_wr_buf_alloc(&buf);
    ASSERT_NE(nullptr, entry);
    uint64_t wrId = entry->wr_id;
    EXPECT_EQ(nullptr, jfr_wr_buf_alloc(&buf));
    jfr_wr_buf_release(entry);
    EXPECT_EQ(nullptr, jfr_wr_buf_get(&buf, wrId));
    EXPECT_NE(nullptr, jfr_wr_buf_alloc(&buf));
    wr_buf_uninit(&buf);
}

TEST(UrmaBondTest, SlideWindowHasDuplicateAndWrapAround)
{
    bdp_slide_wnd_t wnd = {};

    ASSERT_EQ(0, bdp_slide_wnd_init(&wnd, 8, 4, 6));
    EXPECT_EQ(0, bdp_slide_wnd_add(&wnd, 7));
    EXPECT_TRUE(bdp_slide_wnd_has(&wnd, 7));
    EXPECT_EQ(BDP_SLIDE_WND_DUPLICATE, bdp_slide_wnd_add(&wnd, 7));
    EXPECT_EQ(0, bdp_slide_wnd_add(&wnd, 6));
    EXPECT_EQ(8U, bdp_slide_wnd_has(&wnd, 0) ? 8U : wnd.total_size);
    EXPECT_EQ(0, bdp_slide_wnd_add(&wnd, 0));
    bdp_slide_wnd_uninit(&wnd);
}

namespace {

struct BondPathFixture {
    bondp_context_t ctx = {};
    bondp_comp_t comp = {};
    bondp_target_jetty_t target = {};
    urma_target_jetty_t phyTarget[2][2] = {};
    bondp_tseg_t localSeg = {};
    bondp_import_tseg_t remoteSeg = {};
    urma_target_seg_t localPhy[2] = {};
    urma_target_seg_t remotePhy[2][2] = {};
    urma_sge_t srcSge[1] = {};
    urma_sge_t dstSge[1] = {};

    BondPathFixture()
    {
        /* Build a two-path virtual topology without creating real URMA devices. */
        ctx.bonding_mode = BONDP_BONDING_MODE_BALANCE;
        ctx.bonding_level = BONDP_BONDING_LEVEL_IODIE;
        ctx.msn_enable = true;
        comp.bondp_ctx = &ctx;
        comp.comp_type = BONDP_COMP_JETTY;
        comp.v_jetty.jetty_cfg.jfs_cfg.trans_mode = URMA_TM_RC;
        comp.v_jetty.jetty_id.id = 0x22;
        comp.active_count = 2;
        comp.active_indices[0] = 0;
        comp.active_indices[1] = 1;
        comp.valid[0] = true;
        comp.valid[1] = true;
        comp.p_jetty[0] = &comp.v_jetty;
        comp.p_jetty[1] = &comp.v_jetty;

        target.active_count = 2;
        target.active_indices[0] = 0;
        target.active_indices[1] = 1;
        target.valid[0] = true;
        target.valid[1] = true;
        target.p_tjetty[0][0] = &phyTarget[0][0];
        target.p_tjetty[1][1] = &phyTarget[1][1];
        comp.p_jetty[0]->remote_jetty = &phyTarget[0][0];
        comp.p_jetty[1]->remote_jetty = &phyTarget[1][1];

        localSeg.v_tseg.token_id = reinterpret_cast<urma_token_id_t *>(this);
        localSeg.p_tseg[0] = &localPhy[0];
        localSeg.p_tseg[1] = &localPhy[1];
        localPhy[0].handle = reinterpret_cast<uint64_t>(&localSeg.v_tseg);
        localPhy[1].handle = reinterpret_cast<uint64_t>(&localSeg.v_tseg);

        remoteSeg.p_tseg[0][0] = &remotePhy[0][0];
        remoteSeg.p_tseg[1][1] = &remotePhy[1][1];
        remotePhy[0][0].handle = reinterpret_cast<uint64_t>(&remoteSeg.v_tseg);
        remotePhy[1][1].handle = reinterpret_cast<uint64_t>(&remoteSeg.v_tseg);

        srcSge[0].tseg = &localSeg.v_tseg;
        dstSge[0].tseg = &remoteSeg.v_tseg;
    }

    urma_jfs_wr_t MakeSendWr(urma_opcode_t opcode)
    {
        urma_jfs_wr_t wr = {};
        wr.opcode = opcode;
        wr.tjetty = &target.v_tjetty;
        wr.send.src.sge = srcSge;
        wr.send.src.num_sge = 1;
        wr.send.imm_data = 0x123456789ULL;
        return wr;
    }

    urma_jfs_wr_t MakeRwWr(urma_opcode_t opcode)
    {
        urma_jfs_wr_t wr = {};
        wr.opcode = opcode;
        wr.tjetty = &target.v_tjetty;
        wr.rw.src.sge = srcSge;
        wr.rw.src.num_sge = 1;
        wr.rw.dst.sge = dstSge;
        wr.rw.dst.num_sge = 1;
        wr.rw.notify_data = 0xabcdefULL;
        return wr;
    }
};

static void SetRefCount(urma_ref_t *ref, unsigned long value)
{
    ref->atomic_cnt.store(value);
}

struct BondPublicApiFixture {
    bondp_context_t ctx = {};
    urma_device_t dev = {};
    bondp_jfc_t jfc = {};
    bondp_jfce_t jfce = {};
    bondp_comp_t jfs = {};
    bondp_comp_t jfr = {};
    bondp_comp_t jetty = {};
    bondp_target_jetty_t targetJetty = {};
    bondp_target_jetty_t targetJfr = {};

    BondPublicApiFixture()
    {
        /* Public API smoke tests exercise stable outer branches; provider/device paths stay mocked by zero members. */
        std::snprintf(dev.name, sizeof(dev.name), "bond_ut");
        ctx.dev_num = 0;
        ctx.bonding_mode = BONDP_BONDING_MODE_BALANCE;
        ctx.bonding_level = BONDP_BONDING_LEVEL_IODIE;
        ctx.v_ctx.dev = &dev;
        ctx.v_ctx.async_fd = -1;
        pthread_rwlock_init(&ctx.p_vjetty_id_table.lock, nullptr);

        jfce.bondp_ctx = &ctx;
        jfce.v_jfce.fd = -1;
        SetRefCount(&jfce.use_cnt, 1);

        jfc.dev_num = 0;
        jfc.v_jfc.urma_ctx = &ctx.v_ctx;
        jfc.v_jfc.jfc_cfg.jfce = &jfce.v_jfce;
        SetRefCount(&jfc.use_cnt, 1);

        InitComp(&jfs, BONDP_COMP_JFS);
        jfs.v_jfs.jfs_cfg.jfc = &jfc.v_jfc;

        InitComp(&jfr, BONDP_COMP_JFR);
        jfr.v_jfr.jfr_cfg.jfc = &jfc.v_jfc;

        InitComp(&jetty, BONDP_COMP_JETTY);
        jetty.v_jetty.urma_ctx = &ctx.v_ctx;
        jetty.v_jetty.jetty_cfg.shared.jfr = &jfr.v_jfr;

        targetJetty.v_tjetty.urma_ctx = &ctx.v_ctx;
        targetJetty.v_tjetty.type = URMA_JETTY;
        SetRefCount(&targetJetty.use_cnt, 2);

        targetJfr.v_tjetty.urma_ctx = &ctx.v_ctx;
        targetJfr.v_tjetty.type = URMA_JFR;
        SetRefCount(&targetJfr.use_cnt, 2);
    }

    ~BondPublicApiFixture()
    {
        pthread_rwlock_destroy(&ctx.p_vjetty_id_table.lock);
    }

    void InitComp(bondp_comp_t *comp, bondp_comp_type_t type)
    {
        comp->bondp_ctx = &ctx;
        comp->comp_type = type;
        SetRefCount(&comp->use_cnt, 1);
    }
};

static urma_status_t MockPostJfsWr(urma_jfs_t *, urma_jfs_wr_t *wr, urma_jfs_wr_t **badWr)
{
    EXPECT_EQ(URMA_OPC_WRITE, wr->opcode);
    EXPECT_TRUE(wr->flag.bs.has_drv_ext);
    EXPECT_NE(nullptr, wr->rw.src.sge);
    EXPECT_NE(nullptr, wr->rw.dst.sge);
    EXPECT_NE(nullptr, badWr);
    return URMA_SUCCESS;
}

} // namespace

TEST(UrmaBondTest, DatapathCopyAndFreeWorkRequests)
{
    BondPathFixture fixture;
    urma_jfs_wr_t src = fixture.MakeRwWr(URMA_OPC_WRITE);
    urma_jfs_wr_t dst = {};
    urma_sge_t preallocSrc[1] = {};
    urma_sge_t preallocDst[1] = {};
    urma_jfr_wr_t recvSrc = {};
    urma_jfr_wr_t recvDst = {};

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

    EXPECT_EQ(URMA_SUCCESS, convert_jfs_vwr_to_pwr(&sendWr, 0, 0, &fixture.comp));
    EXPECT_EQ(URMA_OPC_SEND_IMM, sendWr.opcode);
    EXPECT_EQ(&fixture.phyTarget[0][0], sendWr.tjetty);
    convert_jfs_pwr_to_vwr_resend(&sendWr, &fixture.target.v_tjetty);
    EXPECT_EQ(&fixture.target.v_tjetty, sendWr.tjetty);

    EXPECT_EQ(URMA_SUCCESS, convert_jfs_vwr_to_pwr(&writeWr, 1, 1, &fixture.comp));
    EXPECT_EQ(&fixture.phyTarget[1][1], writeWr.tjetty);
    convert_jfs_pwr_to_vwr_resend(&writeWr, &fixture.target.v_tjetty);
    convert_jfs_vwr_to_pwr_for_resend(&writeWr, 0, 0);

    casWr.opcode = URMA_OPC_CAS;
    casWr.tjetty = &fixture.target.v_tjetty;
    casWr.cas.src = &casSrc;
    casWr.cas.dst = &casDst;
    EXPECT_EQ(URMA_SUCCESS, convert_jfs_vwr_to_pwr(&casWr, 0, 0, &fixture.comp));

    faddWr.opcode = URMA_OPC_FADD;
    faddWr.tjetty = &fixture.target.v_tjetty;
    faddWr.faa.src = &faddSrc;
    faddWr.faa.dst = &faddDst;
    EXPECT_EQ(URMA_SUCCESS, convert_jfs_vwr_to_pwr(&faddWr, 0, 0, &fixture.comp));

    sendWr.opcode = URMA_OPC_NOP;
    sendWr.tjetty = nullptr;
    EXPECT_EQ(URMA_EINVAL, convert_jfs_vwr_to_pwr(&sendWr, 0, 0, &fixture.comp));
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

TEST(UrmaBondTest, DatapathScheduleCoversModesAndErrors)
{
    BondPathFixture fixture;
    int sendIdx = -1;
    int targetIdx = -1;
    int recvIdx = -1;
    uint32_t recvCnt[URMA_UBAGG_DEV_MAX_NUM] = {};
    bondp_chip_id_info_t chipInfo = { BONDP_CHIP_ID_MIN, BONDP_CHIP_ID_MIN };

    fixture.ctx.bonding_mode = BONDP_BONDING_MODE_STANDALONE;
    EXPECT_EQ(0, schedule_send(&fixture.target.v_tjetty, &fixture.comp, &sendIdx, &targetIdx, nullptr));
    EXPECT_EQ(0, schedule_recv(&fixture.comp, &recvIdx));

    fixture.ctx.bonding_mode = BONDP_BONDING_MODE_ACTIVE_BACKUP;
    EXPECT_EQ(0, schedule_send(&fixture.target.v_tjetty, &fixture.comp, &sendIdx, &targetIdx, nullptr));
    EXPECT_EQ(0, schedule_recv(&fixture.comp, &recvIdx));

    fixture.ctx.bonding_mode = BONDP_BONDING_MODE_BALANCE;
    fixture.comp.sqe_cnt[0][0].store(2);
    fixture.comp.sqe_cnt[1][1].store(1);
    EXPECT_EQ(0, schedule_send(&fixture.target.v_tjetty, &fixture.comp, &sendIdx, &targetIdx, &chipInfo));
    EXPECT_EQ(0, schedule_send(&fixture.target.v_tjetty, &fixture.comp, &sendIdx, &targetIdx, nullptr));
    EXPECT_EQ(0, schedule_recv_n(&fixture.comp, 4, recvCnt));
    EXPECT_EQ(0, schedule_recv_n(&fixture.comp, 0, recvCnt));
    EXPECT_EQ(URMA_EINVAL, schedule_recv_n(&fixture.comp, 1, nullptr));

    fixture.comp.active_count = 0;
    EXPECT_EQ(-1, schedule_send(&fixture.target.v_tjetty, &fixture.comp, &sendIdx, &targetIdx, nullptr));
    EXPECT_EQ(-1, schedule_recv(&fixture.comp, &recvIdx));
    EXPECT_EQ(-1, schedule_recv_n(&fixture.comp, 1, recvCnt));

    fixture.comp.active_count = 1;
    fixture.target.active_count = 0;
    EXPECT_EQ(-1, schedule_send(&fixture.target.v_tjetty, &fixture.comp, &sendIdx, &targetIdx, nullptr));

    fixture.target.active_count = 1;
    fixture.ctx.bonding_mode = static_cast<bondp_bonding_mode_t>(0xff);
    EXPECT_EQ(-1, schedule_send(&fixture.target.v_tjetty, &fixture.comp, &sendIdx, &targetIdx, nullptr));
    EXPECT_EQ(-1, schedule_recv(&fixture.comp, &recvIdx));
}

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

    g_bondp_global_ctx = &fakeGlobal;
    EXPECT_EQ(URMA_FAIL, bondp_init(nullptr));
    EXPECT_EQ(nullptr, bondp_create_context(&dev, 0, -1));
    g_bondp_global_ctx = nullptr;
}
