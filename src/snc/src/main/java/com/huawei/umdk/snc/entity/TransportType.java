/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc transport type module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc transport type module
 */
package com.huawei.umdk.snc.entity;

/**
 * 传输类型枚举
 */
public enum TransportType {
    /** 可靠传输协议 */
    RTP,
    /** 不可靠传输协议 */
    UTP,
    /** 面向连接传输协议 */
    CTP
}