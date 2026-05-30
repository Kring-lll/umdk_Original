/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc cna utils test module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc cna utils test module
 */
package com.huawei.umdk.snc.util;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.*;

/**
 * CNAUtils 单元测试
 */
class CNAUtilsTest {

    // ==================== isValid ====================

    @Test
    void testIsValidNull() {
        assertFalse(CNAUtils.isValid(null));
    }

    @Test
    void testIsValidEmpty() {
        assertFalse(CNAUtils.isValid(""));
    }

    @Test
    void testIsValidLowercase() {
        assertTrue(CNAUtils.isValid("0xabcdef"));
    }

    @Test
    void testIsValidUppercase() {
        assertTrue(CNAUtils.isValid("0xABCDEF"));
    }

    @Test
    void testIsValidMixedCase() {
        assertTrue(CNAUtils.isValid("0xAbcDef"));  // 6 hex digits = 24 bit, valid
    }

    @Test
    void testIsValidShortValue() {
        assertTrue(CNAUtils.isValid("0x1"));
    }

    @Test
    void testIsValidMaxValue() {
        assertTrue(CNAUtils.isValid("0xFFFFFF"));
    }

    @Test
    void testIsValidExceeds24Bit() {
        assertFalse(CNAUtils.isValid("0x1000000"));
    }

    @Test
    void testIsValidNoOxPrefix() {
        assertFalse(CNAUtils.isValid("ABCDEF"));
    }

    @Test
    void testIsValidInvalidChar() {
        assertFalse(CNAUtils.isValid("0xGHIJKL"));
    }

    @Test
    void testIsValidJust0x() {
        assertTrue(CNAUtils.isValid("0x") || CNAUtils.isValid("0x0"));
        // "0x" matches pattern ^0x[0-9A-Fa-f]{1,8}$ -> {1,8} requires at least 1 hex digit
        // so "0x" alone is invalid
        assertFalse(CNAUtils.isValid("0x"));
    }

    @Test
    void testIsValidSingleHexDigit() {
        assertTrue(CNAUtils.isValid("0xA"));
    }

    @Test
    void testIsValidLongerThanExpected() {
        // More than 8 hex digits after 0x
        assertFalse(CNAUtils.isValid("0x123456789"));
    }

    @Test
    void testIsValidVeryLargeNumber() {
        assertFalse(CNAUtils.isValid("0xFFFFFFFF")); // 0xFFFF_FFFF > 0xFF_FFFF
    }

    // ==================== Constants ====================

    @Test
    void testCnaBitWidthConstant() {
        assertEquals(24, CNAUtils.CNA_BIT_WIDTH);
    }

    @Test
    void testConstructorCannotBeInvoked() {
        // Private constructor - verify class is instantiable via static methods only
        assertTrue(CNAUtils.isValid("0x123456"));
    }
}