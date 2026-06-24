#pragma once

/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: Shared helpers for URMA bonding unit tests.
 */

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "bondp_api.h"
#include "bondp_connection.h"
#include "bondp_context_table.h"
#include "bondp_datapath.h"
#include "bondp_datapath_convert.h"
#include "bondp_datapath_schedule.h"
#include "bondp_hash_table.h"
#include "bondp_health_check.h"
#include "bondp_link_recovery.h"
#include "bondp_netlink.h"
#include "bondp_provider_ops.h"
#include "bondp_segment.h"
#include "bondp_slide_window.h"
#include "bondp_timewheel.h"
#include "bondp_types.h"
#include "bondp_worker.h"
#include "bondp_wr_buf.h"
#include "urma_provider.h"
#include "urma_private.h"

namespace urma_test_bond {

static const uint32_t BOND_TEST_RECV_BATCH_POST_MAX_NUM = 280;

struct BondPathFixture {
    bondp_context_t ctx = {};
    urma_device_t dev = {};
    urma_sysfs_dev_t sysfsDev = {};
    urma_context_t phyCtx = {};
    urma_ops_t phyOps = {};
    urma_jfc_t phyJfc = {};
    bondp_comp_t comp = {};
    bondp_target_jetty_t target = {};
    urma_jetty_t phyJetty[2] = {};
    urma_jfs_t phyJfs[2] = {};
    urma_jfr_t phyJfr[2] = {};
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
        std::snprintf(dev.name, sizeof(dev.name), "bond_path_ut");
        sysfsDev.dev_attr.dev_cap.max_jfc_depth = 8;
        sysfsDev.dev_attr.dev_cap.max_jfs_depth = 8;
        sysfsDev.dev_attr.dev_cap.max_jfr_depth = 8;
        sysfsDev.dev_attr.dev_cap.max_jfs_inline_len = 64;
        sysfsDev.dev_attr.dev_cap.max_jfs_sge = 8;
        sysfsDev.dev_attr.dev_cap.max_jfs_rsge = 8;
        sysfsDev.dev_attr.dev_cap.max_jfr_sge = 8;
        dev.sysfs_dev = &sysfsDev;
        ctx.v_ctx.dev = &dev;
        ctx.bonding_mode = BONDP_BONDING_MODE_BALANCE;
        ctx.bonding_level = BONDP_BONDING_LEVEL_IODIE;
        ctx.msn_enable = true;
        phyCtx.dev = &dev;
        phyCtx.ops = &phyOps;
        phyJfc.urma_ctx = &phyCtx;
        comp.bondp_ctx = &ctx;
        comp.comp_type = BONDP_COMP_JETTY;
        comp.v_jfs.jfs_cfg.trans_mode = URMA_TM_RC;
        comp.v_jfr.jfr_cfg.trans_mode = URMA_TM_RC;
        comp.v_jetty.jetty_cfg.jfs_cfg.trans_mode = URMA_TM_RC;
        comp.v_jetty.jetty_id.id = 0x22;
        comp.v_jetty.remote_jetty = &target.v_tjetty;
        comp.v_jfs.jfs_id.id = 0x23;
        comp.v_jfr.jfr_id.id = 0x24;
        comp.active_count = 2;
        comp.active_indices[0] = 0;
        comp.active_indices[1] = 1;
        comp.valid[0] = true;
        comp.valid[1] = true;
        phyJfs[0].urma_ctx = &phyCtx;
        phyJfs[1].urma_ctx = &phyCtx;
        phyJfs[0].jfs_cfg.jfc = &phyJfc;
        phyJfs[1].jfs_cfg.jfc = &phyJfc;
        phyJfr[0].urma_ctx = &phyCtx;
        phyJfr[1].urma_ctx = &phyCtx;
        phyJfr[0].jfr_cfg.jfc = &phyJfc;
        phyJfr[1].jfr_cfg.jfc = &phyJfc;
        phyJetty[0].urma_ctx = &phyCtx;
        phyJetty[1].urma_ctx = &phyCtx;
        comp.p_jfs[0] = &phyJfs[0];
        comp.p_jfs[1] = &phyJfs[1];
        comp.p_jfr[0] = &phyJfr[0];
        comp.p_jfr[1] = &phyJfr[1];
        /*
         * Keep physical jetty objects separate from the virtual jetty. Balance
         * scheduling dereferences physical remote_jetty pointers to emulate
         * provider path selection without touching real hardware.
         */
        comp.p_jetty[0] = &phyJetty[0];
        comp.p_jetty[1] = &phyJetty[1];

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

static urma_jfc_t *MockCreatePhysicalJfc(urma_context_t *ctx, urma_jfc_cfg_t *cfg);
static urma_status_t MockDeletePhysicalJfc(urma_jfc_t *jfc);
static urma_jfs_t *MockCreatePhysicalJfs(urma_context_t *ctx, urma_jfs_cfg_t *cfg);
static urma_status_t MockDeletePhysicalJfs(urma_jfs_t *jfs);
static urma_jfr_t *MockCreatePhysicalJfr(urma_context_t *ctx, urma_jfr_cfg_t *cfg);
static urma_status_t MockDeletePhysicalJfr(urma_jfr_t *jfr);
static urma_jetty_t *MockCreatePhysicalJetty(urma_context_t *ctx, urma_jetty_cfg_t *cfg);
static urma_status_t MockDeletePhysicalJetty(urma_jetty_t *jetty);
static urma_target_seg_t *MockRegisterPhysicalSeg(urma_context_t *ctx, urma_seg_cfg_t *cfg);
static urma_status_t MockUnregisterPhysicalSeg(urma_target_seg_t *seg);

struct BondPublicApiFixture {
    bondp_context_t ctx = {};
    bondp_context_t otherBondCtx = {};
    urma_context_t phyCtx = {};
    urma_ops_t phyOps = {};
    urma_device_t dev = {};
    urma_device_t phyDev = {};
    urma_sysfs_dev_t sysfsDev = {};
    bondp_jfc_t jfc = {};
    bondp_jfce_t jfce = {};
    urma_jfce_t phyJfce[2] = {};
    urma_jfc_t phyJfc = {};
    urma_jfr_t phyJfr = {};
    bondp_comp_t jfs = {};
    bondp_comp_t jfr = {};
    bondp_comp_t jetty = {};
    bondp_target_jetty_t targetJetty = {};
    bondp_target_jetty_t targetJfr = {};

