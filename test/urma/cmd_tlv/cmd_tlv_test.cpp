/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: URMA command TLV unit tests.
 */

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "urma_cmd_mock.h"

namespace {

struct IoctlCapture {
    int fd;
    unsigned long request;
    urma_cmd_hdr_t hdr;
    std::vector<urma_cmd_attr_t> attrs;
    int returnValue;
    int errorNo;
    uint32_t callCount;
};

IoctlCapture g_ioctlCapture;

void ResetIoctlCapture()
{
    g_ioctlCapture = {};
    g_ioctlCapture.returnValue = 0;
}

void SetIoctlFailure(int returnValue, int errorNo)
{
    g_ioctlCapture.returnValue = returnValue;
    g_ioctlCapture.errorNo = errorNo;
}

void ExpectCapturedCommand(urma_cmd_t command, const std::vector<urma_test::ExpectedAttr> &expected)
{
    ASSERT_EQ(1U, g_ioctlCapture.callCount);
    EXPECT_EQ(urma_test::MOCK_IOCTL_FD, g_ioctlCapture.fd);
    EXPECT_EQ(static_cast<unsigned long>(URMA_CMD), g_ioctlCapture.request);
    EXPECT_EQ(static_cast<uint32_t>(command), g_ioctlCapture.hdr.command);
    EXPECT_EQ(expected.size() * sizeof(urma_cmd_attr_t), g_ioctlCapture.hdr.args_len);
    ASSERT_EQ(expected.size(), g_ioctlCapture.attrs.size());
    urma_test::ExpectAttrsEqual(g_ioctlCapture.attrs.data(), expected);
}

/* Header-only checks are used for broad wrapper coverage where full attr order is not yet part of shared mock data. */
void ExpectCapturedHeader(unsigned long request, uint32_t command)
{
    ASSERT_EQ(1U, g_ioctlCapture.callCount);
    EXPECT_EQ(urma_test::MOCK_IOCTL_FD, g_ioctlCapture.fd);
    EXPECT_EQ(request, g_ioctlCapture.request);
    EXPECT_EQ(command, g_ioctlCapture.hdr.command);
    EXPECT_GT(g_ioctlCapture.hdr.args_len, 0U);
    EXPECT_EQ(0U, g_ioctlCapture.hdr.args_len % sizeof(urma_cmd_attr_t));
    EXPECT_EQ(g_ioctlCapture.hdr.args_len / sizeof(urma_cmd_attr_t), g_ioctlCapture.attrs.size());
}

} // namespace

extern "C" int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    void *argp = nullptr;

    va_start(args, request);
    argp = va_arg(args, void *);
    va_end(args);

    g_ioctlCapture.fd = fd;
    g_ioctlCapture.request = request;
    g_ioctlCapture.callCount++;

    if (argp != nullptr) {
        const auto *hdr = static_cast<const urma_cmd_hdr_t *>(argp);
        const auto *attrs = reinterpret_cast<const urma_cmd_attr_t *>(static_cast<uintptr_t>(hdr->args_addr));
        size_t attrNum = hdr->args_len / sizeof(urma_cmd_attr_t);

        /* Copy TLV metadata before returning so later stack changes in the caller cannot affect assertions. */
        g_ioctlCapture.hdr = *hdr;
        g_ioctlCapture.attrs.assign(attrs, attrs + attrNum);
    }

    errno = g_ioctlCapture.errorNo;
    return g_ioctlCapture.returnValue;
}

TEST(UrmaCmdTlvTest, CreateCtxTlvAttrsMatchExpected)
{
    urma_cmd_create_ctx_t arg = urma_test::MakeCreateCtxCmd();
    std::vector<urma_test::ExpectedAttr> expected = urma_test::ExpectedCreateCtxAttrs(&arg);

    ResetIoctlCapture();
    EXPECT_EQ(0, urma_ioctl_create_ctx(urma_test::MOCK_IOCTL_FD, &arg));
    ExpectCapturedCommand(URMA_CMD_CREATE_CTX, expected);
}

