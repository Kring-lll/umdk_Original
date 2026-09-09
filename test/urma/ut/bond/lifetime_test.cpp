/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bonding deletion and failback lifetime regression tests.
 */
#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <future>
#include <thread>
#include <unistd.h>
#include <gtest/gtest.h>
#include "bondp_context_table.h"
#include "bondp_cp_jetty.h"
#include "bondp_datapath.h"
#include "bondp_dp_failback.h"
#include "bondp_worker.h"
#include "urma_api.h"
#include "urma_cmd_tlv.h"
#include "urma_private.h"
#include "urma_provider.h"

namespace {
std::atomic<unsigned> g_created{0};
std::atomic<unsigned> g_deleted{0};
std::atomic<bool> g_flushEntered{false};
std::atomic<bool> g_releaseFlush{false};
bool g_failPhysical = false;
bool g_failVirtual = false;

urma_jetty_t *CreatePhysical(urma_context_t *ctx, urma_jetty_cfg_t *cfg)
{
    auto *jetty = static_cast<urma_jetty_t *>(calloc(1, sizeof(urma_jetty_t)));
    if (jetty == nullptr) {
        return nullptr;
    }
    jetty->urma_ctx = ctx;
    jetty->jetty_cfg = *cfg;
    jetty->jetty_id.id = g_created.fetch_add(1) + 1;
    return jetty;
}

urma_status_t DeletePhysical(urma_jetty_t *jetty)
{
    if (g_failPhysical && jetty->jetty_id.id == 1) {
        return URMA_EAGAIN;
    }
    free(jetty);
    g_deleted.fetch_add(1);
    return URMA_SUCCESS;
}

struct LifetimeFixture {
    bondp_context_t ctx = {};
    bondp_comp_t jfr = {};
    bondp_jfc_t jfc = {};
    urma_context_t physicalCtx = {};
    urma_device_t dev = {};
    urma_sysfs_dev_t sysfs = {};
    urma_ops_t physicalOps = {};
    urma_ops_t virtualOps = {};
    urma_jfr_t physicalJfr = {};
    urma_jfc_t physicalJfc = {};
    urma_jetty_grp_t group = {};
    urma_jetty_t *members[4] = {};
    bondp_comp_t *comp = nullptr;

    void Init(unsigned physicalCount)
    {
        g_created = 0;
        g_deleted = 0;
        g_flushEntered = false;
        g_releaseFlush = false;
        g_failPhysical = false;
        g_failVirtual = false;
        dev.type = URMA_TRANSPORT_UB;
        dev.sysfs_dev = &sysfs;
        sysfs.dev_attr.dev_cap.max_jfs_depth = 16;
        sysfs.dev_attr.dev_cap.max_jfr_depth = 16;
        sysfs.dev_attr.dev_cap.max_jfs_sge = 4;
        sysfs.dev_attr.dev_cap.max_jfs_rsge = 4;
        sysfs.dev_attr.dev_cap.max_jetty_in_jetty_grp = 4;
        physicalCtx.dev = &dev;
        physicalCtx.ops = &physicalOps;
        physicalCtx.ref.atomic_cnt.store(1);
        physicalOps.create_jetty = CreatePhysical;
        physicalOps.delete_jetty = DeletePhysical;
        virtualOps.delete_jetty = bondp_delete_jetty;
        ctx.v_ctx.dev = &dev;
        ctx.v_ctx.ops = &virtualOps;
        ctx.v_ctx.dev_fd = 7;
        ctx.v_ctx.ref.atomic_cnt.store(2);
        ctx.p_ctxs[0] = &physicalCtx;
        ASSERT_EQ(0, bdp_p_vjetty_id_table_create(&ctx.p_vjetty_id_table, 16));
        comp = static_cast<bondp_comp_t *>(calloc(1, sizeof(bondp_comp_t)));
        ASSERT_NE(nullptr, comp);
        comp->bondp_ctx = &ctx;
        comp->comp_type = BONDP_COMP_JETTY;
        ASSERT_EQ(0, pthread_spin_init(&comp->send_lock, PTHREAD_PROCESS_PRIVATE));
        ASSERT_EQ(0, pthread_mutex_init(&comp->v_jetty.event_mutex, nullptr));
        ASSERT_EQ(0, pthread_cond_init(&comp->v_jetty.event_cond, nullptr));
        physicalJfr.urma_ctx = &physicalCtx;
        physicalJfr.jfr_cfg.jfc = &physicalJfc;
        physicalJfr.jfr_cfg.depth = 4;
        physicalJfr.jfr_cfg.trans_mode = URMA_TM_RC;
        physicalJfr.jfr_cfg.flag.bs.order_type = URMA_OL;
        urma_jetty_cfg_t cfg = {};
        cfg.flag.bs.share_jfr = URMA_SHARE_JFR;
        cfg.jfs_cfg.depth = 4;
        cfg.jfs_cfg.max_sge = 1;
        cfg.jfs_cfg.max_rsge = 1;
        cfg.jfs_cfg.trans_mode = URMA_TM_RC;
        cfg.jfs_cfg.jfc = &physicalJfc;
        cfg.shared.jfr = &physicalJfr;
        for (unsigned i = 0; i < physicalCount; ++i) {
            comp->p_jetty[i] = urma_create_jetty(&physicalCtx, &cfg);
            ASSERT_NE(nullptr, comp->p_jetty[i]);
            ASSERT_EQ(0, bdp_p_vjetty_id_table_add_without_lock(&ctx.p_vjetty_id_table,
                comp->p_jetty[i]->jetty_id, JETTY, 100, comp));
        }
        comp->v_jetty.urma_ctx = &ctx.v_ctx;
        comp->v_jetty.jetty_id.id = 100;
        comp->v_jetty.jetty_cfg = cfg;
        comp->v_jetty.jetty_cfg.shared.jfr = &jfr.v_jfr;
        comp->v_jetty.jetty_cfg.shared.jfc = &jfc.v_jfc;
        comp->v_jetty.urma_jetty_opt.is_actived = true;
        jfr.use_cnt.atomic_cnt.store(1);
        jfc.use_cnt.atomic_cnt.store(1);
        group.urma_ctx = &ctx.v_ctx;
        group.jetty_list = members;
        group.jetty_cnt = 1;
        members[0] = &comp->v_jetty;
        ASSERT_EQ(0, pthread_mutex_init(&group.list_mutex, nullptr));
        comp->v_jetty.jetty_cfg.jetty_grp = &group;
    }

