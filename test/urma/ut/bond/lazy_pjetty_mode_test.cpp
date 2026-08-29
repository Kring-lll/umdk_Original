/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: URMA bonding lazy pjetty mode unit tests.
 */

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include <gtest/gtest.h>

#include "bondp_cp_tjetty.h"
#include "bondp_cp_user_ctl.h"
#include "bondp_context_table.h"
#include "bondp_datapath.h"
#include "bondp_dp_failback.h"
#include "bondp_topo_info.h"
#include "bondp_types.h"
#include "bondp_worker.h"
#include "urma_ubagg.h"

namespace {

uint32_t gPhysicalImportCount = 0;
uint32_t gPhysicalUnimportCount = 0;
std::atomic_uint gPhysicalCreateCount = 0;
std::atomic_uint gPhysicalDeleteCount = 0;
std::atomic_uint gPhysicalDeleteFailures = 0;
std::atomic_bool gPhysicalCreateSucceeds = false;
urma_target_jetty_t gPhysicalTarget = {};
urma_jetty_t gCreatedJetty = {};

} // namespace

extern "C" urma_target_jetty_t *urma_import_jetty(urma_context_t *ctx, urma_rjetty_t *rjetty,
                                                   urma_token_t *)
{
    gPhysicalImportCount++;
    gPhysicalTarget = {};
    gPhysicalTarget.urma_ctx = ctx;
    gPhysicalTarget.id = rjetty->jetty_id;
    gPhysicalTarget.trans_mode = rjetty->trans_mode;
    gPhysicalTarget.policy = rjetty->policy;
    gPhysicalTarget.type = rjetty->type;
    gPhysicalTarget.flag = rjetty->flag;
    gPhysicalTarget.tp_type = rjetty->tp_type;
    return &gPhysicalTarget;
}

extern "C" urma_status_t urma_unimport_jetty(urma_target_jetty_t *)
{
    gPhysicalUnimportCount++;
    return URMA_SUCCESS;
}

extern "C" urma_jetty_t *urma_create_jetty(urma_context_t *ctx, urma_jetty_cfg_t *cfg)
{
    gPhysicalCreateCount++;
    if (!gPhysicalCreateSucceeds.load()) {
        return nullptr;
    }
    gCreatedJetty = {};
    gCreatedJetty.urma_ctx = ctx;
    gCreatedJetty.jetty_cfg = *cfg;
    gCreatedJetty.jetty_id.id = 0x501 + gPhysicalCreateCount.load();
    return &gCreatedJetty;
}

extern "C" urma_status_t urma_delete_jetty(urma_jetty_t *)
{
    gPhysicalDeleteCount++;
    if (gPhysicalDeleteFailures.load() != 0) {
        gPhysicalDeleteFailures--;
        return URMA_FAIL;
    }
    return URMA_SUCCESS;
}

