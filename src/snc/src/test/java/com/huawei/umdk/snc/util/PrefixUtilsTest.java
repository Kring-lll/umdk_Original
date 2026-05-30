package com.huawei.umdk.snc.util;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.*;

/**
 * PrefixUtils 单元测试
 *
 * <p>Prefix 格式：32-bit hex，0x 前缀 + 8 位十六进制数字。
 */
class PrefixUtilsTest {

    // ==================== isValid ====================

    @Test
    void testIsValidNull() {
        assertFalse(PrefixUtils.isValid(null));
    }

    @Test
    void testIsValidEmpty() {
        assertFalse(PrefixUtils.isValid(""));
    }

    @Test
    void testIsValidLowercase() {
        assertTrue(PrefixUtils.isValid("0x00abcdef"));
    }

    @Test
    void testIsValidUppercase() {
        assertTrue(PrefixUtils.isValid("0x00ABCDEF"));
    }

    @Test
    void testIsValidMixedCase() {
        assertTrue(PrefixUtils.isValid("0x00AbcDef"));
    }

    @Test
    void testIsValidMinValue() {
        assertTrue(PrefixUtils.isValid("0x00000000"));
    }

    @Test
    void testIsValidMaxValue() {
        assertTrue(PrefixUtils.isValid("0xFFFFFFFF"));
    }

    @Test
    void testIsValidNo0xPrefix() {
        assertFalse(PrefixUtils.isValid("00ABCDEF"));
    }

    @Test
    void testIsValidInvalidChar() {
        assertFalse(PrefixUtils.isValid("0x00GHIJKL"));
    }

    @Test
    void testIsValidTooFewDigits() {
        // 7 hex digits after 0x instead of 8
        assertFalse(PrefixUtils.isValid("0xABCDEFG"));
        assertFalse(PrefixUtils.isValid("0x1234567"));
    }

    @Test
    void testIsValidTooManyDigits() {
        // 9 hex digits after 0x instead of 8
        assertFalse(PrefixUtils.isValid("0x123456789"));
    }

    @Test
    void testIsValidJust0x() {
        assertFalse(PrefixUtils.isValid("0x"));
    }

    @Test
    void testIsValidLeadingSpace() {
        assertFalse(PrefixUtils.isValid(" 0x12345678"));
    }

    @Test
    void testIsValidTrailingSpace() {
        assertFalse(PrefixUtils.isValid("0x12345678 "));
    }

    // ==================== Constants ====================

    @Test
    void testPrefixBitWidthConstant() {
        assertEquals(32, PrefixUtils.PREFIX_BIT_WIDTH);
    }

    // ==================== Utility class ====================

    @Test
    void testUtilityClassViaStaticMethod() {
        assertTrue(PrefixUtils.isValid("0xAABBCCDD"));
    }
}