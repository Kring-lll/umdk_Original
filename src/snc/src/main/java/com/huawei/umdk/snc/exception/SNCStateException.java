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

public class SNCStateException extends SNCException {

    private static final long serialVersionUID = 1L;

    public SNCStateException() {
        super();
    }

    public SNCStateException(String message) {
        super(message);
    }

    public SNCStateException(String message, Throwable cause) {
        super(message, cause);
    }
}
