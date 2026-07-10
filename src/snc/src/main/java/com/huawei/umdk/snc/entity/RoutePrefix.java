/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: SNC (Supernode Network Controller) service
 * Author: OpenCode
 * Create: 2024-11-04
 * Note:
 * History: 2024-11-04  Create File
 */
package com.huawei.umdk.snc.entity;

import lombok.AllArgsConstructor;
import lombok.EqualsAndHashCode;
import lombok.Getter;
import lombok.ToString;

@Getter
@AllArgsConstructor
@EqualsAndHashCode
@ToString
public class RoutePrefix {
    private final String dstAddress;
    private final Integer maskLength;
}
