/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc device type module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc device type module
 */
package com.huawei.umdk.snc.entity;

/**
 * 设备类型枚举
 */
public enum DeviceType {
    /** NPU设备 - 计算节点 */
    NPU,
    /** LA设备 - 框内交换 */
    LA,
    /** LC设备 - 跨框交换 */
    LC
}