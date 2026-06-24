#pragma once

/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: Shared helpers for URMA core unit tests.
 */

#include <cstdio>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "urma_api.h"
#include "urma_device.h"
#include "urma_log.h"
#include "urma_perf.h"
#include "urma_provider.h"
#include "urma_private.h"

namespace urma_core_test {

void SetCoreIoctlResult(int returnValue, int errorNo);

struct CoreApiFixture {
    urma_device_t dev = {};
    urma_sysfs_dev_t sysfsDev = {};
    urma_ops_t ops = {};
    urma_context_t ctx = {};
    urma_jfce_t jfce = {};
    urma_jfc_t jfc = {};
    urma_jfs_t jfs = {};
    urma_jfr_t jfr = {};
    urma_jetty_t jetty = {};
    urma_token_id_t token = {};
    urma_target_seg_t tseg = {};
    urma_seg_t seg = {};
    urma_target_jetty_t tjfr = {};

    CoreApiFixture()
    {
        std::snprintf(dev.name, sizeof(dev.name), "core_ut");
        dev.type = URMA_TRANSPORT_UB;
        sysfsDev.dev_attr.dev_cap.max_jfc_depth = 16;
        sysfsDev.dev_attr.dev_cap.max_jfs_depth = 16;
        sysfsDev.dev_attr.dev_cap.max_jfr_depth = 16;
        sysfsDev.dev_attr.dev_cap.max_jfs_inline_len = 64;
        sysfsDev.dev_attr.dev_cap.max_jfs_sge = 4;
        sysfsDev.dev_attr.dev_cap.max_jfs_rsge = 4;
        sysfsDev.dev_attr.dev_cap.max_jfr_sge = 4;
        sysfsDev.dev_attr.dev_cap.max_jetty_in_jetty_grp = 4;
        dev.sysfs_dev = &sysfsDev;
        ctx.dev = &dev;
        ctx.ops = &ops;
        ctx.ref.atomic_cnt.store(1);
        jfce.urma_ctx = &ctx;
        jfce.ref.atomic_cnt.store(0);

        jfc.urma_ctx = &ctx;
        jfc.jfc_cfg.depth = 4;
        jfc.jfc_cfg.jfce = &jfce;
        jfc.urma_jfc_opt.is_actived = true;

        jfs.urma_ctx = &ctx;
        jfs.jfs_cfg.depth = 4;
        jfs.jfs_cfg.jfc = &jfc;
        jfs.jfs_cfg.trans_mode = URMA_TM_RC;
        jfs.jfs_cfg.max_sge = 1;
        jfs.jfs_cfg.max_rsge = 1;
        jfs.urma_jfs_opt.is_actived = true;

        jfr.urma_ctx = &ctx;
        jfr.jfr_cfg.depth = 4;
        jfr.jfr_cfg.jfc = &jfc;
        jfr.jfr_cfg.trans_mode = URMA_TM_RC;
        jfr.jfr_cfg.flag.bs.order_type = URMA_OL;
        jfr.jfr_cfg.max_sge = 1;
        jfr.urma_jfr_opt.is_actived = true;

        jetty.urma_ctx = &ctx;
        jetty.jetty_cfg.flag.bs.share_jfr = URMA_SHARE_JFR;
        jetty.jetty_cfg.shared.jfr = &jfr;
        jetty.jetty_cfg.shared.jfc = &jfc;
        jetty.jetty_cfg.jfs_cfg.jfc = &jfc;
        jetty.jetty_cfg.jfs_cfg.depth = 4;
        jetty.jetty_cfg.jfs_cfg.max_sge = 1;
        jetty.jetty_cfg.jfs_cfg.max_rsge = 1;
        jetty.jetty_cfg.jfs_cfg.trans_mode = URMA_TM_RC;
        jetty.urma_jetty_opt.is_actived = true;

        token.urma_ctx = &ctx;
        token.ref.atomic_cnt.store(0);
        tseg.urma_ctx = &ctx;
        tseg.token_id = &token;
        tjfr.urma_ctx = &ctx;
        tjfr.trans_mode = URMA_TM_RM;
        seg.attr.bs.token_policy = URMA_TOKEN_NONE;
    }
};

struct CmdIoctlFailureFixture : CoreApiFixture {
    int pipeFd[2] = { -1, -1 };
    bool eventInit = false;
    urma_cmd_udrv_priv_t udata = {};
    urma_import_tseg_cfg_t importSegCfg = {};
    urma_tjfr_cfg_t tjfrCfg = {};
    urma_tjetty_cfg_t tjettyCfg = {};
    urma_import_jfr_ex_cfg_t importJfrExCfg = {};
    urma_import_jetty_ex_cfg_t importJettyExCfg = {};
    urma_bind_jetty_ex_cfg_t bindJettyExCfg = {};
    urma_notifier_t notifier = {};
    urma_token_t importToken = {};