TEST(UrmaCmdTlvTest, TokenIdTlvAttrsMatchExpected)
{
    urma_cmd_alloc_token_id_t allocArg = urma_test::MakeAllocTokenIdCmd();
    urma_cmd_free_token_id_t freeArg = urma_test::MakeFreeTokenIdCmd();
    std::vector<urma_test::ExpectedAttr> allocExpected = urma_test::ExpectedAllocTokenIdAttrs(&allocArg);
    std::vector<urma_test::ExpectedAttr> freeExpected = urma_test::ExpectedFreeTokenIdAttrs(&freeArg);

    ResetIoctlCapture();
    EXPECT_EQ(0, urma_ioctl_alloc_token_id(urma_test::MOCK_IOCTL_FD, &allocArg));
    ExpectCapturedCommand(URMA_CMD_ALLOC_TOKEN_ID, allocExpected);

    ResetIoctlCapture();
    EXPECT_EQ(0, urma_ioctl_free_token_id(urma_test::MOCK_IOCTL_FD, &freeArg));
    ExpectCapturedCommand(URMA_CMD_FREE_TOKEN_ID, freeExpected);
}

TEST(UrmaCmdTlvTest, SegmentTlvAttrsMatchExpected)
{
    urma_cmd_register_seg_t registerArg = urma_test::MakeRegisterSegCmd();
    urma_cmd_import_seg_t importArg = urma_test::MakeImportSegCmd();
    urma_cmd_unregister_seg_t unregisterArg = urma_test::MakeUnregisterSegCmd();

    ResetIoctlCapture();
    EXPECT_EQ(0, urma_ioctl_register_seg(urma_test::MOCK_IOCTL_FD, &registerArg));
    ExpectCapturedCommand(URMA_CMD_REGISTER_SEG, urma_test::ExpectedRegisterSegAttrs(&registerArg));

    ResetIoctlCapture();
    EXPECT_EQ(0, urma_ioctl_import_seg(urma_test::MOCK_IOCTL_FD, &importArg));
    ExpectCapturedCommand(URMA_CMD_IMPORT_SEG, urma_test::ExpectedImportSegAttrs(&importArg));

    ResetIoctlCapture();
    EXPECT_EQ(0, urma_ioctl_unregister_seg(urma_test::MOCK_IOCTL_FD, &unregisterArg));
    ExpectCapturedCommand(URMA_CMD_UNREGISTER_SEG, urma_test::ExpectedUnregisterSegAttrs(&unregisterArg));
}

TEST(UrmaCmdTlvTest, JfsTlvAttrsMatchExpected)
{
    urma_cmd_create_jfs_t createArg = urma_test::MakeCreateJfsCmd();
    urma_cmd_query_jfs_t queryArg = urma_test::MakeQueryJfsCmd();
    urma_cmd_delete_jfs_t deleteArg = urma_test::MakeDeleteJfsCmd();

    ResetIoctlCapture();
    EXPECT_EQ(0, urma_ioctl_create_jfs(urma_test::MOCK_IOCTL_FD, &createArg));
    ExpectCapturedCommand(URMA_CMD_CREATE_JFS, urma_test::ExpectedCreateJfsAttrs(&createArg));

    ResetIoctlCapture();
    EXPECT_EQ(0, urma_ioctl_query_jfs(urma_test::MOCK_IOCTL_FD, &queryArg));
    ExpectCapturedCommand(URMA_CMD_QUERY_JFS, urma_test::ExpectedQueryJfsAttrs(&queryArg));

    ResetIoctlCapture();
    EXPECT_EQ(0, urma_ioctl_delete_jfs(urma_test::MOCK_IOCTL_FD, &deleteArg));
    ExpectCapturedCommand(URMA_CMD_DELETE_JFS, urma_test::ExpectedDeleteJfsAttrs(&deleteArg));
}

