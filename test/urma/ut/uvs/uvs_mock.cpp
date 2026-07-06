/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: UVS unit test mocks.
 */

#include "uvs_fixture.h"

namespace urma_test_uvs {
UvsIoctlCapture g_uvsIoctl;
} // namespace urma_test_uvs

using namespace urma_test_uvs;

struct nl_sock;
struct nl_msg;

extern "C" nl_sock *nl_socket_alloc(void)
{
    return reinterpret_cast<nl_sock *>(0x101);
}

extern "C" void nl_socket_free(nl_sock *)
{
}

extern "C" void nl_close(nl_sock *)
{
}

extern "C" int genl_connect(nl_sock *)
{
    if (!g_uvsIoctl.netlinkSucceed) {
        return g_uvsIoctl.netlinkRet;
    }
    return 0;
}

extern "C" int genl_ctrl_resolve(nl_sock *, const char *name)
{
    if (!g_uvsIoctl.netlinkSucceed) {
        return g_uvsIoctl.netlinkRet;
    }
    if (name == nullptr || std::strcmp(name, "UBAGG_GENL") != 0) {
        return -EINVAL;
    }
    return 88;
}

extern "C" nl_msg *nlmsg_alloc(void)
{
    return reinterpret_cast<nl_msg *>(0x102);
}

extern "C" void nlmsg_free(nl_msg *)
{
}

extern "C" void *genlmsg_put(nl_msg *msg, uint32_t, uint32_t, int, int, int,
    uint8_t, uint8_t)
{
    return msg;
}

extern "C" int nla_put_u64(nl_msg *, int attrtype, uint64_t value)
{
    constexpr int bondpNlHdrArgsAddr = 4;

    if (!g_uvsIoctl.netlinkSucceed) {
        return g_uvsIoctl.netlinkRet;
    }
    if (attrtype != bondpNlHdrArgsAddr || value == 0) {
        return -EINVAL;
    }

    g_uvsIoctl.lastNetlinkArg =
        reinterpret_cast<UvsNetlinkTopoArg *>(static_cast<uintptr_t>(value));
    return 0;
}

extern "C" int nl_send_auto(nl_sock *, nl_msg *)
{
    if (!g_uvsIoctl.netlinkSucceed) {
        return g_uvsIoctl.netlinkRet;
    }
    return 1;
}

extern "C" int nl_recvmsgs_default(nl_sock *)
{
    UvsNetlinkTopoArg *arg = g_uvsIoctl.lastNetlinkArg;
    uint32_t nodeNum;
    uint32_t nodeIdx;

    if (!g_uvsIoctl.netlinkSucceed) {
        return g_uvsIoctl.netlinkRet;
    }
    if (arg == nullptr || arg->in.node_idx < 0) {
        return -EINVAL;
    }

    nodeIdx = static_cast<uint32_t>(arg->in.node_idx);
    nodeNum = g_uvsIoctl.ubaggTopoNum;
    if (g_uvsIoctl.changeTopoNumAfterFirst && g_uvsIoctl.netlinkCallCount > 0) {
        nodeNum++;
    }
    if (nodeNum != 0 && nodeIdx >= nodeNum) {
        return -EINVAL;
    }

    g_uvsIoctl.netlinkCallCount++;
    arg->out.node_num = nodeNum;
    if (nodeNum == 0 || nodeNum > MAX_NODE_NUM) {
        return 0;
    }
    if (nodeIdx < g_uvsIoctl.topoNodes.size()) {
        arg->out.topo_info = g_uvsIoctl.topoNodes[nodeIdx];
    } else {
        arg->out.topo_info = g_uvsIoctl.topoNode;
    }
    return 0;
}

extern "C" int ioctl(int, unsigned long request, ...)
{
    va_list ap;
    void *arg = nullptr;

    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);

    g_uvsIoctl.callCount++;
    if (!g_uvsIoctl.succeed || arg == nullptr) {
        errno = g_uvsIoctl.errnoValue;
        return -1;
    }

    if (request == UVS_UBAGG_CMD) {
        return HandleUbaggIoctl(static_cast<uvs_ubagg_cmd_hdr *>(arg));
    }
    if (request != TPSA_CMD) {
        errno = EINVAL;
        return -1;
    }

    auto *hdr = static_cast<tpsa_cmd_hdr_t *>(arg);
    if (hdr->args_len != 0 && hdr->args_addr == 0) {
        errno = EINVAL;
        return -1;
    }
    CaptureTlvAttrs(hdr);
    return 0;
}

extern "C" int open(const char *path, int flags, ...)
{
    mode_t mode = 0;

    if ((flags & O_CREAT) != 0) {
        va_list ap;

        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    if (std::strcmp(path, "/dev/ubagg") == 0 || std::strcmp(path, "/dev/ubcore/ubcore") == 0) {
        if (g_uvsIoctl.mockDeviceOpen) {
            return 77;
        }
        errno = ENOENT;
        return -1;
    }

    return static_cast<int>(syscall(SYS_openat, AT_FDCWD, path, flags, mode));
}

extern "C" int close(int fd)
{
    if (fd == 77) {
        return 0;
    }
    return static_cast<int>(syscall(SYS_close, fd));
}