    ~CmdIoctlFailureFixture()
    {
        if (eventInit) {
            DestroyEventObjects();
        }
        if (pipeFd[0] >= 0) {
            close(pipeFd[0]);
        }
        if (pipeFd[1] >= 0) {
            close(pipeFd[1]);
        }
    }

    bool Init()
    {
        if (pipe(pipeFd) != 0) {
            return false;
        }
        ctx.dev_fd = pipeFd[0];
        ctx.async_fd = pipeFd[0];
        jfce.fd = pipeFd[0];
        notifier.urma_ctx = &ctx;
        notifier.fd = pipeFd[0];
        InitEventObjects();
        InitCommandObjects();
        return true;
    }

private:
    void InitEventObjects()
    {
        ASSERT_EQ(0, pthread_mutex_init(&jfc.event_mutex, nullptr));
        ASSERT_EQ(0, pthread_cond_init(&jfc.event_cond, nullptr));
        ASSERT_EQ(0, pthread_mutex_init(&jfs.event_mutex, nullptr));
        ASSERT_EQ(0, pthread_cond_init(&jfs.event_cond, nullptr));
        ASSERT_EQ(0, pthread_mutex_init(&jfr.event_mutex, nullptr));
        ASSERT_EQ(0, pthread_cond_init(&jfr.event_cond, nullptr));
        ASSERT_EQ(0, pthread_mutex_init(&jetty.event_mutex, nullptr));
        ASSERT_EQ(0, pthread_cond_init(&jetty.event_cond, nullptr));
        eventInit = true;
    }

    void DestroyEventObjects()
    {
        (void)pthread_cond_destroy(&jetty.event_cond);
        (void)pthread_mutex_destroy(&jetty.event_mutex);
        (void)pthread_cond_destroy(&jfr.event_cond);
        (void)pthread_mutex_destroy(&jfr.event_mutex);
        (void)pthread_cond_destroy(&jfs.event_cond);
        (void)pthread_mutex_destroy(&jfs.event_mutex);
        (void)pthread_cond_destroy(&jfc.event_cond);
        (void)pthread_mutex_destroy(&jfc.event_mutex);
    }

    void InitCommandObjects()
    {
        token.urma_ctx = &ctx;
        token.token_id = 0x101;
        token.handle = 0x202;
        tseg.urma_ctx = &ctx;
        tseg.handle = 0x303;
        jfc.handle = 0x404;
        jfc.jfc_id.id = 0x405;
        jfs.handle = 0x505;
        jfs.jfs_id.id = 0x506;
        jfr.handle = 0x606;
        jfr.jfr_id.id = 0x607;
        jetty.handle = 0x707;
        jetty.jetty_id.id = 0x708;
        tjfr.urma_ctx = &ctx;
        tjfr.handle = 0x808;
        tjfr.id.id = 0x809;
        tjfr.trans_mode = URMA_TM_RC;
        tjfr.tp_type = URMA_RTP;
        jetty.remote_jetty = &tjfr;
        importToken.token = 0x909;
        InitImportCfgs();
    }

    void InitImportCfgs()
    {
        importSegCfg.ubva = tseg.seg.ubva;
        importSegCfg.len = 0x1000;
        importSegCfg.token_id = token.token_id;
        importSegCfg.token = &importToken;
        importSegCfg.mva = 0x2000;
        tjfrCfg.jfr_id = tjfr.id;
        tjfrCfg.trans_mode = URMA_TM_RM;
        tjfrCfg.tp_type = URMA_RTP;
        tjfrCfg.token = &importToken;
        tjettyCfg.jetty_id = tjfr.id;
        tjettyCfg.trans_mode = URMA_TM_RC;
        tjettyCfg.tp_type = URMA_RTP;
        tjettyCfg.type = URMA_JETTY;
        tjettyCfg.token = &importToken;
    }
};

struct TempSysfsTree {
    char rootTemplate[64] = "/tmp/urma_core_ut_XXXXXX";
    std::string root;
    std::vector<std::string> files;
    std::vector<std::string> dirs;