TEST(UrmaCmdTlvTest, DeleteJfsBatchTlvAttrsMatchExpected)
{
    urma_jfs_t firstJfs = {};
    urma_jfs_t secondJfs = {};
    urma_jfs_t thirdJfs = {};
    urma_jfs_t *jfsArr[] = { &firstJfs, &secondJfs, &thirdJfs };
    urma_cmd_delete_jfs_batch_t arg = urma_test::MakeDeleteJfsBatchCmd(jfsArr, 3);

    ResetIoctlCapture();
    EXPECT_EQ(0, urma_ioctl_delete_jfs_batch(urma_test::MOCK_IOCTL_FD, &arg));
    ExpectCapturedCommand(URMA_CMD_DELETE_JFS_BATCH, urma_test::ExpectedDeleteJfsBatchAttrs(&arg));
}

TEST(UrmaCmdTlvTest, IoctlErrorIsPropagated)
{
    urma_cmd_create_ctx_t arg = urma_test::MakeCreateCtxCmd();
    urma_cmd_create_ctx_t original = arg;

    ResetIoctlCapture();
    SetIoctlFailure(-1, EINVAL);

    EXPECT_EQ(-1, urma_ioctl_create_ctx(urma_test::MOCK_IOCTL_FD, &arg));
    EXPECT_EQ(EINVAL, errno);
    EXPECT_EQ(0, std::memcmp(&original, &arg, sizeof(arg)));
    ExpectCapturedCommand(URMA_CMD_CREATE_CTX, urma_test::ExpectedCreateCtxAttrs(&arg));
}

#define EXPECT_URMA_IOCTL_WRAPPER(FUNC, CMD, TYPE)                                      \
    do {                                                                                \
        TYPE arg = {};                                                                  \
        ResetIoctlCapture();                                                            \
        EXPECT_EQ(0, FUNC(urma_test::MOCK_IOCTL_FD, &arg));                              \
        ExpectCapturedHeader(static_cast<unsigned long>(URMA_CMD), static_cast<uint32_t>(CMD)); \
    } while (0)

#define EXPECT_EVENT_IOCTL_WRAPPER(FUNC, REQUEST, CMD, TYPE)                            \
    do {                                                                                \
        TYPE arg = {};                                                                  \
        ResetIoctlCapture();                                                            \
        EXPECT_EQ(0, FUNC(urma_test::MOCK_IOCTL_FD, &arg));                              \
        ExpectCapturedHeader(static_cast<unsigned long>(REQUEST), static_cast<uint32_t>(CMD));  \
    } while (0)

