/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc entity basic test module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc entity basic test module
 */
package com.huawei.umdk.snc.entity;

import org.junit.jupiter.api.DisplayName;
import org.junit.jupiter.api.Nested;
import org.junit.jupiter.api.Test;

import java.util.*;

import static org.junit.jupiter.api.Assertions.*;

/**
 * Entity 基础单元测试 - 覆盖所有实体类的 getter/setter/constructor/toString。
 */
@DisplayName("Entity 基础测试")
class EntityBasicTest {

    // ==================== DeviceType ====================
    @Nested
    @DisplayName("DeviceType 枚举测试")
    class DeviceTypeTest {
        @Test
        @DisplayName("枚举值")
        void testEnumValues() {
            assertEquals(3, DeviceType.values().length);
            assertEquals(DeviceType.NPU, DeviceType.valueOf("NPU"));
            assertEquals(DeviceType.LA, DeviceType.valueOf("LA"));
            assertEquals(DeviceType.LC, DeviceType.valueOf("LC"));
        }
    }

    // ==================== TransportType ====================
    @Nested
    @DisplayName("TransportType 枚举测试")
    class TransportTypeTest {
        @Test
        @DisplayName("枚举值")
        void testEnumValues() {
            assertEquals(3, TransportType.values().length);
            assertEquals(TransportType.RTP, TransportType.valueOf("RTP"));
            assertEquals(TransportType.UTP, TransportType.valueOf("UTP"));
            assertEquals(TransportType.CTP, TransportType.valueOf("CTP"));
        }
    }

    // ==================== OutPortInfo ====================
    @Nested
    @DisplayName("OutPortInfo 测试")
    class OutPortInfoTest {
        @Test
        @DisplayName("无参构造 + getter/setter")
        void testNoArgConstructorAndSetters() {
            OutPortInfo o = new OutPortInfo();
            assertNull(o.getOutInterface());
            assertNull(o.getMaskLength());
            assertNull(o.getNextHop());
            assertNull(o.getPreference());
            assertNull(o.getTag());
            assertNull(o.getProtocol());

            o.setOutInterface("GE0/0/1");
            o.setMaskLength(24);
            o.setNextHop("10.0.0.1");
            o.setPreference(60);
            o.setTag(100);
            o.setProtocol("BGP");

            assertEquals("GE0/0/1", o.getOutInterface());
            assertEquals(Integer.valueOf(24), o.getMaskLength());
            assertEquals("10.0.0.1", o.getNextHop());
            assertEquals(Integer.valueOf(60), o.getPreference());
            assertEquals(Integer.valueOf(100), o.getTag());
            assertEquals("BGP", o.getProtocol());
        }

        @Test
        @DisplayName("有参构造")
        void testParamConstructor() {
            OutPortInfo o = new OutPortInfo("GE0/0/2", 16, "192.168.1.1", 100, 200, "OSPF");
            assertEquals("GE0/0/2", o.getOutInterface());
            assertEquals(Integer.valueOf(16), o.getMaskLength());
            assertEquals("192.168.1.1", o.getNextHop());
            assertEquals(Integer.valueOf(100), o.getPreference());
            assertEquals(Integer.valueOf(200), o.getTag());
            assertEquals("OSPF", o.getProtocol());
        }

        @Test
        @DisplayName("toString")
        void testToString() {
            OutPortInfo o = new OutPortInfo("GE0", 24, "1.1.1.1", 60, 0, "STATIC");
            String s = o.toString();
            assertTrue(s.contains("GE0"));
            assertTrue(s.contains("STATIC"));
        }
    }

    // ==================== RouteEntry ====================
    @Nested
    @DisplayName("RouteEntry 测试")
    class RouteEntryTest {
        @Test
        @DisplayName("无参构造 - 初始化空列表")
        void testNoArgConstructor() {
            RouteEntry r = new RouteEntry();
            assertNull(r.getPrefix());
            assertNotNull(r.getOutPortInfos());
            assertTrue(r.getOutPortInfos().isEmpty());
        }

        @Test
        @DisplayName("有参构造")
        void testParamConstructor() {
            List<OutPortInfo> ports = new ArrayList<>();
            ports.add(new OutPortInfo("GE0", 24, "1.1.1.1", 60, 0, "STATIC"));
            RouteEntry r = new RouteEntry("0xDDDDDD44", ports);
            assertEquals("0xDDDDDD44", r.getPrefix());
            assertSame(ports, r.getOutPortInfos());
        }

