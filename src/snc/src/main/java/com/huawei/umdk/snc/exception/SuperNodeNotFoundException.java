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

public class SuperNodeNotFoundException extends SNCException {

    public SuperNodeNotFoundException() {
        super();
    }

    public SuperNodeNotFoundException(String message) {
        super(message);
    }

    public SuperNodeNotFoundException(String message, Throwable cause) {
        super(message, cause);
    }
}
