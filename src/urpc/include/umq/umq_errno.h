/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: Public header file of UMQ errno
 * Create: 2025-7-16
 * Note:
 * History: 2025-7-16
 */

#ifndef UMQ_ERRNO_H
#define UMQ_ERRNO_H

#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UMQ_SUCCESS                                (0)
#define UMQ_FAIL                                   (-1)
#define UMQ_INVALID_HANDLE                         (0)

#define UMQ_ERR_EPERM                              (EPERM)
#define UMQ_ERR_EAGAIN                             (EAGAIN)
#define UMQ_ERR_ENOMEM                             (ENOMEM)
#define UMQ_ERR_EBUSY                              (EBUSY)
#define UMQ_ERR_EEXIST                             (EEXIST)
#define UMQ_ERR_EINVAL                             (EINVAL)
#define UMQ_ERR_ENODEV                             (ENODEV) 

#ifdef __cplusplus
}
#endif

#endif