package com.huawei.umdk.snc.util;

/**
 * CNA（Campus Network Address，24 bit）工具类。
 *
 * <p>CNA 格式：0x 前缀 + 1~6 位十六进制数字，值范围 0x0 ~ 0xFFFFFF
 *
 * @author SNC Team
 * @since 1.0
 */
public final class CNAUtils {

    /** CNA 位宽 */
    public static final int CNA_BIT_WIDTH = 24;

    /** CNA 正则模式：0x开头、1~8位十六进制数字 */
    private static final String CNA_PATTERN = "^0x[0-9A-Fa-f]{1,8}$";

    private CNAUtils() {
        throw new UnsupportedOperationException("Utility class");
    }

    /**
     * 校验 CNA 格式是否合法。
     *
     * <p>CNA 合法条件：
     * <ul>
     *   <li>非 null、非空</li>
     *   <li>匹配 {@code ^0x[0-9A-Fa-f]{1,8}$}</li>
     *   <li>十六进制值 ≤ 0xFFFFFF（24 bit）</li>
     * </ul>
     *
     * @param cna 待校验的 CNA 字符串
     * @return true 表示合法；false 表示非法
     */
    public static boolean isValid(final String cna) {
        if (cna == null || cna.isEmpty()) {
            return false;
        }
        if (!cna.matches(CNA_PATTERN)) {
            return false;
        }
        try {
            long value = Long.parseLong(cna.substring(2), 16);
            return value <= 0xFFFFFFL;
        } catch (NumberFormatException e) {
            return false;
        }
    }

    /**
     * 将 24 位 CNA 转换为 32 位路由表前缀
     *
     * 转换规则（设计文档 §10.5）：
     * 24位 CNA 格式为 3 字节 [B0][B1][B2]，需补齐为 4 字节 32 位前缀：
     * 在第 3 字节位置插入 B0，即 [B0][B1][B0][B2]
     *
     * 示例：
     *   CNA 0xAAAA11 (bytes: 0xAA, 0xAA, 0x11) → 前缀 AAAAAA11
     *   CNA 0xDDDD44 (bytes: 0xDD, 0xDD, 0x44) → 前缀 DDDDDD44
     *
     * @param cna 24 位 CNA 字符串（格式如 "0xAAAA1111"）
     * @return 8 位十六进制路由前缀字符串（如 "AAAAAA11"），null 表示无效输入
     */
    public static String cnaToRoutePrefix(String cna) {
        if (cna == null || cna.isEmpty()) {
            return null;
        }
        try {
            // 去掉 0x 前缀，解析为 long
            String hex = cna.startsWith("0x") || cna.startsWith("0X")
                    ? cna.substring(2) : cna;
            long value = Long.parseLong(hex, 16);

            // 提取 3 个字节 [B0][B1][B2]（24 位 CNA）
            int b0 = (int) ((value >> 16) & 0xFF);
            int b1 = (int) ((value >> 8) & 0xFF);
            int b2 = (int) (value & 0xFF);

            // 拼成 32 位前缀 [B0][B1][B0][B2]
            int prefix = (b0 << 24) | (b1 << 16) | (b0 << 8) | b2;

            return String.format("%08X", prefix);
        } catch (NumberFormatException e) {
            return null;
        }
    }
}