    bool Init()
    {
        char *created = mkdtemp(rootTemplate);
        if (created == nullptr) {
            return false;
        }
        root = created;
        return true;
    }

    bool Mkdir(const char *relativePath)
    {
        std::string path = root + "/" + relativePath;
        if (mkdir(path.c_str(), 0700) != 0) {
            return false;
        }
        dirs.push_back(path);
        return true;
    }

    bool WriteFile(const char *relativePath, const char *content)
    {
        std::string path = root + "/" + relativePath;
        FILE *fp = fopen(path.c_str(), "w");
        if (fp == nullptr) {
            return false;
        }
        bool ok = fputs(content, fp) >= 0;
        ok = fclose(fp) == 0 && ok;
        if (ok) {
            files.push_back(path);
        }
        return ok;
    }

    ~TempSysfsTree()
    {
        for (auto it = files.rbegin(); it != files.rend(); ++it) {
            (void)unlink(it->c_str());
        }
        for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
            (void)rmdir(it->c_str());
        }
        if (!root.empty()) {
            (void)rmdir(root.c_str());
        }
    }
};

static urma_jfc_t *MockCreateJfc(urma_context_t *, urma_jfc_cfg_t *cfg)
{
    static urma_jfc_t jfc = {};

    jfc = {};
    jfc.jfc_cfg = *cfg;
    jfc.urma_ctx = cfg->jfce->urma_ctx;
    return &jfc;
}

static urma_status_t MockStatusSuccess()
{
    return URMA_SUCCESS;
}

static urma_status_t MockModifyJfc(urma_jfc_t *, urma_jfc_attr_t *)
{
    return MockStatusSuccess();
}

static urma_status_t MockDeleteJfc(urma_jfc_t *)
{
    return MockStatusSuccess();
}

static urma_status_t MockDeleteJfcBatch(urma_jfc_t **, int, urma_jfc_t **badJfc)
{
    *badJfc = nullptr;
    return URMA_SUCCESS;
}

static urma_status_t MockAllocJfc(urma_context_t *ctx, urma_jfc_cfg_t *cfg, urma_jfc_t **jfc)
{
    static urma_jfc_t mockJfc = {};

    mockJfc.urma_ctx = ctx;
    mockJfc.jfc_cfg = *cfg;
    mockJfc.urma_jfc_opt.is_actived = false;
    *jfc = &mockJfc;
    return URMA_SUCCESS;
}

static urma_status_t MockJfcStatus(urma_jfc_t *)
{
    return URMA_SUCCESS;
}

static urma_status_t MockJfcOpt(urma_jfc_t *, uint64_t, void *, uint32_t)
{
    return URMA_SUCCESS;
}

static urma_status_t MockModifyJfs(urma_jfs_t *, urma_jfs_attr_t *)
{
    return URMA_ENOPERM;
}

static urma_status_t MockQueryJfs(urma_jfs_t *, urma_jfs_cfg_t *, urma_jfs_attr_t *)
{
    return URMA_SUCCESS;
}

static urma_status_t MockDeleteJfsBatch(urma_jfs_t **jfsArr, int, urma_jfs_t **badJfs)
{
    *badJfs = jfsArr[1];
    return URMA_FAIL;
}

static urma_status_t MockDeleteJfsBatchSuccess(urma_jfs_t **, int, urma_jfs_t **badJfs)
{
    *badJfs = nullptr;
    return URMA_SUCCESS;
}

static urma_status_t MockModifyJfr(urma_jfr_t *, urma_jfr_attr_t *)
{
    return URMA_SUCCESS;
}

static urma_status_t MockQueryJfr(urma_jfr_t *, urma_jfr_cfg_t *, urma_jfr_attr_t *)
{
    return URMA_EAGAIN;
}

static urma_status_t MockDeleteJfrBatchSuccess(urma_jfr_t **, int, urma_jfr_t **badJfr)
{
    *badJfr = nullptr;
    return URMA_SUCCESS;
}

static int MockFlushJfs(urma_jfs_t *, int, urma_cr_t *)
{
    return 1;
}

