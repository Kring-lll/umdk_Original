package com.huawei.umdk.snc.util;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.*;

/**
 * EIDUtils 单元测试
 */
class EIDUtilsTest {

    // ==================== isValid ====================

    @Test
    void testIsValidNull() {
        assertFalse(EIDUtils.isValid(null));
    }

    @Test
    void testIsValidEmpty() {
        assertFalse(EIDUtils.isValid(""));
    }

    @Test
    void testIsValidLowercase() {
        assertTrue(EIDUtils.isValid("abcdef0123456789abcdef0123456789"));
    }

    @Test
    void testIsValidUppercase() {
        assertTrue(EIDUtils.isValid("ABCDEF0123456789ABCDEF0123456789"));
    }

    @Test
    void testIsValidMixedCase() {
        assertTrue(EIDUtils.isValid("AbCdEf0123456789AbCdEf0123456789"));
    }

    @Test
    void testIsValidAllZeros() {
        assertTrue(EIDUtils.isValid("00000000000000000000000000000000"));
    }

    @Test
    void testIsValidAllFs() {
        assertTrue(EIDUtils.isValid("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"));
    }

    @Test
    void testIsValidWrongLengthShort() {
        assertFalse(EIDUtils.isValid("abcdef0123456789abcdef012345678"));
    }

    @Test
    void testIsValidWrongLengthLong() {
        assertFalse(EIDUtils.isValid("abcdef0123456789abcdef0123456789a"));
    }

    @Test
    void testIsValidInvalidChar() {
        assertFalse(EIDUtils.isValid("GHIJKL0123456789ABCDEF0123456789"));
    }

    @Test
    void testIsValidInvalidCharG() {
        String eid = "0000000000000000000000000000000g";
        assertFalse(EIDUtils.isValid(eid));
    }

    @Test
    void testIsValidNonHexChars() {
        assertFalse(EIDUtils.isValid("ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ"));
    }

    @Test
    void testIsValidShortStrings() {
        assertFalse(EIDUtils.isValid("EID1"));
        assertFalse(EIDUtils.isValid("ABCDEF1234"));
    }

    // ==================== Constants ====================

    @Test
    void testEidBitWidthConstant() {
        assertEquals(128, EIDUtils.EID_BIT_WIDTH);
    }

    @Test
    void testEidHexLengthConstant() {
        assertEquals(32, EIDUtils.EID_HEX_LENGTH);
    }
}