namespace {

constexpr uint32_t TEST_UASID = 0x71;

urma_eid_t MakeEid(uint32_t value)
{
    urma_eid_t eid = {};

    eid.in6.subnet_prefix = 0x10000000ULL + value;
    eid.in6.interface_id = 0x20000000ULL + value;
    return eid;
}

urma_jetty_id_t MakeJettyId(uint32_t value)
{
    urma_jetty_id_t id = {};

    id.eid = MakeEid(value);
    id.uasid = TEST_UASID;
    id.id = value;
    return id;
}

void CopyEidToTopo(char dst[EID_LEN], const urma_eid_t &eid)
{
    static_assert(sizeof(urma_eid_t) == EID_LEN, "topology EID size mismatch");
    std::memcpy(dst, &eid, sizeof(eid));
}

size_t CalcRjettyExtLen(uint32_t localCnt, uint32_t targetCnt)
{
    return sizeof(urma_bond_jetty_ext_v0_t) + localCnt * sizeof(uint8_t) +
           targetCnt * sizeof(bondp_rjetty_target_ctx_t);
}

const urma_bond_jetty_ext_v0_t *GetCompactExt(const urma_rjetty_t *rjetty)
{
    const bondp_rjetty_ext_priv_t *extHdr = bondp_rjetty_get_priv_ext_const(rjetty);
    return reinterpret_cast<const urma_bond_jetty_ext_v0_t *>(extHdr->data);
}

bondp_rjetty_target_ctx_t GetTargetEntry(const urma_bond_jetty_ext_v0_t *ext, uint32_t ordinal)
{
    bondp_rjetty_target_ctx_t entry = {};
    const uint8_t *targetBytes = reinterpret_cast<const uint8_t *>(ext) + sizeof(*ext) +
                                 ext->local_ctx_cnt;

    std::memcpy(&entry, targetBytes + ordinal * sizeof(entry), sizeof(entry));
    return entry;
}

urma_rjetty_t *AllocRemoteRjetty(bool lazy, const uint32_t *slaveIds, uint32_t targetCount)
{
    constexpr uint32_t LOCAL_COUNT = 1;
    size_t extLen = CalcRjettyExtLen(LOCAL_COUNT, targetCount);
    size_t totalLen = sizeof(urma_rjetty_t) + sizeof(bondp_rjetty_ext_priv_t) + extLen;
    auto *remote = static_cast<urma_rjetty_t *>(std::calloc(1, totalLen));
    if (remote == nullptr) {
        return nullptr;
    }

    remote->flag.bs.has_user_info = 1;
    remote->jetty_id = MakeJettyId(0x301);
    remote->trans_mode = URMA_TM_RM;
    remote->type = URMA_JETTY;

    bondp_rjetty_ext_priv_t *extHdr = bondp_rjetty_get_priv_ext(remote);
    extHdr->len = static_cast<uint32_t>(extLen);
    auto *ext = reinterpret_cast<urma_bond_jetty_ext_v0_t *>(extHdr->data);
    ext->version = BONDP_RJETTY_EXT_VERSION_V0;
    ext->mask = BONDP_RJETTY_EXT_MASK_LOCAL_CTX | BONDP_RJETTY_EXT_MASK_TARGET_CTX;
    if (lazy) {
        ext->mask |= BONDP_RJETTY_EXT_MASK_LAZY_PJETTY;
    }
    ext->local_ctx_cnt = LOCAL_COUNT;
    ext->target_ctx_cnt = targetCount;

    auto *localIndices = reinterpret_cast<uint8_t *>(ext) + sizeof(*ext);
    localIndices[0] = 0;
    for (uint32_t i = 0; i < targetCount; ++i) {
        bondp_rjetty_target_ctx_t entry = {};
        entry.target_idx = static_cast<uint8_t>(i);
        entry.eid = MakeEid(0x302 + i);
        entry.jetty_id = slaveIds[i];
        std::memcpy(localIndices + LOCAL_COUNT + i * sizeof(entry), &entry, sizeof(entry));
    }
    return remote;
}

class LazyPjettyModeTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_EQ(0, pthread_mutex_init(&ctx_.v_ctx.mutex, nullptr));
        ASSERT_EQ(0, pthread_spin_init(&lazyComp_.send_lock, PTHREAD_PROCESS_PRIVATE));
        ASSERT_EQ(0, pthread_spin_init(&fullComp_.send_lock, PTHREAD_PROCESS_PRIVATE));
        ASSERT_EQ(0, bdp_p_vjetty_id_table_create(&ctx_.p_vjetty_id_table, 16));

        ctx_.enable_failover = true;
        ctx_.enable_failback = true;
        ctx_.bonding_mode = BONDP_BONDING_MODE_ACTIVE_BACKUP;
        ctx_.bonding_level = BONDP_BONDING_LEVEL_IODIE;
        ctx_.default_pjetty_create_mode = BONDP_PJETTY_CREATE_FULL;
        ctx_.enabled_count = 1;
        ctx_.enabled_indices[0] = 0;
        ctx_.p_ctxs[0] = &physicalCtx_;
        gPhysicalImportCount = 0;
        gPhysicalUnimportCount = 0;
        gPhysicalCreateCount.store(0);
        gPhysicalDeleteCount.store(0);
        gPhysicalDeleteFailures.store(0);
        gPhysicalCreateSucceeds.store(false);
        gPhysicalTarget = {};
        gCreatedJetty = {};

