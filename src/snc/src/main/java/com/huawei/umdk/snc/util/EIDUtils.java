/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc snc eidutils module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc snc eidutils module
 */
package com.huawei.umdk.snc.util;

/**
 * EID（128 bit）地址工具类
 *
 * EID格式：32位十六进制字符串（128 bit），用于唯一标识网络端点
 */
public final class EIDUtils {

    /** EID 位宽 */
    public static final int EID_BIT_WIDTH = 128;
    /** EID 十六进制字符长度 */
    public static final int EID_HEX_LENGTH = 32;
    /** EID 正则模式 */
    private static final String EID_PATTERN = "^[0-9A-Fa-f]{32}$";

    private EIDUtils() {
    }

    /**
     * 校验 EID 格式是否合法
     */
    public static boolean isValid(String eid) {
        if (eid == null || eid.length() != EID_HEX_LENGTH) {
            return false;
        }
        return eid.matches(EID_PATTERN);
    }
}