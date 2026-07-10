/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: SNC (Supernode Network Controller) service
 * Author: OpenCode
 * Create: 2024-11-04
 * Note:
 * History: 2024-11-04  Create File
 */
package com.huawei.umdk.snc.exception;

public class AclNotFoundException extends SNCException {

    private static final long serialVersionUID = 1L;

    public AclNotFoundException() {
        super();
    }

    public AclNotFoundException(String message) {
        super(message);
    }

    public AclNotFoundException(String message, Throwable cause) {
        super(message, cause);
    }
}