        InitComp(&lazyComp_, 0x101);
        InitComp(&fullComp_, 0x102);

        lazyPrimary_.jetty_id = MakeJettyId(0x201);
        fullPrimary_.jetty_id = MakeJettyId(0x211);
        fullBackup_.jetty_id = MakeJettyId(0x212);
        lazyComp_.p_jetty[0] = &lazyPrimary_;
        fullComp_.p_jetty[0] = &fullPrimary_;
        fullComp_.p_jetty[1] = &fullBackup_;
        ASSERT_EQ(0, bdp_p_vjetty_id_table_add_without_lock(
            &ctx_.p_vjetty_id_table, lazyPrimary_.jetty_id, JETTY,
            lazyComp_.v_jetty.jetty_id.id, &lazyComp_));
    }

    void TearDown() override
    {
        bondp_topo_uninit();
        EXPECT_EQ(0, bondp_fb_uninit(&ctx_));
        if (workerStarted_) {
            bondp_worker_destroy();
        }
        EXPECT_EQ(0, bdp_p_vjetty_id_table_destroy(&ctx_.p_vjetty_id_table));
        (void)pthread_spin_destroy(&fullComp_.send_lock);
        (void)pthread_spin_destroy(&lazyComp_.send_lock);
        (void)pthread_mutex_destroy(&ctx_.v_ctx.mutex);
    }

    void StartWorker()
    {
        ASSERT_FALSE(workerStarted_);
        ASSERT_EQ(0, bondp_worker_create());
        workerStarted_ = true;
    }

    bool WaitForRebuildDone(uint32_t idx)
    {
        for (uint32_t retry = 0; retry < 300; ++retry) {
            if (lazyComp_.rebuild_done[idx].load()) {
                return true;
            }
            usleep(10000);
        }
        return false;
    }

    bool WaitForPhysicalCreate(uint32_t expected)
    {
        for (uint32_t retry = 0; retry < 300; ++retry) {
            if (gPhysicalCreateCount.load() >= expected) {
                return true;
            }
            usleep(10000);
        }
        return false;
    }

    bool WaitForPjettyDestroyed(uint32_t idx)
    {
        for (uint32_t retry = 0; retry < 400; ++retry) {
            (void)pthread_spin_lock(&lazyComp_.send_lock);
            bool destroyed = lazyComp_.p_jetty[idx] == nullptr;
            (void)pthread_spin_unlock(&lazyComp_.send_lock);
            if (destroyed) {
                return true;
            }
            usleep(10000);
        }
        return false;
    }

    void InitComp(bondp_comp_t *comp, uint32_t id)
    {
        comp->bondp_ctx = &ctx_;
        comp->comp_type = BONDP_COMP_JETTY;
        comp->v_jetty.urma_ctx = &ctx_.v_ctx;
        comp->v_jetty.jetty_id = MakeJettyId(id);
        comp->v_jetty.jetty_cfg.jfs_cfg.trans_mode = URMA_TM_RM;
        comp->enabled_count = 2;
        comp->enabled_indices[0] = 0;
        comp->enabled_indices[1] = 1;
        comp->active_count = 2;
        comp->active_indices[0] = 0;
        comp->active_indices[1] = 1;
    }

    int SetMode(uint32_t mode)
    {
        bondp_set_pjetty_create_mode_in_t modeIn = {
            .version = BONDP_PJETTY_CREATE_CFG_VERSION_V1,
            .length = sizeof(modeIn),
            .mode = mode,
            .flags = 0,
            .reserved = 0,
        };
        urma_user_ctl_in_t in = {
            .addr = reinterpret_cast<uint64_t>(&modeIn),
            .len = sizeof(modeIn),
            .opcode = BONDP_USER_CTL_SET_PJETTY_CREATE_MODE,
        };
        return bondp_user_ctl(&ctx_.v_ctx, &in, nullptr);
    }

    int SetModeWithPayload(const bondp_set_pjetty_create_mode_in_t *modeIn, uint32_t inputLen)
    {
        urma_user_ctl_in_t in = {
            .addr = reinterpret_cast<uint64_t>(modeIn),
            .len = inputLen,
            .opcode = BONDP_USER_CTL_SET_PJETTY_CREATE_MODE,
        };
        return bondp_user_ctl(&ctx_.v_ctx, &in, nullptr);
    }

    urma_rjetty_t *GetRjetty(bondp_comp_t *comp)
    {
        urma_rjetty_t *rjetty = nullptr;
        urma_user_ctl_in_t in = {
            .addr = reinterpret_cast<uint64_t>(&comp->v_jetty),
            .len = sizeof(comp->v_jetty),
            .opcode = BONDP_USER_CTL_OPCODE_GET_RJETTY,
        };
        urma_user_ctl_out_t out = {
            .addr = reinterpret_cast<uint64_t>(&rjetty),
            .len = sizeof(rjetty),
            .reserved = 0,
        };

        EXPECT_EQ(0, bondp_user_ctl(&ctx_.v_ctx, &in, &out));
        return rjetty;
    }

    bondp_context_t ctx_ = {};
    bondp_comp_t lazyComp_ = {};
    bondp_comp_t fullComp_ = {};
    urma_jetty_t lazyPrimary_ = {};
    urma_jetty_t fullPrimary_ = {};
    urma_jetty_t fullBackup_ = {};
    urma_context_t physicalCtx_ = {};
    bool workerStarted_ = false;
};

