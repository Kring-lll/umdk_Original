package com.huawei.umdk.snc.engine;

import com.huawei.umdk.snc.entity.*;

import java.util.*;

/**
 * 路由查找引擎
 *
 * 职责：
 * - 在设备的路由表中查找最长前缀匹配的下一跳
 * - 支持基于 RouteEntry.prefix 的 LPM 查找
 * - 查找 key 支持 CNA 路由前缀（32 bit）、EID（128 bit）等任意 hex 字符串
 */
public class RouteLookupEngine {

    /**
     * 查找设备路由表中匹配目标前缀的最佳路由条目
     *
     * @param routerTable 设备路由表
     * @param dstPrefix   目标前缀（如 CNA 变换后的 32 位 hex 字符串 "AAAAAA11"）
     * @return 最长前缀匹配的 RouteEntry，无匹配返回 null
     */
    public RouteEntry lookup(RouterTable routerTable, String dstPrefix) {
        if (routerTable == null || dstPrefix == null) {
            return null;
        }

        String normalizedDst = normalizePrefix(dstPrefix);

        Map<String, RouteEntry> routes = routerTable.getRoutes();
        if (routes == null || routes.isEmpty()) {
            return null;
        }

        RouteEntry best = null;
        int bestLen = -1;
        for (RouteEntry entry : routes.values()) {
            if (entry == null || entry.getPrefix() == null) {
                continue;
            }
            String prefix = normalizePrefix(entry.getPrefix());
            if (normalizedDst.startsWith(prefix) && prefix.length() > bestLen) {
                best = entry;
                bestLen = prefix.length();
            }
        }
        return best;
    }

    /**
     * 标准化前缀：去掉 0x/0X 前缀并转大写
     */
    private String normalizePrefix(String prefix) {
        String s = prefix.trim();
        if (s.startsWith("0x") || s.startsWith("0X")) {
            s = s.substring(2);
        }
        return s.toUpperCase();
    }

    /**
     * 查找默认路由（前缀为空或长度为 0 的条目）
     */
    public RouteEntry lookupDefault(RouterTable routerTable) {
        if (routerTable == null) {
            return null;
        }
        Map<String, RouteEntry> routes = routerTable.getRoutes();
        if (routes == null || routes.isEmpty()) {
            return null;
        }
        for (RouteEntry entry : routes.values()) {
            if (entry == null) continue;
            String p = entry.getPrefix();
            if (p == null || p.isEmpty()) {
                return entry;
            }
        }
        return null;
    }
}
