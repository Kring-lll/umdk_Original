package com.huawei.umdk.snc.util;

/**
 * Prefix（32 bit）地址工具类。
 *
 * <p>Prefix 格式：{@code 0x} 前缀 + 1~8 位十六进制数字，值范围 0x0 ~ 0xFFFFFFFF。
 *
 * @author SNC Team
 * @since 1.0
 */
public final class PrefixUtils {

    /** Prefix 位宽 */
    public static final int PREFIX_BIT_WIDTH = 32;

    /** Prefix 正则模式：0x开头、8位十六进制数字 */
    private static final String PREFIX_PATTERN = "^0x[0-9A-Fa-f]{8}$";

    private PrefixUtils() {
        throw new UnsupportedOperationException("Utility class");
    }

    /**
     * 校验 Prefix 格式是否合法。
     *
     * <p>合法条件：
     * <ul>
     *   <li>非 null、非空</li>
     *   <li>匹配 {@code ^0x[0-9A-Fa-f]{1,8}$}</li>
     *   <li>十六进制值 ≤ 0xFFFFFFFF（32 bit）</li>
     * </ul>
     *
     * @param prefix 待校验的 Prefix 字符串
     * @return true 表示合法；false 表示非法
     */
    public static boolean isValid(final String prefix) {
        if (prefix == null || prefix.isEmpty()) {
            return false;
        }
        if (!prefix.matches(PREFIX_PATTERN)) {
            return false;
        }
        try {
            long value = Long.parseLong(prefix.substring(2), 16);
            return value <= 0xFFFFFFFFL;
        } catch (NumberFormatException e) {
            return false;
        }
    }
}