TEST_F(LazyPjettyModeTest, SetModeValidatesVersionedInputAndPreservesStateOnFailure)
{
    bondp_set_pjetty_create_mode_in_t input = {
        .version = BONDP_PJETTY_CREATE_CFG_VERSION_V1,
        .length = sizeof(input),
        .mode = BONDP_PJETTY_CREATE_LAZY_BY_IODIE,
        .flags = 0,
        .reserved = 0,
    };
    urma_user_ctl_in_t validCtl = {
        .addr = reinterpret_cast<uint64_t>(&input),
        .len = sizeof(input),
        .opcode = BONDP_USER_CTL_SET_PJETTY_CREATE_MODE,
    };

    EXPECT_EQ(-EINVAL, bondp_user_ctl(&ctx_.v_ctx, nullptr, nullptr));
    EXPECT_EQ(-EINVAL, bondp_user_ctl(nullptr, &validCtl, nullptr));
    EXPECT_EQ(-EINVAL, SetModeWithPayload(nullptr, sizeof(input)));
    EXPECT_EQ(-EINVAL, SetModeWithPayload(&input, sizeof(input) - 1));
    EXPECT_EQ(-EINVAL, SetModeWithPayload(&input, sizeof(input) + 1));
    EXPECT_EQ(BONDP_PJETTY_CREATE_FULL, ctx_.default_pjetty_create_mode);
    EXPECT_EQ(nullptr, ctx_.fb_ctx);

    input.version = 0;
    EXPECT_EQ(-EINVAL, SetModeWithPayload(&input, sizeof(input)));
    input.version = BONDP_PJETTY_CREATE_CFG_VERSION_V1;
    input.length = sizeof(input) - 1;
    EXPECT_EQ(-EINVAL, SetModeWithPayload(&input, sizeof(input)));
    input.length = sizeof(input);
    input.mode = BONDP_PJETTY_CREATE_MODE_MAX;
    EXPECT_EQ(-EINVAL, SetModeWithPayload(&input, sizeof(input)));
    input.mode = BONDP_PJETTY_CREATE_LAZY_BY_IODIE;
    input.flags = 1;
    EXPECT_EQ(-EINVAL, SetModeWithPayload(&input, sizeof(input)));
    input.flags = 0;
    input.reserved = 1;
    EXPECT_EQ(-EINVAL, SetModeWithPayload(&input, sizeof(input)));
    input.reserved = 0;
    EXPECT_EQ(BONDP_PJETTY_CREATE_FULL, ctx_.default_pjetty_create_mode);
    EXPECT_EQ(nullptr, ctx_.fb_ctx);

    ctx_.enable_failover = false;
    EXPECT_EQ(-EINVAL, SetModeWithPayload(&input, sizeof(input)));
    ctx_.enable_failover = true;
    ctx_.bonding_mode = BONDP_BONDING_MODE_BALANCE;
    EXPECT_EQ(-EINVAL, SetModeWithPayload(&input, sizeof(input)));
    ctx_.bonding_mode = BONDP_BONDING_MODE_ACTIVE_BACKUP;
    EXPECT_EQ(BONDP_PJETTY_CREATE_FULL, ctx_.default_pjetty_create_mode);
    EXPECT_EQ(nullptr, ctx_.fb_ctx);

    EXPECT_EQ(0, SetMode(BONDP_PJETTY_CREATE_LAZY_BY_IODIE));
    EXPECT_EQ(BONDP_PJETTY_CREATE_LAZY_BY_IODIE, ctx_.default_pjetty_create_mode);
    ASSERT_NE(nullptr, ctx_.fb_ctx);
    bondp_fb_ctx_t *fbCtx = ctx_.fb_ctx;

    EXPECT_EQ(0, SetMode(BONDP_PJETTY_CREATE_FULL));
    EXPECT_EQ(BONDP_PJETTY_CREATE_FULL, ctx_.default_pjetty_create_mode);
    EXPECT_EQ(fbCtx, ctx_.fb_ctx);
}