TEST(UrmaCmdTlvTest, AllCoreTlvWrappersEmitHeaders)
{
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_unimport_seg, URMA_CMD_UNIMPORT_SEG, urma_cmd_unimport_seg_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_modify_jfs, URMA_CMD_MODIFY_JFS, urma_cmd_modify_jfs_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_alloc_jfs, URMA_CMD_ALLOC_JFS, urma_cmd_alloc_jfs_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_free_jfs, URMA_CMD_FREE_JFS, urma_cmd_free_jfs_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_set_jfs_opt, URMA_CMD_SET_JFS_OPT, urma_cmd_set_jfs_opt_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_get_jfs_opt, URMA_CMD_GET_JFS_OPT, urma_cmd_get_jfs_opt_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_active_jfs, URMA_CMD_ACTIVE_JFS, urma_cmd_active_jfs_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_deactive_jfs, URMA_CMD_DEACTIVE_JFS, urma_cmd_deactive_jfs_t);

    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_create_jfr, URMA_CMD_CREATE_JFR, urma_cmd_create_jfr_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_modify_jfr, URMA_CMD_MODIFY_JFR, urma_cmd_modify_jfr_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_query_jfr, URMA_CMD_QUERY_JFR, urma_cmd_query_jfr_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_delete_jfr, URMA_CMD_DELETE_JFR, urma_cmd_delete_jfr_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_delete_jfr_batch, URMA_CMD_DELETE_JFR_BATCH, urma_cmd_delete_jfr_batch_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_alloc_jfr, URMA_CMD_ALLOC_JFR, urma_cmd_alloc_jfr_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_free_jfr, URMA_CMD_FREE_JFR, urma_cmd_free_jfr_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_set_jfr_opt, URMA_CMD_SET_JFR_OPT, urma_cmd_set_jfr_opt_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_get_jfr_opt, URMA_CMD_GET_JFR_OPT, urma_cmd_get_jfr_opt_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_active_jfr, URMA_CMD_ACTIVE_JFR, urma_cmd_active_jfr_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_deactive_jfr, URMA_CMD_DEACTIVE_JFR, urma_cmd_deactive_jfr_t);

    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_create_jfc, URMA_CMD_CREATE_JFC, urma_cmd_create_jfc_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_modify_jfc, URMA_CMD_MODIFY_JFC, urma_cmd_modify_jfc_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_delete_jfc, URMA_CMD_DELETE_JFC, urma_cmd_delete_jfc_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_delete_jfc_batch, URMA_CMD_DELETE_JFC_BATCH, urma_cmd_delete_jfc_batch_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_alloc_jfc, URMA_CMD_ALLOC_JFC, urma_cmd_alloc_jfc_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_free_jfc, URMA_CMD_FREE_JFC, urma_cmd_free_jfc_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_set_jfc_opt, URMA_CMD_SET_JFC_OPT, urma_cmd_set_jfc_opt_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_get_jfc_opt, URMA_CMD_GET_JFC_OPT, urma_cmd_get_jfc_opt_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_active_jfc, URMA_CMD_ACTIVE_JFC, urma_cmd_active_jfc_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_deactive_jfc, URMA_CMD_DEACTIVE_JFC, urma_cmd_deactive_jfc_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_create_jfce, URMA_CMD_CREATE_JFCE, urma_cmd_create_jfce_t);
}

TEST(UrmaCmdTlvTest, AllJettyAndControlTlvWrappersEmitHeaders)
{
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_import_jfr, URMA_CMD_IMPORT_JFR, urma_cmd_import_jfr_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_import_jfr_ex, URMA_CMD_IMPORT_JFR_EX, urma_cmd_import_jfr_ex_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_unimport_jfr, URMA_CMD_UNIMPORT_JFR, urma_cmd_unimport_jfr_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_create_jetty, URMA_CMD_CREATE_JETTY, urma_cmd_create_jetty_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_modify_jetty, URMA_CMD_MODIFY_JETTY, urma_cmd_modify_jetty_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_query_jetty, URMA_CMD_QUERY_JETTY, urma_cmd_query_jetty_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_delete_jetty, URMA_CMD_DELETE_JETTY, urma_cmd_delete_jetty_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_delete_jetty_batch, URMA_CMD_DELETE_JETTY_BATCH,
        urma_cmd_delete_jetty_batch_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_import_jetty, URMA_CMD_IMPORT_JETTY, urma_cmd_import_jetty_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_import_jetty_ex, URMA_CMD_IMPORT_JETTY_EX,
        urma_cmd_import_jetty_ex_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_unimport_jetty, URMA_CMD_UNIMPORT_JETTY,
        urma_cmd_unimport_jetty_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_advise_jfr, URMA_CMD_ADVISE_JFR, urma_cmd_advise_jetty_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_unadvise_jfr, URMA_CMD_UNADVISE_JFR,
        urma_cmd_unadvise_jetty_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_advise_jetty, URMA_CMD_ADVISE_JETTY, urma_cmd_advise_jetty_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_unadvise_jetty, URMA_CMD_UNADVISE_JETTY,
        urma_cmd_unadvise_jetty_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_bind_jetty, URMA_CMD_BIND_JETTY, urma_cmd_bind_jetty_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_bind_jetty_ex, URMA_CMD_BIND_JETTY_EX,
        urma_cmd_bind_jetty_ex_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_unbind_jetty, URMA_CMD_UNBIND_JETTY, urma_cmd_unadvise_jetty_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_create_jetty_grp, URMA_CMD_CREATE_JETTY_GRP,
        urma_cmd_create_jetty_grp_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_delete_jetty_grp, URMA_CMD_DESTROY_JETTY_GRP,
        urma_cmd_delete_jetty_grp_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_alloc_jetty, URMA_CMD_ALLOC_JETTY, urma_cmd_alloc_jetty_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_free_jetty, URMA_CMD_FREE_JETTY, urma_cmd_free_jetty_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_set_jetty_opt, URMA_CMD_SET_JETTY_OPT, urma_cmd_set_jetty_opt_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_get_jetty_opt, URMA_CMD_GET_JETTY_OPT, urma_cmd_get_jetty_opt_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_active_jetty, URMA_CMD_ACTIVE_JETTY, urma_cmd_active_jetty_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_deactive_jetty, URMA_CMD_DEACTIVE_JETTY,
        urma_cmd_deactive_jetty_t);
}