    BondPublicApiFixture()
    {
        /* Public API smoke tests exercise stable outer branches; provider/device paths stay mocked by zero members. */
        std::snprintf(dev.name, sizeof(dev.name), "bond_ut");
        std::snprintf(phyDev.name, sizeof(phyDev.name), "bond_phy_ut");
        sysfsDev.dev_attr.dev_cap.max_jfc_depth = 8;
        sysfsDev.dev_attr.dev_cap.max_jfs_depth = 8;
        sysfsDev.dev_attr.dev_cap.max_jfr_depth = 8;
        sysfsDev.dev_attr.dev_cap.max_jfs_inline_len = 64;
        sysfsDev.dev_attr.dev_cap.max_jfs_sge = 8;
        sysfsDev.dev_attr.dev_cap.max_jfs_rsge = 8;
        sysfsDev.dev_attr.dev_cap.max_jfr_sge = 8;
        dev.sysfs_dev = &sysfsDev;
        otherBondCtx.v_ctx.dev = &dev;
        ctx.dev_num = 0;
        ctx.bonding_mode = BONDP_BONDING_MODE_BALANCE;
        ctx.bonding_level = BONDP_BONDING_LEVEL_IODIE;
        ctx.v_ctx.dev = &dev;
        ctx.v_ctx.dev_fd = -1;
        ctx.v_ctx.async_fd = -1;
        phyDev.sysfs_dev = &sysfsDev;
        phyCtx.dev = &phyDev;
        phyCtx.ops = &phyOps;
        phyCtx.eid_index = 0;
        phyCtx.ref.atomic_cnt.store(1);
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

    void InitActiveComp(bondp_comp_t *comp, uint32_t firstIndex)
    {
        comp->enabled_count = 1;
        comp->active_count = 1;
        comp->enabled_indices[0] = firstIndex;
        comp->active_indices[0] = firstIndex;
    }

    void InitJfceFdList()
    {
        jfce.dev_num = 2;
        phyJfce[0].fd = 10;
        phyJfce[1].fd = 11;
        jfce.p_jfce[0] = &phyJfce[0];
        jfce.p_jfce[1] = &phyJfce[1];
    }

    void InitSinglePhysicalMember()
    {
        ctx.dev_num = 1;
        ctx.p_ctxs[0] = &phyCtx;
        phyOps.create_jfc = MockCreatePhysicalJfc;
        phyOps.delete_jfc = MockDeletePhysicalJfc;
        phyOps.create_jfs = MockCreatePhysicalJfs;
        phyOps.delete_jfs = MockDeletePhysicalJfs;
        phyOps.create_jfr = MockCreatePhysicalJfr;
        phyOps.delete_jfr = MockDeletePhysicalJfr;
        phyOps.create_jetty = MockCreatePhysicalJetty;
        phyOps.delete_jetty = MockDeletePhysicalJetty;
        phyOps.register_seg = MockRegisterPhysicalSeg;
        phyOps.unregister_seg = MockUnregisterPhysicalSeg;
        phyJfce[0].urma_ctx = &phyCtx;
        phyJfc.urma_ctx = &phyCtx;
        phyJfc.jfc_id.id = 0x404;
        phyJfr.urma_ctx = &phyCtx;
        phyJfr.jfr_id.id = 0x405;
        phyJfr.jfr_cfg.jfc = &phyJfc;
        jfc.p_jfc[0] = &phyJfc;
        jfr.p_jfr[0] = &phyJfr;
    }
};

static urma_user_ctl_in_t MakeUserCtl(uint32_t opcode, void *addr, uint32_t len)
{
    urma_user_ctl_in_t in = {};

    in.opcode = opcode;
    in.addr = reinterpret_cast<uint64_t>(addr);
    in.len = len;
    return in;
}

static urma_user_ctl_out_t MakeUserCtlOut(void *addr, uint32_t len)
{
    urma_user_ctl_out_t out = {};

    out.addr = reinterpret_cast<uint64_t>(addr);
    out.len = len;
    return out;
}

static int CallBondUserCtl(urma_context_t *ctx, uint32_t opcode, void *addr, uint32_t len,
                           urma_user_ctl_out_t *out)
{
    urma_user_ctl_in_t in = MakeUserCtl(opcode, addr, len);

    return bondp_user_ctl(ctx, &in, out);
}

static urma_jfc_t *MockCreatePhysicalJfc(urma_context_t *ctx, urma_jfc_cfg_t *cfg)
{
    urma_jfc_t *jfc = static_cast<urma_jfc_t *>(std::calloc(1, sizeof(urma_jfc_t)));
    if (jfc == nullptr) {
        return nullptr;
    }
    jfc->urma_ctx = ctx;
    jfc->jfc_cfg = *cfg;
    jfc->jfc_id.id = 0x101;
    return jfc;
}

static urma_status_t MockDeletePhysicalJfc(urma_jfc_t *jfc)
{
    std::free(jfc);
    return URMA_SUCCESS;
}

static urma_jfs_t *MockCreatePhysicalJfs(urma_context_t *ctx, urma_jfs_cfg_t *cfg)
{
    urma_jfs_t *jfs = static_cast<urma_jfs_t *>(std::calloc(1, sizeof(urma_jfs_t)));
    if (jfs == nullptr) {
        return nullptr;
    }
    jfs->urma_ctx = ctx;
    jfs->jfs_cfg = *cfg;
    jfs->jfs_id.id = 0x202;
    return jfs;
}

static urma_status_t MockDeletePhysicalJfs(urma_jfs_t *jfs)
{
    std::free(jfs);
    return URMA_SUCCESS;
}

static urma_jfr_t *MockCreatePhysicalJfr(urma_context_t *ctx, urma_jfr_cfg_t *cfg)
{
    urma_jfr_t *jfr = static_cast<urma_jfr_t *>(std::calloc(1, sizeof(urma_jfr_t)));
    if (jfr == nullptr) {
        return nullptr;
    }
    jfr->urma_ctx = ctx;
    jfr->jfr_cfg = *cfg;
    jfr->jfr_id.id = 0x303;
    return jfr;
}

static urma_status_t MockDeletePhysicalJfr(urma_jfr_t *jfr)
{
    std::free(jfr);
    return URMA_SUCCESS;
}

static urma_jetty_t *MockCreatePhysicalJetty(urma_context_t *ctx, urma_jetty_cfg_t *cfg)
{
    urma_jetty_t *jetty = static_cast<urma_jetty_t *>(std::calloc(1, sizeof(urma_jetty_t)));
    if (jetty == nullptr) {
        return nullptr;
    }
    jetty->urma_ctx = ctx;
    jetty->jetty_cfg = *cfg;
    jetty->jetty_id.id = 0x505;
    return jetty;
}

static urma_status_t MockDeletePhysicalJetty(urma_jetty_t *jetty)
{
    std::free(jetty);
    return URMA_SUCCESS;
}

static urma_target_seg_t *MockRegisterPhysicalSeg(urma_context_t *ctx, urma_seg_cfg_t *cfg)
{
    urma_target_seg_t *seg = static_cast<urma_target_seg_t *>(std::calloc(1, sizeof(urma_target_seg_t)));
    if (seg == nullptr) {
        return nullptr;
    }

    seg->urma_ctx = ctx;
    seg->token_id = cfg->token_id;
    seg->seg.ubva.va = cfg->va;
    seg->seg.len = cfg->len;
    seg->seg.token_id = 0x5151;
    seg->handle = reinterpret_cast<uint64_t>(seg);
    return seg;
}

static urma_status_t MockUnregisterPhysicalSeg(urma_target_seg_t *seg)
{
    std::free(seg);
    return URMA_SUCCESS;
}

static urma_status_t MockPostJfsWr(urma_jfs_t *, urma_jfs_wr_t *wr, urma_jfs_wr_t **badWr)
{
    EXPECT_EQ(URMA_OPC_WRITE, wr->opcode);
    EXPECT_TRUE(wr->flag.bs.has_drv_ext);
    EXPECT_NE(nullptr, wr->rw.src.sge);
    EXPECT_NE(nullptr, wr->rw.dst.sge);
    EXPECT_NE(nullptr, badWr);
    return URMA_SUCCESS;
}

static urma_status_t MockPostAnyJfsWr(urma_jfs_t *, urma_jfs_wr_t *wr, urma_jfs_wr_t **badWr)
{
    EXPECT_NE(nullptr, wr);
    EXPECT_NE(nullptr, badWr);
    *badWr = nullptr;
    return URMA_SUCCESS;
}

static urma_status_t MockPostAnyJfrWr(urma_jfr_t *, urma_jfr_wr_t *wr, urma_jfr_wr_t **badWr)
{
    EXPECT_NE(nullptr, wr);
    EXPECT_NE(nullptr, badWr);
    *badWr = nullptr;
    return URMA_SUCCESS;
}

static urma_cr_t g_mockDatapathCr = {};
static int g_mockDatapathCrCount = 0;

static int MockPollOneCr(urma_jfc_t *, int crCnt, urma_cr_t *cr)
{
    if (g_mockDatapathCrCount <= 0 || crCnt <= 0) {
        return 0;
    }
    cr[0] = g_mockDatapathCr;
    g_mockDatapathCrCount--;
    return 1;
}

static int MockFlushOneCr(urma_jetty_t *, int crCnt, urma_cr_t *cr)
{
    return MockPollOneCr(nullptr, crCnt, cr);
}

static urma_jfc_t *g_mockWaitJfc = nullptr;
static urma_async_event_t g_mockAsyncEvent = {};
static int g_mockAckAsyncCount = 0;

static urma_status_t MockRearmPhysicalJfc(urma_jfc_t *, bool)
{
    return URMA_SUCCESS;
}

static int MockWaitOnePhysicalJfc(urma_jfce_t *, uint32_t, int, urma_jfc_t *jfc[])
{
    jfc[0] = g_mockWaitJfc;
    return 1;
}

static void MockAckPhysicalJfc(urma_jfc_t *[], uint32_t [], uint32_t)
{
}

static urma_status_t MockGetAsyncEvent(urma_context_t *ctx, urma_async_event_t *event)
{
    *event = g_mockAsyncEvent;
    event->urma_ctx = ctx;
    return URMA_SUCCESS;
}

static void MockAckAsyncEvent(urma_async_event_t *)
{
    g_mockAckAsyncCount++;
}

static urma_jetty_id_t MakeJettyId(uint32_t id)
{
    urma_jetty_id_t jettyId = {};

    jettyId.id = id;
    jettyId.uasid = id + 1;
    jettyId.eid.in6.subnet_prefix = 0x10000000ULL + id;
    jettyId.eid.in6.interface_id = 0x20000000ULL + id;
    return jettyId;
}

} // namespace urma_test_bond