TEST_F(LazyPjettyModeTest, ExistingJettySnapshotControlsLazyAndFullRjettyExt)
{
    ASSERT_EQ(0, SetMode(BONDP_PJETTY_CREATE_LAZY_BY_IODIE));
    lazyComp_.pjetty_create_mode = ctx_.default_pjetty_create_mode;

    ASSERT_EQ(0, SetMode(BONDP_PJETTY_CREATE_FULL));
    EXPECT_EQ(BONDP_PJETTY_CREATE_LAZY_BY_IODIE, lazyComp_.pjetty_create_mode);
    urma_rjetty_t *lazyRjetty = GetRjetty(&lazyComp_);
    ASSERT_NE(nullptr, lazyRjetty);

    fullComp_.pjetty_create_mode = ctx_.default_pjetty_create_mode;
    ASSERT_EQ(0, SetMode(BONDP_PJETTY_CREATE_LAZY_BY_IODIE));
    EXPECT_EQ(BONDP_PJETTY_CREATE_FULL, fullComp_.pjetty_create_mode);
    urma_rjetty_t *fullRjetty = GetRjetty(&fullComp_);
    ASSERT_NE(nullptr, fullRjetty);

    const urma_bond_jetty_ext_v0_t *lazyExt = GetCompactExt(lazyRjetty);
    ASSERT_EQ(2U, lazyExt->target_ctx_cnt);
    EXPECT_NE(0U, lazyExt->mask & BONDP_RJETTY_EXT_MASK_LAZY_PJETTY);
    bondp_rjetty_target_ctx_t lazyPrimary = GetTargetEntry(lazyExt, 0);
    bondp_rjetty_target_ctx_t lazyBackup = GetTargetEntry(lazyExt, 1);
    EXPECT_EQ(0U, lazyPrimary.target_idx);
    EXPECT_NE(0U, lazyPrimary.jetty_id);
    EXPECT_EQ(1U, lazyBackup.target_idx);
    EXPECT_EQ(0U, lazyBackup.jetty_id);

    const urma_bond_jetty_ext_v0_t *fullExt = GetCompactExt(fullRjetty);
    ASSERT_EQ(2U, fullExt->target_ctx_cnt);
    EXPECT_EQ(0U, fullExt->mask & BONDP_RJETTY_EXT_MASK_LAZY_PJETTY);
    bondp_rjetty_target_ctx_t fullPrimary = GetTargetEntry(fullExt, 0);
    bondp_rjetty_target_ctx_t fullBackup = GetTargetEntry(fullExt, 1);
    EXPECT_NE(0U, fullPrimary.jetty_id);
    EXPECT_NE(0U, fullBackup.jetty_id);

    std::free(fullRjetty);
    std::free(lazyRjetty);
}