        @Test
        @DisplayName("setter/getter")
        void testSetterGetter() {
            RouteEntry r = new RouteEntry();
            r.setPrefix("0xAAAAAA11");
            assertEquals("0xAAAAAA11", r.getPrefix());
            List<OutPortInfo> list = Arrays.asList(new OutPortInfo());
            r.setOutPortInfos(list);
            assertSame(list, r.getOutPortInfos());
        }

        @Test
        @DisplayName("toString")
        void testToString() {
            RouteEntry r = new RouteEntry("0xPREFIX", Collections.emptyList());
            String s = r.toString();
            assertTrue(s.contains("0xPREFIX"));
        }
    }

    // ==================== RouterTable ====================
    @Nested
    @DisplayName("RouterTable 测试")
    class RouterTableTest {
        @Test
        @DisplayName("无参构造 + getter/setter")
        void testNoArgConstructorAndSetters() {
            RouterTable rt = new RouterTable();
            assertNull(rt.getInstance());
            assertNull(rt.getRoutes());
            rt.setInstance(0);
            Map<String, RouteEntry> routes = new HashMap<>();
            routes.put("0xPREFIX", new RouteEntry("0xPREFIX", new ArrayList<>()));
            rt.setRoutes(routes);
            assertEquals(Integer.valueOf(0), rt.getInstance());
            assertSame(routes, rt.getRoutes());
        }

        @Test
        @DisplayName("findRoute - 找到")
        void testFindRouteFound() {
            RouterTable rt = new RouterTable();
            Map<String, RouteEntry> routes = new HashMap<>();
            RouteEntry entry = new RouteEntry("0xKEY", new ArrayList<>());
            routes.put("0xKEY", entry);
            rt.setRoutes(routes);

            RouteEntry found = rt.findRoute("0xKEY");
            assertNotNull(found);
            assertEquals(entry, found);
        }

        @Test
        @DisplayName("findRoute - 未找到")
        void testFindRouteNotFound() {
            RouterTable rt = new RouterTable();
            Map<String, RouteEntry> routes = new HashMap<>();
            rt.setRoutes(routes);
            assertNull(rt.findRoute("NONEXIST"));
        }

        @Test
        @DisplayName("findRoute - routes 为 null")
        void testFindRouteNullMap() {
            RouterTable rt = new RouterTable();
            assertNull(rt.findRoute("ANY"));
        }

        @Test
        @DisplayName("toString - routes 为 null")
        void testToStringNullRoutes() {
            RouterTable rt = new RouterTable();
            assertNotNull(rt.toString());
        }
    }

    // ==================== PortEntity ====================
    @Nested
    @DisplayName("PortEntity 测试")
    class PortEntityTest {
        @Test
        @DisplayName("无参构造 + getter/setter")
        void testNoArgConstructorAndSetters() {
            PortEntity p = new PortEntity();
            assertNull(p.getName());
            assertNull(p.getId());
            assertNull(p.getInstance());
            assertNull(p.getConnectedDevice());
            assertNull(p.getConnectedPort());
            assertNull(p.getCna());
            assertNull(p.getEid());

            p.setName("400GE 0/0/1");
            p.setId(1);
            p.setInstance(0);
            p.setConnectedDevice("rack1#la0");
            p.setConnectedPort("400GE 1/0/1");
            p.setCna("0xAAAA1111");
            p.setEid("AAAAAA11000000000000000000000001");

            assertEquals("400GE 0/0/1", p.getName());
            assertEquals(Integer.valueOf(1), p.getId());
            assertEquals(Integer.valueOf(0), p.getInstance());
            assertEquals("rack1#la0", p.getConnectedDevice());
            assertEquals("400GE 1/0/1", p.getConnectedPort());
            assertEquals("0xAAAA1111", p.getCna());
            assertEquals("AAAAAA11000000000000000000000001", p.getEid());
        }

        @Test
        @DisplayName("toString")
        void testToString() {
            PortEntity p = new PortEntity();
            p.setName("GE0");
            String s = p.toString();
            assertTrue(s.contains("GE0"));
        }
    }

