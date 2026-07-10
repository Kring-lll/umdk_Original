/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: SNC (Supernode Network Controller) service
 * Author: OpenCode
 * Create: 2024-11-04
 * Note:
 * History: 2024-11-04  Create File
 */
package com.huawei.umdk.snc.dto;

import java.util.LinkedHashMap;

import lombok.AllArgsConstructor;
import lombok.EqualsAndHashCode;
import lombok.Getter;
import lombok.NoArgsConstructor;
import lombok.Setter;
import lombok.ToString;

@Getter
@Setter
@NoArgsConstructor
@AllArgsConstructor
@EqualsAndHashCode
@ToString
public class PathPlanRequest {
    private String superNodeName;
    private String srcPort;
    private String destPort;
    private String srcDevice;
    private String destDevice;
    /** Intermediate devices in topology order. Use LinkedHashMap to guarantee iteration order. */
    private LinkedHashMap<String, String> interDevices;
}