static urma_status_t MockAllocJfs(urma_context_t *ctx, urma_jfs_cfg_t *cfg, urma_jfs_t **jfs)
{
    static urma_jfs_t mockJfs = {};

    mockJfs.urma_ctx = ctx;
    mockJfs.jfs_cfg = *cfg;
    mockJfs.urma_jfs_opt.is_actived = false;
    *jfs = &mockJfs;
    return URMA_SUCCESS;
}

static urma_status_t MockJfsStatus(urma_jfs_t *)
{
    return URMA_SUCCESS;
}

static urma_status_t MockJfsOpt(urma_jfs_t *, uint64_t, void *, uint32_t)
{
    return URMA_SUCCESS;
}

static urma_status_t MockAllocJfr(urma_context_t *ctx, urma_jfr_cfg_t *cfg, urma_jfr_t **jfr)
{
    static urma_jfr_t mockJfr = {};

    mockJfr.urma_ctx = ctx;
    mockJfr.jfr_cfg = *cfg;
    mockJfr.urma_jfr_opt.is_actived = false;
    *jfr = &mockJfr;
    return URMA_SUCCESS;
}

static urma_status_t MockJfrStatus(urma_jfr_t *)
{
    return URMA_SUCCESS;
}

static urma_status_t MockJfrOpt(urma_jfr_t *, uint64_t, void *, uint32_t)
{
    return URMA_SUCCESS;
}

static urma_token_id_t *MockAllocTokenId(urma_context_t *ctx)
{
    static urma_token_id_t token = {};

    token.urma_ctx = ctx;
    token.ref.atomic_cnt.store(0);
    token.token_id = 0xabc;
    return &token;
}

static urma_token_id_t *MockAllocTokenIdEx(urma_context_t *ctx, urma_token_id_flag_t)
{
    return MockAllocTokenId(ctx);
}

static urma_status_t MockFreeTokenId(urma_token_id_t *)
{
    return URMA_SUCCESS;
}

static urma_target_seg_t *MockImportSeg(urma_context_t *ctx, urma_seg_t *, urma_token_t *, uint64_t,
                                        urma_import_seg_flag_t)
{
    static urma_target_seg_t tseg = {};

    tseg = {};
    tseg.urma_ctx = ctx;
    return &tseg;
}

static urma_status_t MockUnimportSeg(urma_target_seg_t *)
{
    return URMA_SUCCESS;
}

static urma_target_seg_t *MockRegisterSeg(urma_context_t *ctx, urma_seg_cfg_t *cfg)
{
    static urma_target_seg_t tseg = {};

    tseg = {};
    tseg.urma_ctx = ctx;
    tseg.token_id = cfg->token_id;
    return &tseg;
}

static urma_status_t MockUnregisterSeg(urma_target_seg_t *)
{
    return URMA_SUCCESS;
}

static urma_status_t MockGetAsyncEvent(urma_context_t *, urma_async_event_t *)
{
    return URMA_SUCCESS;
}

static void MockAckAsyncEvent(urma_async_event_t *)
{
}

static int MockUserCtl(urma_context_t *, urma_user_ctl_in_t *, urma_user_ctl_out_t *)
{
    return URMA_ENOPERM;
}

static int MockModifyTp(urma_context_t *, uint32_t, urma_tp_cfg_t *, urma_tp_attr_t *, urma_tp_attr_mask_t)
{
    return URMA_SUCCESS;
}

static urma_status_t MockGetTpList(urma_context_t *, urma_get_tp_cfg_t *, uint32_t *tpCnt, urma_tp_info_t *)
{
    *tpCnt = 1;
    return URMA_SUCCESS;
}

static urma_status_t MockSetTpAttr(const urma_context_t *, const uint64_t, const uint8_t,
                                   const uint32_t, const urma_tp_attr_value_t *)
{
    return URMA_SUCCESS;
}

static urma_status_t MockGetTpAttr(const urma_context_t *, const uint64_t, uint8_t *tpAttrCnt,
                                   uint32_t *tpAttrBitmap, urma_tp_attr_value_t *)
{
    *tpAttrCnt = 1;
    *tpAttrBitmap = 0;
    return URMA_SUCCESS;
}

static urma_status_t MockNetLookup()
{
    return URMA_SUCCESS;
}

static urma_status_t MockGetEidByIp(const urma_context_t *, const urma_net_addr_t *, urma_eid_t *)
{
    return MockNetLookup();
}

