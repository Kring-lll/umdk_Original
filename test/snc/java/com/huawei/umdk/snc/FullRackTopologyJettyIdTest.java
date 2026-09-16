/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: full rack topology jetty id allocation test
 * Create: 2026-09-13
 * Note:
 * History: 2026-09-13  Create File
 */
package com.huawei.umdk.snc;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

import com.huawei.umdk.snc.entity.DeviceEntity;
import com.huawei.umdk.snc.entity.DeviceType;
import com.huawei.umdk.snc.entity.ForwardingChip;
import com.huawei.umdk.snc.entity.NpuPortEntity;
import com.huawei.umdk.snc.entity.PortEntity;
import com.huawei.umdk.snc.entity.SuperNode;
import com.huawei.umdk.snc.util.HashUtils;

import org.junit.jupiter.api.DisplayName;
import org.junit.jupiter.api.Test;

/**
 * Pins the fixed jetty id allocation of {@link FullRackTopologyGenerator}: the
 * jetty id of NPU port index {@code p} is {@code 32 + p}, identical on every
 * generator invocation, unique per NPU and inside {@code [32, 1023]}.
 */
@DisplayName("FullRackTopologyGenerator：jettyId 固定分配（32 + 端口序号）")
class FullRackTopologyJettyIdTest {

    /** (npuDevice#portName) -> jettyId of the whole topology. */
    private static Map<String, Integer> jettyMap(SuperNode sn) {
        Map<String, Integer> map = new HashMap<>();
        for (DeviceEntity dev : sn.getAllDevices().values()) {
            if (dev.getDeviceType() != DeviceType.NPU || dev.getForwardingChips() == null) {
                continue;
            }
            for (ForwardingChip chip : dev.getForwardingChips().values()) {
                if (chip.getPorts() == null) continue;
                for (PortEntity p : chip.getPorts().values()) {
                    assertTrue(p instanceof NpuPortEntity);
                    Integer jettyId = ((NpuPortEntity) p).getJettyId();
                    assertNotNull(jettyId, "every NPU physical port needs a jettyId: "
                        + dev.getDeviceName() + ":" + p.getPortName());
                    map.put(dev.getDeviceName() + "#" + p.getPortName(), jettyId);
                }
            }
        }
        return map;
    }

    @Test
    @DisplayName("每个 NPU 物理端口都有 jettyId，且在 [32,1023] 内、NPU 内唯一")
    void rangeAndUniqueness() {
        SuperNode sn = FullRackTopologyGenerator.buildRawTopology();
        Map<String, Set<Integer>> perNpu = new HashMap<>();
        int portCount = 0;

        for (DeviceEntity dev : sn.getAllDevices().values()) {
            if (dev.getDeviceType() != DeviceType.NPU || dev.getForwardingChips() == null) {
                continue;
            }
            Set<Integer> jettyIds = new HashSet<>();
            for (ForwardingChip chip : dev.getForwardingChips().values()) {
                if (chip.getPorts() == null) continue;
                for (PortEntity p : chip.getPorts().values()) {
                    portCount++;
                    Integer jettyId = ((NpuPortEntity) p).getJettyId();
                    assertNotNull(jettyId);
                    assertTrue(HashUtils.isValidJettyId(jettyId),
                        "jettyId out of [32,1023]: " + jettyId);
                    assertTrue(jettyIds.add(jettyId),
                        "jettyId must be unique within " + dev.getDeviceName()
                            + ", duplicate " + jettyId);
                }
            }
            perNpu.put(dev.getDeviceName(), jettyIds);
        }

        // 128 NPU x 8 ports, each NPU owning exactly the 8 fixed jetty ids 32..39
        assertEquals(128 * 8, portCount, "full rack has 1024 NPU physical ports");
        assertEquals(128, perNpu.size(), "full rack has 128 NPUs");
        for (Map.Entry<String, Set<Integer>> e : perNpu.entrySet()) {
            assertEquals(8, e.getValue().size(), e.getKey() + " must own 8 jetty ids");
        }
    }

    @Test
    @DisplayName("固定映射：端口序号 p -> 32 + p（含已知端口的精确断言）")
    void fixedMapping() {
        SuperNode sn = FullRackTopologyGenerator.buildRawTopology();
        DeviceEntity npu = sn.getAllDevices().get("rack1#board1#npu1");
        assertNotNull(npu);

        // portName = "400GUB {b}/{2n}/{p+1}" -> 400GUB 1/2/1 .. 400GUB 1/2/8
        for (int p = 0; p < 8; p++) {
            String portName = "400GUB 1/2/" + (p + 1);
            PortEntity port = npu.getForwardingChips().get(2).getPorts().get(portName);
            assertNotNull(port, "port must exist: " + portName);
            assertEquals(FullRackTopologyGenerator.JETTY_ID_BASE + p,
                ((NpuPortEntity) port).getJettyId(),
                "jetty id must be fixed as 32 + port index, port " + portName);
        }
        assertEquals(32, FullRackTopologyGenerator.JETTY_ID_BASE);

        // spot check another NPU in another rack
        DeviceEntity npu2 = sn.getAllDevices().get("rack4#board8#npu4");
        assertNotNull(npu2);
        PortEntity last = npu2.getForwardingChips().get(2).getPorts().get("400GUB 8/8/8");
        assertNotNull(last);
        assertEquals(39, ((NpuPortEntity) last).getJettyId());
    }

    @Test
    @DisplayName("跨两次生成结果完全一致（每次进入都不变）")
    void stableAcrossInvocations() {
        Map<String, Integer> first = jettyMap(FullRackTopologyGenerator.buildRawTopology());
        Map<String, Integer> second = jettyMap(FullRackTopologyGenerator.buildRawTopology());
        assertEquals(first, second,
            "jetty id allocation must be identical on every generator invocation");
    }
}