TEST_F(LazyPjettyModeTest, ImportRequiresAtLeastOneUsableRemotePjetty)
{
    auto *topo = static_cast<bondp_topo_node_t *>(std::calloc(2, sizeof(bondp_topo_node_t)));
    ASSERT_NE(nullptr, topo);
    topo[0].is_current = true;
    CopyEidToTopo(topo[0].agg_devs[0].agg_eid, MakeEid(0x401));
    CopyEidToTopo(topo[0].agg_devs[0].ues[0].primary_eid, MakeEid(0x402));
    CopyEidToTopo(topo[0].agg_devs[0].ues[0].port_eid[0], MakeEid(0x403));
    CopyEidToTopo(topo[1].agg_devs[0].agg_eid, MakeEid(0x301));
    topo[1].links[0][0] = true;
    topo[1].links[0][PORT_NUM] = true;
    ASSERT_EQ(0, bondp_topo_init(topo, 2));
    std::free(topo);

    urma_token_t token = {};
    const uint32_t allZero[] = {0};
    urma_rjetty_t *fullRemote = AllocRemoteRjetty(false, allZero, 1);
    ASSERT_NE(nullptr, fullRemote);
    EXPECT_EQ(nullptr, bondp_import_jetty(&ctx_.v_ctx, fullRemote, &token));
    std::free(fullRemote);

    urma_rjetty_t *lazyAllZero = AllocRemoteRjetty(true, allZero, 1);
    ASSERT_NE(nullptr, lazyAllZero);
    EXPECT_EQ(nullptr, bondp_import_jetty(&ctx_.v_ctx, lazyAllZero, &token));
    std::free(lazyAllZero);
    EXPECT_EQ(0U, gPhysicalImportCount);

    const uint32_t partialZero[] = {0x311, 0};
    urma_rjetty_t *lazyPartialZero = AllocRemoteRjetty(true, partialZero, 2);
    ASSERT_NE(nullptr, lazyPartialZero);
    urma_target_jetty_t *target = bondp_import_jetty(&ctx_.v_ctx, lazyPartialZero, &token);
    ASSERT_NE(nullptr, target);
    auto *bondTarget = CONTAINER_OF_FIELD(target, bondp_target_jetty_t, v_tjetty);
    ASSERT_EQ(2U, bondTarget->p_tjetty_count);
    EXPECT_EQ(&gPhysicalTarget, bondTarget->p_tjettys[0].p_tjetty);
    EXPECT_TRUE(bondTarget->p_tjettys[0].valid.load());
    EXPECT_EQ(nullptr, bondTarget->p_tjettys[1].p_tjetty);
    EXPECT_FALSE(bondTarget->p_tjettys[1].valid.load());
    EXPECT_EQ(1U, gPhysicalImportCount);
    EXPECT_EQ(URMA_SUCCESS, bondp_unimport_jetty(target));
    EXPECT_EQ(1U, gPhysicalUnimportCount);
    std::free(lazyPartialZero);
}