    ~LifetimeFixture()
    {
        if (comp != nullptr) {
            for (auto *jetty : comp->p_jetty) {
                free(jetty);
            }
            (void)pthread_spin_destroy(&comp->send_lock);
            free(comp);
        }
        (void)pthread_mutex_destroy(&group.list_mutex);
        (void)bdp_p_vjetty_id_table_destroy(&ctx.p_vjetty_id_table);
    }
};
} // namespace

extern "C" int ioctl(int, unsigned long request, ...)
{
    va_list args;
    va_start(args, request);
    auto *hdr = va_arg(args, urma_cmd_hdr_t *);
    va_end(args);
    if (request != URMA_CMD || hdr->command != URMA_CMD_DELETE_JETTY || g_failVirtual) {
        errno = EBUSY;
        return -1;
    }
    // The zero-initialized command output reports no asynchronous events.
    return 0;
}

TEST(BondLifetimeTest, DeleteFailuresRetainResourcesAndAllowRetry)
{
    LifetimeFixture fixture;
    fixture.Init(2);
    ASSERT_FALSE(HasFatalFailure());
    auto *jetty = &fixture.comp->v_jetty;
    g_failPhysical = true;
    EXPECT_EQ(URMA_EAGAIN, urma_delete_jetty(jetty));
    EXPECT_NE(nullptr, fixture.comp->p_jetty[0]);
    EXPECT_EQ(nullptr, fixture.comp->p_jetty[1]);
    EXPECT_EQ(1U, g_deleted.load());
    EXPECT_EQ(1U, fixture.group.jetty_cnt);
    EXPECT_EQ(jetty, fixture.members[0]);
    EXPECT_EQ(2UL, fixture.ctx.v_ctx.ref.atomic_cnt.load());
    EXPECT_EQ(1UL, fixture.jfr.use_cnt.atomic_cnt.load());
    EXPECT_EQ(1UL, fixture.jfc.use_cnt.atomic_cnt.load());

    g_failPhysical = false;
    g_failVirtual = true;
    EXPECT_NE(URMA_SUCCESS, urma_delete_jetty(jetty));
    EXPECT_EQ(nullptr, fixture.comp->p_jetty[0]);
    EXPECT_EQ(2U, g_deleted.load());
    EXPECT_EQ(1U, fixture.group.jetty_cnt);
    EXPECT_EQ(1UL, fixture.jfr.use_cnt.atomic_cnt.load());
    EXPECT_EQ(1UL, fixture.jfc.use_cnt.atomic_cnt.load());

    g_failVirtual = false;
    ASSERT_EQ(URMA_SUCCESS, urma_delete_jetty(jetty));
    fixture.comp = nullptr;
    EXPECT_EQ(2U, g_deleted.load());
    EXPECT_EQ(0U, fixture.group.jetty_cnt);
    EXPECT_EQ(1UL, fixture.ctx.v_ctx.ref.atomic_cnt.load());
    EXPECT_EQ(0UL, fixture.jfr.use_cnt.atomic_cnt.load());
    EXPECT_EQ(0UL, fixture.jfc.use_cnt.atomic_cnt.load());
}

TEST(BondLifetimeTest, FailbackWaitsForInFlightPhysicalFlush)
{
    ASSERT_EXIT({
        alarm(10);
        LifetimeFixture fixture;
        fixture.Init(1);
        if (::testing::Test::HasFatalFailure()) {
            _exit(2);
        }
        fixture.physicalOps.flush_jetty = [](urma_jetty_t *jetty, int, urma_cr_t *) {
            g_flushEntered.store(true);
            while (!g_releaseFlush.load()) {
                std::this_thread::yield();
            }
            return jetty->jetty_id.id == 1 ? 0 : -1;
        };
        if (bondp_worker_create() != 0 || bondp_fb_init(&fixture.ctx) != 0) {
            _exit(2);
        }
        auto flush = std::async(std::launch::async, [&]() {
            urma_cr_t cr = {};
            return bondp_flush_jetty(&fixture.comp->v_jetty, 1, &cr);
        });
        while (!g_flushEntered.load()) {
            std::this_thread::yield();
        }
        EXPECT_EQ(0, bondp_fb_add_task(&fixture.ctx, 100, 0));
        while (g_created.load() < 2) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        EXPECT_EQ(0U, g_deleted.load());
        g_releaseFlush.store(true);
        EXPECT_EQ(0, flush.get());
        bondp_fb_cancel_tasks(&fixture.ctx, 100);
        bondp_worker_destroy();
        bondp_fb_uninit(&fixture.ctx);
        EXPECT_EQ(1U, g_deleted.load());
        _exit(::testing::Test::HasFailure() ? 1 : 0);
    }, ::testing::ExitedWithCode(0), "");
}
