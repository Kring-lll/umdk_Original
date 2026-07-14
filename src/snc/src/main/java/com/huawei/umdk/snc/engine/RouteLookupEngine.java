/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: SNC (Supernode Network Controller) service
 * Author: guoshuangjian
 * Create: 2026-07-07
 * Note:
 * History: 2026-07-07  Create File
 */
package com.huawei.umdk.snc.engine;

import java.util.List;
import java.util.Map;

import com.huawei.umdk.snc.config.Logger;
import com.huawei.umdk.snc.entity.RoutePrefix;
import com.huawei.umdk.snc.entity.RoutingEntry;
import com.huawei.umdk.snc.util.AddressUtils;

public class RouteLookupEngine {

    private static final Logger LOG = new Logger(RouteLookupEngine.class);

    public RoutingEntry lookup(String targetAddr, Map<RoutePrefix, RoutingEntry> routes,
                               List<Integer> maskLengths) {
        if (routes == null || routes.isEmpty()) {
            LOG.warn("lookup called with null or empty routes for target: %s", targetAddr);
            return null;
        }
        if (maskLengths == null) {
            LOG.warn("lookup called with null maskLengths for target: %s", targetAddr);
            return null;
        }

        for (int maskLen : maskLengths) {
            String networkAddr = AddressUtils.applyMask(targetAddr, maskLen);
            RoutePrefix prefix = new RoutePrefix(networkAddr, maskLen);
                RoutingEntry entry = routes.get(prefix);
                if (entry != null) {
                    LOG.debug("route found for target %s with mask %d", targetAddr, maskLen);
                    return entry;
                }
        }

        RoutePrefix defaultPrefix = new RoutePrefix("0.0.0.0", 0);
        if (!maskLengths.contains(0)) {
            RoutingEntry entry = routes.get(defaultPrefix);
            if (entry != null) {
                LOG.debug("default route found for target %s", targetAddr);
                return entry;
            }
        }

        LOG.warn("no route found for target %s", targetAddr);
        return null;
    }
}
