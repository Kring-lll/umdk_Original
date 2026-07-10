/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: SNC (Super Node Controller) service
 * Author: OpenCode
 * Create: 2024-11-04
 * Note:
 * History: 2024-11-04  Create File
 */
package com.huawei.umdk.snc.exception;

public class SNCException extends RuntimeException {

    public SNCException() {
        super();
    }

    public SNCException(String message) {
        super(message);
    }

    public SNCException(String message, Throwable cause) {
        super(message, cause);
    }
}
