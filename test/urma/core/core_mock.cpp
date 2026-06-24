/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: URMA core unit-test syscall mocks.
 */

#include "core_fixture.h"

namespace urma_core_test {

namespace {
int g_coreIoctlReturn = -1;
int g_coreIoctlErrno = ENOTTY;
} // namespace

void SetCoreIoctlResult(int returnValue, int errorNo)
{
    g_coreIoctlReturn = returnValue;
    g_coreIoctlErrno = errorNo;
}

} // namespace urma_core_test

extern "C" int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list args;

    va_start(args, request);
    (void)va_arg(args, void *);
    va_end(args);

    (void)fd;
    (void)request;
    errno = urma_core_test::g_coreIoctlErrno;
    return urma_core_test::g_coreIoctlReturn;
}
