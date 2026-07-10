/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: SNC (Super Node Controller) service
 * Author: OpenCode
 * Create: 2024-11-04
 * Note:
 * History: 2024-11-04  Create File
 */
package com.huawei.umdk.snc.engine;

import java.util.List;
import java.util.Map;

import com.huawei.umdk.snc.entity.RoutePrefix;
import com.huawei.umdk.snc.entity.RoutingEntry;
import com.huawei.umdk.snc.util.AddressUtils;

public class RouteLookupEngine {

    public RoutingEntry lookup(String targetAddr, Map<RoutePrefix, RoutingEntry> routes,
                               List<Integer> maskLengths) {
        if (routes == null || routes.isEmpty()) {
            return null;
        }

        for (int maskLen : maskLengths) {
            String networkAddr = AddressUtils.applyMask(targetAddr, maskLen);
            RoutePrefix prefix = new RoutePrefix(networkAddr, maskLen);
            RoutingEntry entry = routes.get(prefix);
            if (entry != null) {
                return entry;
            }
        }

        RoutePrefix defaultPrefix = new RoutePrefix("0.0.0.0", 0);
        if (!maskLengths.contains(0)) {
            RoutingEntry entry = routes.get(defaultPrefix);
            if (entry != null) {
                return entry;
            }
        }

        return null;
    }
}