    // ==================== LogicPortEntity ====================
    @Nested
    @DisplayName("LogicPortEntity 测试")
    class LogicPortEntityTest {
        @Test
        @DisplayName("无参构造 + getter/setter")
        void testNoArgConstructorAndSetters() {
            LogicPortEntity lp = new LogicPortEntity();
            assertNull(lp.getName());
            assertNull(lp.getCna());
            assertNull(lp.getEid());
            assertNull(lp.getPorts());

            lp.setName("port_group1");
            lp.setCna("0xCCCC3333");
            lp.setEid("CCCC3333000000000000000000000003");
            List<String> ports = Arrays.asList("400GE 0/0/1", "400GE 0/0/2");
            lp.setPorts(ports);

            assertEquals("port_group1", lp.getName());
            assertEquals("0xCCCC3333", lp.getCna());
            assertEquals("CCCC3333000000000000000000000003", lp.getEid());
            assertSame(ports, lp.getPorts());
        }

        @Test
        @DisplayName("toString")
        void testToString() {
            LogicPortEntity lp = new LogicPortEntity();
            lp.setName("lg1");
            String s = lp.toString();
            assertTrue(s.contains("lg1"));
        }
    }

    // ==================== DeviceEntity ====================
    @Nested
    @DisplayName("DeviceEntity 测试")
    class DeviceEntityTest {
        @Test
        @DisplayName("无参构造 + getter/setter")
        void testNoArgConstructorAndSetters() {
            DeviceEntity d = new DeviceEntity();
            assertNull(d.getDeviceId());
            assertNull(d.getDeviceType());
            assertNull(d.getIp());
            assertNull(d.getRack());
            assertNull(d.getOsZone());
            assertNull(d.getModuleidx());
            assertNull(d.getIndex());
            assertNull(d.getPorts());
            assertNull(d.getLogicPorts());
            assertNull(d.getRouterTables());

            d.setDeviceId("rack1#la0");
            d.setDeviceType(DeviceType.LA);
            d.setIp("1.1.1.1");
            d.setRack("rack1");
            d.setOsZone(0);
            d.setModuleidx(1);
            d.setIndex(2);
            d.setPorts(new ArrayList<>());
            d.setLogicPorts(new ArrayList<>());
            d.setRouterTables(new ArrayList<>());

            assertEquals("rack1#la0", d.getDeviceId());
            assertEquals(DeviceType.LA, d.getDeviceType());
            assertEquals("1.1.1.1", d.getIp());
            assertEquals("rack1", d.getRack());
            assertEquals(Integer.valueOf(0), d.getOsZone());
            assertEquals(Integer.valueOf(1), d.getModuleidx());
            assertEquals(Integer.valueOf(2), d.getIndex());
            assertNotNull(d.getPorts());
            assertNotNull(d.getLogicPorts());
            assertNotNull(d.getRouterTables());
        }

        @Test
        @DisplayName("findPort - 找到")
        void testFindPortFound() {
            DeviceEntity d = new DeviceEntity();
            PortEntity p1 = new PortEntity();
            p1.setName("GE0");
            PortEntity p2 = new PortEntity();
            p2.setName("GE1");
            d.setPorts(Arrays.asList(p1, p2));

            PortEntity found = d.findPort("GE1");
            assertNotNull(found);
            assertEquals("GE1", found.getName());
        }

        @Test
        @DisplayName("findPort - 未找到")
        void testFindPortNotFound() {
            DeviceEntity d = new DeviceEntity();
            PortEntity p1 = new PortEntity();
            p1.setName("GE0");
            d.setPorts(Arrays.asList(p1));

            assertNull(d.findPort("GE99"));
        }

        @Test
        @DisplayName("findPort - ports 为 null")
        void testFindPortNullPorts() {
            DeviceEntity d = new DeviceEntity();
            assertNull(d.findPort("GE0"));
        }

        @Test
        @DisplayName("findPort - portName 为 null")
        void testFindPortNullName() {
            DeviceEntity d = new DeviceEntity();
            d.setPorts(Arrays.asList(new PortEntity()));
            assertNull(d.findPort(null));
        }

        @Test
        @DisplayName("findLogicPort - 找到")
        void testFindLogicPortFound() {
            DeviceEntity d = new DeviceEntity();
            LogicPortEntity lp = new LogicPortEntity();
            lp.setName("lg1");
            d.setLogicPorts(Arrays.asList(lp));

            LogicPortEntity found = d.findLogicPort("lg1");
            assertNotNull(found);
            assertEquals("lg1", found.getName());
        }

        @Test
        @DisplayName("findLogicPort - 未找到")
        void testFindLogicPortNotFound() {
            DeviceEntity d = new DeviceEntity();
            d.setLogicPorts(new ArrayList<>());
            assertNull(d.findLogicPort("lg99"));
        }

        @Test
        @DisplayName("findLogicPort - logicPorts 为 null")
        void testFindLogicPortNull() {
            DeviceEntity d = new DeviceEntity();
            assertNull(d.findLogicPort("lg1"));
        }