TEST_F(LazyPjettyModeTest, WireRemoteLazyRejectsPeerBindAndPost)
{
    auto *topo = static_cast<bondp_topo_node_t *>(std::calloc(2, sizeof(bondp_topo_node_t)));
    ASSERT_NE(nullptr, topo);
    topo[0].is_current = true;
    CopyEidToTopo(topo[0].agg_devs[0].agg_eid, MakeEid(0x401));
    CopyEidToTopo(topo[0].agg_devs[0].ues[0].primary_eid, MakeEid(0x402));
    CopyEidToTopo(topo[0].agg_devs[0].ues[0].port_eid[0], MakeEid(0x403));
    CopyEidToTopo(topo[1].agg_devs[0].agg_eid, MakeEid(0x301));
    topo[1].links[0][0] = true;
    ASSERT_EQ(0, bondp_topo_init(topo, 2));
    std::free(topo);

    const uint32_t validSlave[] = {0x321};
    urma_rjetty_t *wireRemote = AllocRemoteRjetty(true, validSlave, 1);
    ASSERT_NE(nullptr, wireRemote);

    urma_token_t token = {};
    urma_target_jetty_t *target = bondp_import_jetty(&ctx_.v_ctx, wireRemote, &token);
    ASSERT_NE(nullptr, target);
    auto *bondTarget = CONTAINER_OF_FIELD(target, bondp_target_jetty_t, v_tjetty);
    EXPECT_NE(0U, bondTarget->mask & BONDP_TJETTY_FLAG_REMOTE_LAZY);
    EXPECT_EQ(URMA_ENOPERM, bondp_bind_jetty(&fullComp_.v_jetty, target));
    EXPECT_EQ(nullptr, fullComp_.v_jetty.remote_jetty);

    urma_jfs_wr_t wr = {};
    wr.opcode = URMA_OPC_SEND;
    wr.tjetty = target;
    urma_jfs_wr_t *badWr = nullptr;
    EXPECT_EQ(URMA_ENOPERM, bondp_post_jetty_send_wr(&fullComp_.v_jetty, &wr, &badWr));
    EXPECT_EQ(&wr, badWr);
    EXPECT_EQ(1U, gPhysicalImportCount);
    EXPECT_EQ(URMA_SUCCESS, bondp_unimport_jetty(target));
    EXPECT_EQ(1U, gPhysicalUnimportCount);
    std::free(wireRemote);
}

TEST_F(LazyPjettyModeTest, ModifyToErrorCancelsLazyCreateBeforePhysicalCreate)
{
    constexpr uint32_t COLD_IDX = 1;
    ctx_.p_ctxs[COLD_IDX] = &physicalCtx_;
    lazyComp_.modify_to_error.store(true);
    lazyComp_.valid[COLD_IDX].store(false);
    bondp_lazy_create_info_t info = {};
    info.new_send_idx = COLD_IDX;
    info.new_target_idx = COLD_IDX;
    info.new_send_idx2 = BONDP_LAZY_IDX_NONE;
    info.new_target_idx2 = BONDP_LAZY_IDX_NONE;

    EXPECT_NE(0, bondp_lazy_create_and_resend(&lazyComp_, COLD_IDX, &info));
    EXPECT_EQ(nullptr, lazyComp_.p_jetty[COLD_IDX]);
    EXPECT_FALSE(lazyComp_.valid[COLD_IDX].load());
    EXPECT_EQ(0U, gPhysicalCreateCount.load());
}

TEST_F(LazyPjettyModeTest, PendingLazyCreateWithNoRoutePreservesMsnAndSetsBadWr)
{
    constexpr uint32_t INITIAL_MSN = 0x55;
    auto *target = static_cast<bondp_target_jetty_t *>(std::calloc(1, sizeof(bondp_target_jetty_t)));
    ASSERT_NE(nullptr, target);
    target->v_tjetty.urma_ctx = &ctx_.v_ctx;
    target->active_count = 1;
    target->mask |= BONDP_TJETTY_FLAG_MSN_ENABLED;

    lazyComp_.pjetty_create_mode = BONDP_PJETTY_CREATE_LAZY_BY_IODIE;
    lazyComp_.lazy_create_pending.store(true);
    lazyComp_.msn.store(INITIAL_MSN);
    urma_jfs_wr_t wr = {};
    wr.opcode = URMA_OPC_SEND;
    wr.tjetty = &target->v_tjetty;
    urma_jfs_wr_t *badWr = nullptr;

    EXPECT_EQ(URMA_EAGAIN, bondp_post_jetty_send_wr(&lazyComp_.v_jetty, &wr, &badWr));
    EXPECT_EQ(&wr, badWr);
    EXPECT_EQ(INITIAL_MSN, lazyComp_.msn.load());
    std::free(target);
}

