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

import java.util.Collections;
import java.util.Map;
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
public class SuperNode {
    private String name;
    private String version;
    private Map<String, DeviceEntity> devices;

    public Map<String, DeviceEntity> getDevices() {
        return devices == null ? null : Collections.unmodifiableMap(devices);
    }

    public Map<String, DeviceEntity> getMutableDevices() {
        return devices;
    }
}