TEST(UrmaCmdTlvTest, AllMiscAndEventTlvWrappersEmitHeaders)
{
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_user_ctl, URMA_CMD_USER_CTL, urma_cmd_user_ctl_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_get_eid_list, URMA_CMD_GET_EID_LIST, urma_cmd_get_eid_list_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_get_netaddr_list, URMA_CMD_GET_NETADDR_LIST,
        urma_cmd_get_net_addr_list_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_modify_tp, URMA_CMD_MODIFY_TP, urma_cmd_modify_tp_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_query_dev_attr, URMA_CMD_QUERY_DEV_ATTR,
        urma_cmd_query_device_attr_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_import_jetty_async, URMA_CMD_IMPORT_JETTY_ASYNC,
        urma_cmd_import_jetty_async_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_unimport_jetty_async, URMA_CMD_UNIMPORT_JETTY_ASYNC,
        urma_cmd_unimport_jetty_async_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_bind_jetty_async, URMA_CMD_BIND_JETTY_ASYNC,
        urma_cmd_bind_jetty_async_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_unbind_jetty_async, URMA_CMD_UNBIND_JETTY_ASYNC,
        urma_cmd_unbind_jetty_async_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_create_notifier, URMA_CMD_CREATE_NOTIFIER,
        urma_cmd_create_notifier_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_get_tp_list, URMA_CMD_GET_TP_LIST, urma_cmd_get_tp_list_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_set_tp_attr, URMA_CMD_SET_TP_ATTR, urma_cmd_set_tp_attr_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_get_tp_attr, URMA_CMD_GET_TP_ATTR, urma_cmd_get_tp_attr_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_exchange_tp_info, URMA_CMD_EXCHANGE_TP_INFO,
        urma_cmd_exchange_tp_info_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_get_eid_by_ip, URMA_CMD_GET_EID_BY_IP,
        urma_cmd_get_eid_by_ip_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_get_ip_by_eid, URMA_CMD_GET_IP_BY_EID,
        urma_cmd_get_ip_by_eid_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_get_smac, URMA_CMD_GET_SMAC, urma_cmd_get_smac_t);
    EXPECT_URMA_IOCTL_WRAPPER(urma_ioctl_get_dmac, URMA_CMD_GET_DMAC, urma_cmd_get_dmac_t);

    EXPECT_EVENT_IOCTL_WRAPPER(urma_ioctl_wait_jfc, URMA_CMD_WAIT_JFC, URMA_EVENT_CMD_WAIT_JFCE,
        urma_cmd_jfce_wait_t);
    EXPECT_EVENT_IOCTL_WRAPPER(urma_ioctl_get_async_event, URMA_CMD_GET_ASYNC_EVENT,
        URMA_EVENT_CMD_GET_ASYNC_EVENT, urma_cmd_async_event_t);
    EXPECT_EVENT_IOCTL_WRAPPER(urma_ioctl_wait_notify, URMA_CMD_WAIT_NOTIFY, URMA_EVENT_CMD_WAIT_NOTIFY,
        urma_cmd_wait_notify_t);
}