        @Test
        @DisplayName("findLogicPort - name 为 null")
        void testFindLogicPortNullName() {
            DeviceEntity d = new DeviceEntity();
            d.setLogicPorts(Arrays.asList(new LogicPortEntity()));
            assertNull(d.findLogicPort(null));
        }

        @Test
        @DisplayName("findRouterTable - 找到")
        void testFindRouterTableFound() {
            DeviceEntity d = new DeviceEntity();
            RouterTable rt0 = new RouterTable();
            rt0.setInstance(0);
            RouterTable rt1 = new RouterTable();
            rt1.setInstance(1);
            d.setRouterTables(Arrays.asList(rt0, rt1));

            RouterTable found = d.findRouterTable(1);
            assertNotNull(found);
            assertEquals(Integer.valueOf(1), found.getInstance());
        }

        @Test
        @DisplayName("findRouterTable - 未找到")
        void testFindRouterTableNotFound() {
            DeviceEntity d = new DeviceEntity();
            d.setRouterTables(new ArrayList<>());
            assertNull(d.findRouterTable(99));
        }

        @Test
        @DisplayName("findRouterTable - routerTables 为 null")
        void testFindRouterTableNull() {
            DeviceEntity d = new DeviceEntity();
            assertNull(d.findRouterTable(0));
        }

        @Test
        @DisplayName("findRouterTable - instance 为 null")
        void testFindRouterTableNullInstance() {
            DeviceEntity d = new DeviceEntity();
            d.setRouterTables(Arrays.asList(new RouterTable()));
            assertNull(d.findRouterTable(null));
        }

        @Test
        @DisplayName("toString - null fields")
        void testToString() {
            DeviceEntity d = new DeviceEntity();
            d.setDeviceId("dev1");
            String s = d.toString();
            assertTrue(s.contains("dev1"));
        }
    }

    // ==================== TopoData ====================
    @Nested
    @DisplayName("TopoData 测试")
    class TopoDataTest {
        @Test
        @DisplayName("无参构造 + getter/setter")
        void testNoArgConstructorAndSetters() {
            TopoData t = new TopoData();
            assertNull(t.getName());
            assertNull(t.getVersion());
            assertNull(t.getDevices());

            t.setName("topo1");
            t.setVersion("v1.0");
            Map<String, DeviceEntity> devices = new HashMap<>();
            t.setDevices(devices);

            assertEquals("topo1", t.getName());
            assertEquals("v1.0", t.getVersion());
            assertSame(devices, t.getDevices());
        }

        @Test
        @DisplayName("findDevice - 找到")
        void testFindDeviceFound() {
            TopoData t = new TopoData();
            Map<String, DeviceEntity> devices = new HashMap<>();
            DeviceEntity d = new DeviceEntity();
            d.setDeviceId("dev1");
            devices.put("dev1", d);
            t.setDevices(devices);

            DeviceEntity found = t.findDevice("dev1");
            assertNotNull(found);
            assertEquals("dev1", found.getDeviceId());
        }

        @Test
        @DisplayName("findDevice - 未找到")
        void testFindDeviceNotFound() {
            TopoData t = new TopoData();
            Map<String, DeviceEntity> devices = new HashMap<>();
            t.setDevices(devices);
            assertNull(t.findDevice("dev99"));
        }

        @Test
        @DisplayName("findDevice - devices 为 null")
        void testFindDeviceNullDevices() {
            TopoData t = new TopoData();
            assertNull(t.findDevice("dev1"));
        }

        @Test
        @DisplayName("findDevice - deviceId 为 null")
        void testFindDeviceNullId() {
            TopoData t = new TopoData();
            t.setDevices(new HashMap<>());
            assertNull(t.findDevice(null));
        }

        @Test
        @DisplayName("toString - null devices")
        void testToString() {
            TopoData t = new TopoData();
            t.setName("myTopo");
            String s = t.toString();
            assertTrue(s.contains("myTopo"));
            assertTrue(s.contains("devicesCount=0"));
        }

        @Test
        @DisplayName("toString - with devices")
        void testToStringWithDevices() {
            TopoData t = new TopoData();
            t.setName("myTopo");
            Map<String, DeviceEntity> devices = new HashMap<>();
            devices.put("d1", new DeviceEntity());
            t.setDevices(devices);
            String s = t.toString();
            assertTrue(s.contains("devicesCount=1"));
        }
    }
}