static urma_status_t MockGetIpByEid(const urma_context_t *, const urma_eid_t *, urma_net_addr_t *)
{
    return MockNetLookup();
}

static urma_status_t MockGetSmac(const urma_context_t *, uint8_t *)
{
    return MockNetLookup();
}

static urma_status_t MockGetDmac(const urma_context_t *, const urma_net_addr_t *, uint8_t *)
{
    return MockNetLookup();
}

static urma_jfce_t *MockCreateJfce(urma_context_t *ctx)
{
    static urma_jfce_t jfce = {};

    jfce.urma_ctx = ctx;
    jfce.ref.atomic_cnt.store(0);
    return &jfce;
}

static urma_status_t MockDeleteJfce(urma_jfce_t *)
{
    return URMA_SUCCESS;
}

static urma_jetty_t *MockCreateJetty(urma_context_t *ctx, urma_jetty_cfg_t *cfg)
{
    static urma_jetty_t jetty = {};

    jetty.urma_ctx = ctx;
    jetty.jetty_cfg = *cfg;
    jetty.urma_jetty_opt.is_actived = false;
    return &jetty;
}

static urma_status_t MockJettyStatus(urma_jetty_t *)
{
    return URMA_SUCCESS;
}

static urma_status_t MockModifyJetty(urma_jetty_t *, urma_jetty_attr_t *)
{
    return URMA_SUCCESS;
}

static urma_status_t MockQueryJetty(urma_jetty_t *, urma_jetty_cfg_t *, urma_jetty_attr_t *)
{
    return URMA_SUCCESS;
}

static urma_status_t MockDeleteJettyBatch(urma_jetty_t **, int, urma_jetty_t **badJetty)
{
    *badJetty = nullptr;
    return URMA_SUCCESS;
}

static int MockFlushJetty(urma_jetty_t *, int, urma_cr_t *)
{
    return 1;
}

static urma_status_t MockAllocJetty(urma_context_t *ctx, urma_jetty_cfg_t *cfg, urma_jetty_t **jetty)
{
    static urma_jetty_t mockJetty = {};

    mockJetty.urma_ctx = ctx;
    mockJetty.jetty_cfg = *cfg;
    mockJetty.urma_jetty_opt.is_actived = false;
    *jetty = &mockJetty;
    return URMA_SUCCESS;
}

static urma_status_t MockJettyOpt(urma_jetty_t *, uint64_t, void *, uint32_t)
{
    return URMA_SUCCESS;
}

static urma_target_jetty_t *MockImportJfr(urma_context_t *ctx, urma_rjfr_t *rjfr, urma_token_t *)
{
    static urma_target_jetty_t tjfr = {};

    tjfr.urma_ctx = ctx;
    tjfr.trans_mode = rjfr->trans_mode;
    tjfr.tp_type = rjfr->tp_type;
    return &tjfr;
}

static urma_target_jetty_t *MockImportJfrEx(urma_context_t *ctx, urma_rjfr_t *rjfr, urma_token_t *,
                                            urma_active_tp_cfg_t *)
{
    return MockImportJfr(ctx, rjfr, nullptr);
}

static urma_target_jetty_t *MockImportJetty(urma_context_t *ctx, urma_rjetty_t *rjetty, urma_token_t *)
{
    static urma_target_jetty_t tjetty = {};

    tjetty.urma_ctx = ctx;
    tjetty.trans_mode = rjetty->trans_mode;
    tjetty.flag = rjetty->flag;
    tjetty.tp_type = rjetty->tp_type;
    return &tjetty;
}

static urma_target_jetty_t *MockImportJettyEx(urma_context_t *ctx, urma_rjetty_t *rjetty, urma_token_t *,
                                              urma_active_tp_cfg_t *)
{
    return MockImportJetty(ctx, rjetty, nullptr);
}

static urma_status_t MockTargetJettyStatus(urma_target_jetty_t *)
{
    return URMA_SUCCESS;
}

static urma_status_t MockAdviseJfr(urma_jfs_t *, urma_target_jetty_t *)
{
    return URMA_SUCCESS;
}

static urma_status_t MockAdviseJfrAsync(urma_jfs_t *, urma_target_jetty_t *, urma_advise_async_cb_func, void *)
{
    return URMA_SUCCESS;
}

