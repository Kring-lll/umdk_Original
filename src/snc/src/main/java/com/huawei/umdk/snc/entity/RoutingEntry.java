/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: SNC (Supernode Network Controller) service
 * Create: 2026-07-07
 * Note:
 * History: 2026-07-07  Create File
 */
package com.huawei.umdk.snc.entity;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

import lombok.AllArgsConstructor;
import lombok.EqualsAndHashCode;
import lombok.Getter;
import lombok.NoArgsConstructor;
import lombok.Setter;
import lombok.ToString;

@Getter
@Setter
@NoArgsConstructor
@AllArgsConstructor
@EqualsAndHashCode
@ToString
public class RoutingEntry {
    private RoutePrefix prefix;
    private List<OutPortInfo> outPortInfos;

    public List<OutPortInfo> getOutPortInfos() {
        return outPortInfos == null ? null : Collections.unmodifiableList(outPortInfos);
    }

    public static RoutingEntry copy(RoutingEntry srcRoutingEntry) {
        if (srcRoutingEntry == null) {
            return null;
        }

        RoutePrefix srcPrefix = srcRoutingEntry.getPrefix();
        RoutePrefix dstPrefix = (srcPrefix == null) ? null
            : new RoutePrefix(srcPrefix.getDstAddress(), srcPrefix.getMaskLength());

        List<OutPortInfo> dstOutPortList = new ArrayList<>();
        List<OutPortInfo> srcOutPortList = srcRoutingEntry.getOutPortInfos();
        if (srcOutPortList != null) {
            for (OutPortInfo srcPort : srcOutPortList) {
                OutPortInfo dstPort = (srcPort == null) ? null : new OutPortInfo(
                    srcPort.getPortName(), srcPort.getNextHop(), srcPort.getPreference(),
                    srcPort.getTag(), srcPort.getProtocol(), srcPort.isActive());
                dstOutPortList.add(dstPort);
            }
        }

        RoutingEntry destEntry = new RoutingEntry();
        destEntry.setPrefix(dstPrefix);
        destEntry.setOutPortInfos(dstOutPortList);
        return destEntry;
    }
}