TEST_F(LazyPjettyModeTest, RebuildPublishesRecoveryForHealthCheckFailback)
{
    ASSERT_EQ(0, SetMode(BONDP_PJETTY_CREATE_LAZY_BY_IODIE));
    lazyComp_.pjetty_create_mode = BONDP_PJETTY_CREATE_LAZY_BY_IODIE;
    lazyComp_.valid[0].store(false);
    lazyComp_.rebuild_done[0].store(false);
    gPhysicalCreateSucceeds.store(false);
    StartWorker();

    ASSERT_EQ(0, bondp_fb_add_task(&ctx_, lazyComp_.v_jetty.jetty_id.id, 0));
    ASSERT_TRUE(WaitForPhysicalCreate(1));
    EXPECT_FALSE(lazyComp_.rebuild_done[0].load());
    gPhysicalCreateSucceeds.store(true);
    ASSERT_TRUE(WaitForRebuildDone(0));
    EXPECT_NE(&lazyPrimary_, lazyComp_.p_jetty[0]);
    EXPECT_GE(gPhysicalCreateCount.load(), 2U);
    EXPECT_EQ(1U, gPhysicalDeleteCount.load());

    lazyComp_.hc_valid[0].store(true);
    EXPECT_TRUE(bondp_fb_need_switch_path(&lazyComp_));
    EXPECT_EQ(1U, bondp_fb_switch_path(&lazyComp_));
    EXPECT_TRUE(lazyComp_.valid[0].load());
}

TEST_F(LazyPjettyModeTest, FailbackDestroysMaterializedColdStandby)
{
    constexpr uint32_t COLD_IDX = 1;
    ASSERT_EQ(0, SetMode(BONDP_PJETTY_CREATE_LAZY_BY_IODIE));
    lazyComp_.pjetty_create_mode = BONDP_PJETTY_CREATE_LAZY_BY_IODIE;
    ctx_.p_ctxs[COLD_IDX] = &physicalCtx_;
    lazyComp_.p_jetty[COLD_IDX] = &fullBackup_;
    lazyComp_.valid[0].store(true);
    lazyComp_.valid[COLD_IDX].store(true);
    lazyComp_.hc_valid[COLD_IDX].store(true);
    lazyComp_.rebuild_done[COLD_IDX].store(true);
    lazyComp_.sqe_cnt[COLD_IDX][0].store(1);
    gPhysicalDeleteFailures.store(1);
    ASSERT_EQ(0, bdp_p_vjetty_id_table_add_without_lock(
        &ctx_.p_vjetty_id_table, fullBackup_.jetty_id, JETTY,
        lazyComp_.v_jetty.jetty_id.id, &lazyComp_));
    StartWorker();

    bondp_fb_schedule_lazy_destroy(&lazyComp_);
    usleep(100000);
    (void)pthread_spin_lock(&lazyComp_.send_lock);
    EXPECT_EQ(&fullBackup_, lazyComp_.p_jetty[COLD_IDX]);
    (void)pthread_spin_unlock(&lazyComp_.send_lock);
    EXPECT_EQ(0U, gPhysicalDeleteCount.load());
    lazyComp_.sqe_cnt[COLD_IDX][0].store(0);
    ASSERT_TRUE(WaitForPjettyDestroyed(COLD_IDX));
    EXPECT_FALSE(lazyComp_.valid[COLD_IDX].load());
    EXPECT_FALSE(lazyComp_.hc_valid[COLD_IDX].load());
    EXPECT_FALSE(lazyComp_.rebuild_done[COLD_IDX].load());
    EXPECT_EQ(2U, gPhysicalDeleteCount.load());
}

} // namespace