static urma_status_t MockAdviseJetty(urma_jetty_t *, urma_target_jetty_t *)
{
    return URMA_SUCCESS;
}

static urma_status_t MockBindJetty(urma_jetty_t *, urma_target_jetty_t *)
{
    return URMA_SUCCESS;
}

static urma_status_t MockBindJettyEx(urma_jetty_t *, urma_target_jetty_t *, urma_active_tp_cfg_t *)
{
    return URMA_SUCCESS;
}

static int MockGetTpn(urma_jetty_t *)
{
    return 7;
}

static urma_jetty_grp_t *MockCreateJettyGrp(urma_context_t *ctx, urma_jetty_grp_cfg_t *cfg)
{
    static urma_jetty_grp_t jettyGrp = {};

    jettyGrp.urma_ctx = ctx;
    jettyGrp.cfg = *cfg;
    return &jettyGrp;
}

static urma_status_t MockDeleteJettyGrp(urma_jetty_grp_t *)
{
    return URMA_SUCCESS;
}

static urma_notifier_t *MockCreateNotifier(urma_context_t *ctx)
{
    static urma_notifier_t notifier = {};

    notifier.urma_ctx = ctx;
    return &notifier;
}

static urma_status_t MockDeleteNotifier(urma_notifier_t *)
{
    return URMA_SUCCESS;
}

static int MockWaitNotify(urma_notifier_t *, uint32_t cnt, urma_notify_t *notify, int)
{
    if (cnt > 0) {
        notify[0].status = URMA_SUCCESS;
    }
    return static_cast<int>(cnt);
}

static void MockAckNotify(uint32_t, urma_notify_t *)
{
}

static void MockAdviseCallback(urma_status_t, void *)
{
}

static urma_target_jetty_t *MockImportJettyAsync(urma_notifier_t *notifier, const urma_rjetty_t *rjetty,
                                                 const urma_token_t *, uint64_t, int)
{
    static urma_target_jetty_t tjetty = {};

    tjetty.urma_ctx = notifier->urma_ctx;
    tjetty.trans_mode = rjetty->trans_mode;
    return &tjetty;
}

static urma_status_t MockBindJettyAsync(urma_notifier_t *, urma_jetty_t *, urma_target_jetty_t *, uint64_t, int)
{
    return URMA_SUCCESS;
}

static urma_status_t MockPostJfsWr(urma_jfs_t *, urma_jfs_wr_t *, urma_jfs_wr_t **badWr)
{
    *badWr = nullptr;
    return URMA_SUCCESS;
}

static urma_status_t MockPostJfrWr(urma_jfr_t *, urma_jfr_wr_t *, urma_jfr_wr_t **badWr)
{
    *badWr = nullptr;
    return URMA_SUCCESS;
}

static urma_status_t MockPostJettySendWr(urma_jetty_t *, urma_jfs_wr_t *, urma_jfs_wr_t **badWr)
{
    *badWr = nullptr;
    return URMA_SUCCESS;
}

static urma_status_t MockPostJettyRecvWr(urma_jetty_t *, urma_jfr_wr_t *, urma_jfr_wr_t **badWr)
{
    *badWr = nullptr;
    return URMA_SUCCESS;
}

static int MockPollJfc(urma_jfc_t *, int, urma_cr_t *)
{
    return 1;
}

static urma_status_t MockRearmJfc(urma_jfc_t *, bool)
{
    return URMA_SUCCESS;
}

static int MockWaitJfc(urma_jfce_t *, uint32_t, int, urma_jfc_t *[])
{
    return 1;
}

static void MockAckJfc(urma_jfc_t *[], uint32_t [], uint32_t)
{
}

static urma_status_t MockProviderGetUasid(uint32_t *uasid)
{
    *uasid = 0x5a5a;
    return URMA_SUCCESS;
}

static urma_context_t *MockProviderCreateContext(urma_device_t *dev, uint32_t, int devFd)
{
    static urma_context_t ctx = {};

    ctx.dev = dev;
    ctx.dev_fd = devFd;
    return &ctx;
}

static urma_status_t MockProviderDeleteContext(urma_context_t *)
{
    return URMA_SUCCESS;
}

static urma_status_t MockProviderDeleteContextBusy(urma_context_t *)
{
    return URMA_FAIL;
}

} // namespace urma_core_test
