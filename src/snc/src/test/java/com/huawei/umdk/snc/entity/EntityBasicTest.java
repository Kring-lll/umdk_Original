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
    // ==================== AclKey ====================
    @Nested
    @DisplayName("AclKey 测试")
    class AclKeyTest {
        @Test
        @DisplayName("无参构造 + getter/setter")
        void testNoArgConstructorAndSetters() {
            AclKey k = new AclKey();
            assertNull(k.getSourceEid());
            assertNull(k.getDestEid());
            assertNull(k.getTransportType());
            k.setSourceEid("src123");
            k.setDestEid("dst456");
            k.setTransportType(TransportType.RTP);
            assertEquals("src123", k.getSourceEid());
            assertEquals("dst456", k.getDestEid());
            assertEquals(TransportType.RTP, k.getTransportType());
        }

        @Test
        @DisplayName("有参构造")
        void testParamConstructor() {
            AclKey k = new AclKey("src123", "dst456", TransportType.UTP);
            assertEquals("src123", k.getSourceEid());
            assertEquals("dst456", k.getDestEid());
            assertEquals(TransportType.UTP, k.getTransportType());
        }

        @Test
        @DisplayName("equals - 相同对象")
        void testEqualsSame() {
            AclKey k = new AclKey("a", "b", TransportType.RTP);
            assertEquals(k, k);
        }

        @Test
        @DisplayName("equals - null")
        void testEqualsNull() {
            AclKey k = new AclKey("a", "b", TransportType.RTP);
            assertNotEquals(null, k);
        }

        @Test
        @DisplayName("equals - 不同类")
        void testEqualsDifferentClass() {
            AclKey k = new AclKey("a", "b", TransportType.RTP);
            assertNotEquals("string", k);
        }

        @Test
        @DisplayName("equals - 相同值")
        void testEqualsSameValue() {
            AclKey k1 = new AclKey("src", "dst", TransportType.CTP);
            AclKey k2 = new AclKey("src", "dst", TransportType.CTP);
            assertEquals(k1, k2);
        }

        @Test
        @DisplayName("equals - 不同 sourceEid")
        void testEqualsDifferentSrc() {
            AclKey k1 = new AclKey("src1", "dst", TransportType.RTP);
            AclKey k2 = new AclKey("src2", "dst", TransportType.RTP);
            assertNotEquals(k1, k2);
        }

        @Test
        @DisplayName("equals - 不同 destEid")
        void testEqualsDifferentDst() {
            AclKey k1 = new AclKey("src", "dst1", TransportType.RTP);
            AclKey k2 = new AclKey("src", "dst2", TransportType.RTP);
            assertNotEquals(k1, k2);
        }

        @Test
        @DisplayName("equals - 不同 transportType")
        void testEqualsDifferentTransport() {
            AclKey k1 = new AclKey("src", "dst", TransportType.RTP);
            AclKey k2 = new AclKey("src", "dst", TransportType.UTP);
            assertNotEquals(k1, k2);
        }

        @Test
        @DisplayName("hashCode - 相同值")
        void testHashCodeSameValue() {
            AclKey k1 = new AclKey("src", "dst", TransportType.CTP);
            AclKey k2 = new AclKey("src", "dst", TransportType.CTP);
            assertEquals(k1.hashCode(), k2.hashCode());
        }

        @Test
        @DisplayName("hashCode - 不同值")
        void testHashCodeDifferentValue() {
            AclKey k1 = new AclKey("src", "dst", TransportType.RTP);
            AclKey k2 = new AclKey("other", "dst", TransportType.RTP);
            assertNotEquals(k1.hashCode(), k2.hashCode());
        }

        @Test
        @DisplayName("toString")
        void testToString() {
            AclKey k = new AclKey("src", "dst", TransportType.RTP);
            String s = k.toString();
            assertTrue(s.contains("src"));
            assertTrue(s.contains("dst"));
            assertTrue(s.contains("RTP"));
        }
    }

    // ==================== AclData ====================
    @Nested
    @DisplayName("AclData 测试")
    class AclDataTest {
        @Test
        @DisplayName("无参构造 + getter/setter")
        void testNoArgConstructorAndSetters() {
            AclData d = new AclData();
            assertNull(d.getAclId());
            assertNull(d.getTpAcls());
            d.setAclId("acl-1");
            Map<AclKey, TpAclEntity> map = new HashMap<>();
            d.setTpAcls(map);
            assertEquals("acl-1", d.getAclId());
            assertSame(map, d.getTpAcls());
        }

        @Test
        @DisplayName("findAcl - 找到")
        void testFindAclFound() {
            AclData d = new AclData();
            Map<AclKey, TpAclEntity> map = new HashMap<>();
            TpAclEntity entity = new TpAclEntity();
            entity.setSourceEid("src");
            entity.setDestEid("dst");
            entity.setTransportType(TransportType.RTP);
            map.put(entity.toKey(), entity);
            d.setTpAcls(map);

            TpAclEntity found = d.findAcl("src", "dst", TransportType.RTP);
            assertNotNull(found);
            assertEquals(entity, found);
        }

        @Test
        @DisplayName("findAcl - 未找到")
        void testFindAclNotFound() {
            AclData d = new AclData();
            d.setTpAcls(new HashMap<>());
            TpAclEntity found = d.findAcl("src", "dst", TransportType.RTP);
            assertNull(found);
        }

        @Test
        @DisplayName("findAcl - tpAcls 为 null")
        void testFindAclNullMap() {
            AclData d = new AclData();
            assertNull(d.findAcl("src", "dst", TransportType.RTP));
        }

        @Test
        @DisplayName("toString")
        void testToString() {
            AclData d = new AclData();
            d.setAclId("acl-001");
            String s = d.toString();
            assertTrue(s.contains("acl-001"));
        }

        @Test
        @DisplayName("toString - tpAcls 为 null 时不抛异常")
        void testToStringNullTpAcls() {
            AclData d = new AclData();
            assertNotNull(d.toString());
        }
    }

    // ==================== TpAclEntity ====================
    @Nested
    @DisplayName("TpAclEntity 测试")
    class TpAclEntityTest {
        @Test
        @DisplayName("无参构造 + getter/setter")
        void testNoArgConstructorAndSetters() {
            TpAclEntity e = new TpAclEntity();
            assertNull(e.getSourceEid());
            assertNull(e.getDestEid());
            assertNull(e.getTransportType());
            assertNull(e.getSourceCna());
            assertNull(e.getDestCna());
            assertNull(e.getTemplateId());

            e.setSourceEid("src");
            e.setDestEid("dst");
            e.setTransportType(TransportType.RTP);
            e.setSourceCna("0xAAA");
            e.setDestCna("0xBBB");
            e.setTemplateId(5);

            assertEquals("src", e.getSourceEid());
            assertEquals("dst", e.getDestEid());
            assertEquals(TransportType.RTP, e.getTransportType());
            assertEquals("0xAAA", e.getSourceCna());
            assertEquals("0xBBB", e.getDestCna());
            assertEquals(Integer.valueOf(5), e.getTemplateId());
        }

        @Test
        @DisplayName("toKey")
        void testToKey() {
            TpAclEntity e = new TpAclEntity();
            e.setSourceEid("src");
            e.setDestEid("dst");
            e.setTransportType(TransportType.UTP);
            AclKey key = e.toKey();
            assertEquals("src", key.getSourceEid());
            assertEquals("dst", key.getDestEid());
            assertEquals(TransportType.UTP, key.getTransportType());
        }

        @Test
        @DisplayName("toString")
        void testToString() {
            TpAclEntity e = new TpAclEntity();
            e.setSourceEid("src");
            String s = e.toString();
            assertTrue(s.contains("src"));
        }
    }

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

    // ==================== PathPlanRequest ====================
    @Nested
    @DisplayName("PathPlanRequest 测试")
    class PathPlanRequestTest {
        @Test
        @DisplayName("无参构造 + getter/setter")
        void testNoArgConstructorAndSetters() {
            PathPlanRequest r = new PathPlanRequest();
            assertNull(r.getSrcDevice());
            assertNull(r.getSrcPort());
            assertNull(r.getDestDevice());
            assertNull(r.getDestPort());
            assertNull(r.getInterDevices());

            r.setSrcDevice("rack1#os0#npu1");
            r.setSrcPort("400GE 0/0/1");
            r.setDestDevice("rack1#os0#npu2");
            r.setDestPort("400GE 0/0/1");
            LinkedHashMap<String, String> inter = new LinkedHashMap<>();
            inter.put("rack1#la0", "400GE 1/0/2");
            r.setInterDevices(inter);

            assertEquals("rack1#os0#npu1", r.getSrcDevice());
            assertEquals("400GE 0/0/1", r.getSrcPort());
            assertEquals("rack1#os0#npu2", r.getDestDevice());
            assertEquals("400GE 0/0/1", r.getDestPort());
            assertSame(inter, r.getInterDevices());
        }

        @Test
        @DisplayName("toString")
        void testToString() {
            PathPlanRequest r = new PathPlanRequest();
            r.setSrcDevice("dev1");
            String s = r.toString();
            assertTrue(s.contains("dev1"));
        }
    }

    // ==================== PathPlanResult ====================
    @Nested
    @DisplayName("PathPlanResult 测试")
    class PathPlanResultTest {
        @Test
        @DisplayName("无参构造 + getter/setter")
        void testNoArgConstructorAndSetters() {
            PathPlanResult r = new PathPlanResult();
            assertNull(r.getStatus());
            assertEquals(0, r.getErrorCode());
            assertNull(r.getErrorMessage());
            assertNull(r.getPaths());

            r.setStatus(PlanStatus.SUCCESS);
            r.setErrorCode(0);
            r.setErrorMessage("OK");
            List<PathInfo> paths = new ArrayList<>();
            r.setPaths(paths);

            assertEquals(PlanStatus.SUCCESS, r.getStatus());
            assertEquals(0, r.getErrorCode());
            assertEquals("OK", r.getErrorMessage());
            assertSame(paths, r.getPaths());
        }

        @Test
        @DisplayName("工厂方法 success")
        void testFactorySuccess() {
            List<PathInfo> paths = Collections.singletonList(new PathInfo());
            PathPlanResult r = PathPlanResult.success(paths);
            assertEquals(PlanStatus.SUCCESS, r.getStatus());
            assertEquals(0, r.getErrorCode());
            assertNull(r.getErrorMessage());
            assertSame(paths, r.getPaths());
        }

        @Test
        @DisplayName("工厂方法 fail")
        void testFactoryFail() {
            PathPlanResult r = PathPlanResult.fail(PlanStatus.SRC_EID_NOT_FOUND, -3, "not found");
            assertEquals(PlanStatus.SRC_EID_NOT_FOUND, r.getStatus());
            assertEquals(-3, r.getErrorCode());
            assertEquals("not found", r.getErrorMessage());
            assertNull(r.getPaths());
        }

        @Test
        @DisplayName("toString")
        void testToString() {
            PathPlanResult r = PathPlanResult.success(Collections.emptyList());
            String s = r.toString();
            assertTrue(s.contains("SUCCESS"));
        }
    }

    // ==================== PathInfo ====================
    @Nested
    @DisplayName("PathInfo 测试")
    class PathInfoTest {
        @Test
        @DisplayName("无参构造 + getter/setter")
        void testNoArgConstructorAndSetters() {
            PathInfo p = new PathInfo();
            assertNull(p.getHops());

            List<HopInfo> hops = new ArrayList<>();
            p.setHops(hops);

            assertSame(hops, p.getHops());
        }

        @Test
        @DisplayName("toString")
        void testToString() {
            PathInfo p = new PathInfo();
            String s = p.toString();
            assertNotNull(s);
            assertTrue(s.contains("hopsCount=0"));
        }
    }

    // ==================== HopInfo ====================
    @Nested
    @DisplayName("HopInfo 测试")
    class HopInfoTest {
        @Test
        @DisplayName("无参构造 + getter/setter")
        void testNoArgConstructorAndSetters() {
            HopInfo h = new HopInfo();
            assertNull(h.getDeviceId());
            assertNull(h.getInPort());
            assertNull(h.getOutPort());
            assertNull(h.getDeviceType());

            h.setDeviceId("rack1#os0#npu1");
            h.setInPort("400GE 0/0/1");
            h.setOutPort("400GE 0/0/1");
            h.setDeviceType(DeviceType.NPU);

            assertEquals("rack1#os0#npu1", h.getDeviceId());
            assertEquals("400GE 0/0/1", h.getInPort());
            assertEquals("400GE 0/0/1", h.getOutPort());
            assertEquals(DeviceType.NPU, h.getDeviceType());
        }

        @Test
        @DisplayName("有参构造")
        void testParamConstructor() {
            HopInfo h = new HopInfo("dev1", "inP", "outP", DeviceType.LA);
            assertEquals("dev1", h.getDeviceId());
            assertEquals("inP", h.getInPort());
            assertEquals("outP", h.getOutPort());
            assertEquals(DeviceType.LA, h.getDeviceType());
        }

        @Test
        @DisplayName("toString")
        void testToString() {
            HopInfo h = new HopInfo("dev1", "in", "out", DeviceType.NPU);
            String s = h.toString();
            assertTrue(s.contains("dev1"));
            assertTrue(s.contains("NPU"));
        }
    }

    // ==================== PlanStatus ====================
    @Nested
    @DisplayName("PlanStatus 枚举测试")
    class PlanStatusTest {
        @Test
        @DisplayName("枚举值完整性")
        void testEnumValuesExist() {
            assertNotNull(PlanStatus.SUCCESS);
            assertNotNull(PlanStatus.FAILED);
            assertNotNull(PlanStatus.SRC_EID_NOT_FOUND);
            assertNotNull(PlanStatus.DEST_EID_NOT_FOUND);
        }

        @Test
        @DisplayName("getCode")
        void testGetCode() {
            assertEquals(0, PlanStatus.SUCCESS.getCode());
            assertEquals(1, PlanStatus.FAILED.getCode());
            assertEquals(1001, PlanStatus.SRC_EID_NOT_FOUND.getCode());
            assertEquals(1002, PlanStatus.DEST_EID_NOT_FOUND.getCode());
            assertEquals(1003, PlanStatus.SRC_INFO_ERR.getCode());
            assertEquals(1004, PlanStatus.DST_INFO_ERR.getCode());
            assertEquals(1005, PlanStatus.ACL_CHECK_FAILED.getCode());
            assertEquals(1006, PlanStatus.NO_ROUTE_FOUND.getCode());
            assertEquals(1007, PlanStatus.TOPO_INCOMPLETE.getCode());
            assertEquals(1008, PlanStatus.TOPO_CONNECTION_ERROR.getCode());
            assertEquals(1009, PlanStatus.TOPO_CONNECTION_NOT_FOUND.getCode());
            assertEquals(1010, PlanStatus.ROUTE_NOT_REACHABLE.getCode());
            assertEquals(1011, PlanStatus.MULTI_PATH_NOT_SUPPORTED.getCode());
            assertEquals(3002, PlanStatus.SRC_AND_DST_MUST_BE_NPU.getCode());
            assertEquals(2001, PlanStatus.TOPO_NOT_FOUND.getCode());
            assertEquals(2002, PlanStatus.ACL_NOT_FOUND.getCode());
            assertEquals(2003, PlanStatus.ACL_DENIED.getCode());
            assertEquals(2004, PlanStatus.PATH_NOT_FOUND.getCode());
            assertEquals(3001, PlanStatus.INVALID_PARAM.getCode());
        }

        @Test
        @DisplayName("getMessage")
        void testGetMessage() {
            assertEquals("成功", PlanStatus.SUCCESS.getMessage());
            assertNotNull(PlanStatus.SRC_EID_NOT_FOUND.getMessage());
        }

        @Test
        @DisplayName("fromCode - 所有已知 code")
        void testFromCodeAll() {
            assertEquals(PlanStatus.SUCCESS, PlanStatus.fromCode(0));
            assertEquals(PlanStatus.FAILED, PlanStatus.fromCode(1));
            assertEquals(PlanStatus.SRC_EID_NOT_FOUND, PlanStatus.fromCode(1001));
            assertEquals(PlanStatus.DEST_EID_NOT_FOUND, PlanStatus.fromCode(1002));
            assertEquals(PlanStatus.SRC_INFO_ERR, PlanStatus.fromCode(1003));
            assertEquals(PlanStatus.DST_INFO_ERR, PlanStatus.fromCode(1004));
            assertEquals(PlanStatus.ACL_CHECK_FAILED, PlanStatus.fromCode(1005));
            assertEquals(PlanStatus.NO_ROUTE_FOUND, PlanStatus.fromCode(1006));
            assertEquals(PlanStatus.TOPO_INCOMPLETE, PlanStatus.fromCode(1007));
            assertEquals(PlanStatus.TOPO_CONNECTION_ERROR, PlanStatus.fromCode(1008));
            assertEquals(PlanStatus.TOPO_CONNECTION_NOT_FOUND, PlanStatus.fromCode(1009));
            assertEquals(PlanStatus.ROUTE_NOT_REACHABLE, PlanStatus.fromCode(1010));
            assertEquals(PlanStatus.MULTI_PATH_NOT_SUPPORTED, PlanStatus.fromCode(1011));
            assertEquals(PlanStatus.SRC_AND_DST_MUST_BE_NPU, PlanStatus.fromCode(3002));
            assertEquals(PlanStatus.TOPO_NOT_FOUND, PlanStatus.fromCode(2001));
            assertEquals(PlanStatus.ACL_NOT_FOUND, PlanStatus.fromCode(2002));
            assertEquals(PlanStatus.ACL_DENIED, PlanStatus.fromCode(2003));
            assertEquals(PlanStatus.PATH_NOT_FOUND, PlanStatus.fromCode(2004));
            assertEquals(PlanStatus.INVALID_PARAM, PlanStatus.fromCode(3001));
        }

        @Test
        @DisplayName("fromCode - 未知 code 返回 FAILED")
        void testFromCodeUnknown() {
            assertEquals(PlanStatus.FAILED, PlanStatus.fromCode(9999));
            assertEquals(PlanStatus.FAILED, PlanStatus.fromCode(-1));
        }

        @Test
        @DisplayName("isSuccess - SUCCESS")
        void testIsSuccessTrue() {
            assertTrue(PlanStatus.SUCCESS.isSuccess());
        }

        @Test
        @DisplayName("isSuccess - 非 SUCCESS")
        void testIsSuccessFalse() {
            assertFalse(PlanStatus.FAILED.isSuccess());
            assertFalse(PlanStatus.SRC_EID_NOT_FOUND.isSuccess());
            assertFalse(PlanStatus.INVALID_PARAM.isSuccess());
        }
    }

    // ==================== CandidateOutPort ====================
    @Nested
    @DisplayName("CandidateOutPort 测试")
    class CandidateOutPortTest {
        @Test
        @DisplayName("无参构造 + getter/setter")
        void testNoArgConstructorAndSetters() {
            CandidateOutPort c = new CandidateOutPort();
            assertNull(c.getOutInterface());
            assertNull(c.getNextHop());
            assertFalse(c.isSelected());
            c.setOutInterface("eth0/1");
            c.setNextHop("10.0.0.1");
            c.setSelected(true);
            assertEquals("eth0/1", c.getOutInterface());
            assertEquals("10.0.0.1", c.getNextHop());
            assertTrue(c.isSelected());
        }

        @Test
        @DisplayName("有参构造")
        void testParamConstructor() {
            CandidateOutPort c = new CandidateOutPort("eth0/2", "10.0.0.2", true);
            assertEquals("eth0/2", c.getOutInterface());
            assertEquals("10.0.0.2", c.getNextHop());
            assertTrue(c.isSelected());
        }

        @Test
        @DisplayName("有参构造 - selected=false")
        void testParamConstructorNotSelected() {
            CandidateOutPort c = new CandidateOutPort("eth1/0", "192.168.1.1", false);
            assertEquals("eth1/0", c.getOutInterface());
            assertEquals("192.168.1.1", c.getNextHop());
            assertFalse(c.isSelected());
        }

        @Test
        @DisplayName("toString 不抛异常且包含字段")
        void testToString() {
            CandidateOutPort c = new CandidateOutPort("eth0/1", "10.0.0.1", true);
            String s = c.toString();
            assertNotNull(s);
            assertTrue(s.contains("eth0/1"));
            assertTrue(s.contains("10.0.0.1"));
            assertTrue(s.contains("true"));
        }

        @Test
        @DisplayName("默认值 selected=false")
        void testSelectedDefault() {
            CandidateOutPort c = new CandidateOutPort();
            assertFalse(c.isSelected());
        }

        @Test
        @DisplayName("setSelected false → isSelected false")
        void testSelectedFalse() {
            CandidateOutPort c = new CandidateOutPort();
            c.setSelected(false);
            assertFalse(c.isSelected());
        }
    }

    // ==================== Direction ====================
    @Nested
    @DisplayName("Direction 枚举测试")
    class DirectionTest {
        @Test
        @DisplayName("枚举值存在")
        void testEnumValuesExist() {
            assertNotNull(RouteSelectionRecord.Direction.FORWARD);
            assertNotNull(RouteSelectionRecord.Direction.REVERSE);
        }

        @Test
        @DisplayName("枚举数量 = 2")
        void testEnumCount() {
            assertEquals(2, RouteSelectionRecord.Direction.values().length);
        }

        @Test
        @DisplayName("valueOf - FORWARD")
        void testValueOfForward() {
            assertEquals(RouteSelectionRecord.Direction.FORWARD, RouteSelectionRecord.Direction.valueOf("FORWARD"));
        }

        @Test
        @DisplayName("valueOf - REVERSE")
        void testValueOfReverse() {
            assertEquals(RouteSelectionRecord.Direction.REVERSE, RouteSelectionRecord.Direction.valueOf("REVERSE"));
        }

        @Test
        @DisplayName("ordinal 顺序")
        void testOrdinal() {
            assertEquals(0, RouteSelectionRecord.Direction.FORWARD.ordinal());
            assertEquals(1, RouteSelectionRecord.Direction.REVERSE.ordinal());
        }

        @Test
        @DisplayName("name 方法")
        void testName() {
            assertEquals("FORWARD", RouteSelectionRecord.Direction.FORWARD.name());
            assertEquals("REVERSE", RouteSelectionRecord.Direction.REVERSE.name());
        }
    }

    // ==================== InternalPathHop ====================
    @Nested
    @DisplayName("InternalPathHop 测试")
    class InternalPathHopTest {
        @Test
        @DisplayName("无参构造 + getter/setter")
        void testNoArgConstructorAndSetters() {
            InternalPathHop hop = new InternalPathHop();
            assertNull(hop.getDeviceId());
            assertNull(hop.getDeviceType());
            assertNull(hop.getInPort());
            assertNull(hop.getOutPort());
            assertNull(hop.getCna());
            assertNull(hop.getEid());
            assertNull(hop.getConnectedDevice());
            assertNull(hop.getConnectedPort());
            assertNull(hop.getRack());
            assertEquals(0, hop.getHopIndex());

            hop.setDeviceId("dev1");
            hop.setDeviceType(DeviceType.NPU);
            hop.setInPort("p1");
            hop.setOutPort("p2");
            hop.setCna("0x123456");
            hop.setEid("EID001");
            hop.setConnectedDevice("dev2");
            hop.setConnectedPort("p3");
            hop.setRack("rack01");
            hop.setHopIndex(3);

            assertEquals("dev1", hop.getDeviceId());
            assertEquals(DeviceType.NPU, hop.getDeviceType());
            assertEquals("p1", hop.getInPort());
            assertEquals("p2", hop.getOutPort());
            assertEquals("0x123456", hop.getCna());
            assertEquals("EID001", hop.getEid());
            assertEquals("dev2", hop.getConnectedDevice());
            assertEquals("p3", hop.getConnectedPort());
            assertEquals("rack01", hop.getRack());
            assertEquals(3, hop.getHopIndex());
        }

        @Test
        @DisplayName("toString 不抛异常且包含字段")
        void testToString() {
            InternalPathHop hop = new InternalPathHop();
            hop.setDeviceId("dev1");
            hop.setDeviceType(DeviceType.LA);
            hop.setInPort("in1");
            hop.setOutPort("out1");
            hop.setCna("0xAAAAAA");
            hop.setEid("EID-X");
            hop.setHopIndex(0);
            String s = hop.toString();
            assertNotNull(s);
            assertTrue(s.contains("dev1"));
            assertTrue(s.contains("EID-X"));
        }

        @Test
        @DisplayName("toString - 所有字段 null 不抛异常")
        void testToStringAllNull() {
            InternalPathHop hop = new InternalPathHop();
            String s = hop.toString();
            assertNotNull(s);
            assertTrue(s.contains("InternalPathHop"));
        }

        @Test
        @DisplayName("DeviceType 枚举覆盖")
        void testDeviceTypeEnumValues() {
            InternalPathHop hop = new InternalPathHop();
            hop.setDeviceType(DeviceType.NPU);
            assertEquals(DeviceType.NPU, hop.getDeviceType());
            hop.setDeviceType(DeviceType.LA);
            assertEquals(DeviceType.LA, hop.getDeviceType());
        }

        @Test
        @DisplayName("hopIndex 边界值 - 最小")
        void testHopIndexMin() {
            InternalPathHop hop = new InternalPathHop();
            hop.setHopIndex(0);
            assertEquals(0, hop.getHopIndex());
        }

        @Test
        @DisplayName("hopIndex 边界值 - 最大")
        void testHopIndexMax() {
            InternalPathHop hop = new InternalPathHop();
            hop.setHopIndex(Integer.MAX_VALUE);
            assertEquals(Integer.MAX_VALUE, hop.getHopIndex());
        }
    }

    // ==================== InternalPathInfo ====================
    @Nested
    @DisplayName("InternalPathInfo 测试")
    class InternalPathInfoTest {
        @Test
        @DisplayName("无参构造 + getter/setter")
        void testNoArgConstructorAndSetters() {
            InternalPathInfo info = new InternalPathInfo();
            assertNull(info.getHops());
            assertNull(info.getSourceEid());
            assertNull(info.getDestEid());
            assertNull(info.getSourceCna());
            assertNull(info.getDestCna());
            assertEquals(0, info.getHopCount());

            List<InternalPathHop> hops = new ArrayList<>();
            hops.add(new InternalPathHop());
            info.setHops(hops);
            info.setSourceEid("EID-SRC");
            info.setDestEid("EID-DST");
            info.setSourceCna("0x111111");
            info.setDestCna("0x222222");
            info.setHopCount(5);

            assertEquals(hops, info.getHops());
            assertEquals("EID-SRC", info.getSourceEid());
            assertEquals("EID-DST", info.getDestEid());
            assertEquals("0x111111", info.getSourceCna());
            assertEquals("0x222222", info.getDestCna());
            assertEquals(5, info.getHopCount());
        }

        @Test
        @DisplayName("toString - 有 hops 列表")
        void testToStringWithHops() {
            InternalPathInfo info = new InternalPathInfo();
            info.setSourceEid("EID-SRC");
            info.setDestEid("EID-DST");
            info.setSourceCna("0xAAAAAA");
            info.setDestCna("0xBBBBBB");
            info.setHopCount(2);

            List<InternalPathHop> hops = new ArrayList<>();
            hops.add(new InternalPathHop());
            hops.add(new InternalPathHop());
            info.setHops(hops);

            String s = info.toString();
            assertNotNull(s);
            assertTrue(s.contains("EID-SRC"));
            assertTrue(s.contains("EID-DST"));
            assertTrue(s.contains("hopsCount=2"));
        }

        @Test
        @DisplayName("toString - hops=null 不抛异常")
        void testToStringNullHops() {
            InternalPathInfo info = new InternalPathInfo();
            info.setSourceEid("S");
            info.setDestEid("D");
            String s = info.toString();
            assertNotNull(s);
            assertTrue(s.contains("hopsCount=0"));
        }

        @Test
        @DisplayName("toString - 空 hops 列表")
        void testToStringEmptyHops() {
            InternalPathInfo info = new InternalPathInfo();
            info.setHops(Collections.emptyList());
            String s = info.toString();
            assertNotNull(s);
            assertTrue(s.contains("hopsCount=0"));
        }

        @Test
        @DisplayName("hopCount 边界值测试")
        void testHopCountBoundary() {
            InternalPathInfo info = new InternalPathInfo();
            info.setHopCount(0);
            assertEquals(0, info.getHopCount());
            info.setHopCount(Integer.MAX_VALUE);
            assertEquals(Integer.MAX_VALUE, info.getHopCount());
        }
    }

    // ==================== RouteSelectionRecord ====================
    @Nested
    @DisplayName("RouteSelectionRecord 测试")
    class RouteSelectionRecordTest {
        @Test
        @DisplayName("无参构造 + getter/setter")
        void testNoArgConstructorAndSetters() {
            RouteSelectionRecord r = new RouteSelectionRecord();
            assertNull(r.getDeviceId());
            assertNull(r.getPrefix());
            assertNull(r.getCandidateOutPorts());
            assertNull(r.getScna());
            assertNull(r.getDcna());
            assertNull(r.getHashInfo());
            assertNull(r.getDirection());

            List<CandidateOutPort> candidates = new ArrayList<>();
            candidates.add(new CandidateOutPort("eth0/1", "10.0.0.1", true));
            r.setDeviceId("devA");
            r.setPrefix("192.168.0.0/16");
            r.setCandidateOutPorts(candidates);
            r.setScna("0xAA0000");
            r.setDcna("0xBB0000");
            r.setHashInfo("hash123");
            r.setDirection(RouteSelectionRecord.Direction.FORWARD);

            assertEquals("devA", r.getDeviceId());
            assertEquals("192.168.0.0/16", r.getPrefix());
            assertEquals(candidates, r.getCandidateOutPorts());
            assertEquals("0xAA0000", r.getScna());
            assertEquals("0xBB0000", r.getDcna());
            assertEquals("hash123", r.getHashInfo());
            assertEquals(RouteSelectionRecord.Direction.FORWARD, r.getDirection());
        }

        @Test
        @DisplayName("Direction 枚举 - FORWARD")
        void testDirectionForward() {
            RouteSelectionRecord r = new RouteSelectionRecord();
            r.setDirection(RouteSelectionRecord.Direction.FORWARD);
            assertEquals(RouteSelectionRecord.Direction.FORWARD, r.getDirection());
        }

        @Test
        @DisplayName("Direction 枚举 - REVERSE")
        void testDirectionReverse() {
            RouteSelectionRecord r = new RouteSelectionRecord();
            r.setDirection(RouteSelectionRecord.Direction.REVERSE);
            assertEquals(RouteSelectionRecord.Direction.REVERSE, r.getDirection());
        }

        @Test
        @DisplayName("toString - 正常场景")
        void testToStringNormal() {
            RouteSelectionRecord r = new RouteSelectionRecord();
            r.setDeviceId("devA");
            r.setPrefix("10.0.0.0/8");
            r.setDirection(RouteSelectionRecord.Direction.FORWARD);
            String s = r.toString();
            assertNotNull(s);
            assertTrue(s.contains("devA"));
            assertTrue(s.contains("10.0.0.0/8"));
            assertTrue(s.contains("FORWARD"));
        }

        @Test
        @DisplayName("toString - candidateOutPorts=null 不抛异常")
        void testToStringNullCandidates() {
            RouteSelectionRecord r = new RouteSelectionRecord();
            r.setDeviceId("devB");
            r.setDirection(RouteSelectionRecord.Direction.REVERSE);
            String s = r.toString();
            assertNotNull(s);
            assertTrue(s.contains("candidateCount=0"));
        }

        @Test
        @DisplayName("toString - 空 candidateOutPorts 列表")
        void testToStringEmptyCandidates() {
            RouteSelectionRecord r = new RouteSelectionRecord();
            r.setCandidateOutPorts(Collections.emptyList());
            String s = r.toString();
            assertNotNull(s);
            assertTrue(s.contains("candidateCount=0"));
        }

        @Test
        @DisplayName("toString - 有候选端口")
        void testToStringWithCandidates() {
            RouteSelectionRecord r = new RouteSelectionRecord();
            List<CandidateOutPort> candidates = new ArrayList<>();
            candidates.add(new CandidateOutPort("eth0", "1.1.1.1", true));
            candidates.add(new CandidateOutPort("eth1", "2.2.2.2", false));
            r.setCandidateOutPorts(candidates);
            String s = r.toString();
            assertNotNull(s);
            assertTrue(s.contains("candidateCount=2"));
        }

        @Test
        @DisplayName("hashInfo setter/getter")
        void testHashInfo() {
            RouteSelectionRecord r = new RouteSelectionRecord();
            assertNull(r.getHashInfo());
            r.setHashInfo("hash-abc-123");
            assertEquals("hash-abc-123", r.getHashInfo());
            r.setHashInfo(null);
            assertNull(r.getHashInfo());
        }

        @Test
        @DisplayName("prefix setter/getter - null")
        void testPrefixNull() {
            RouteSelectionRecord r = new RouteSelectionRecord();
            r.setPrefix("192.168.1.0/24");
            assertEquals("192.168.1.0/24", r.getPrefix());
            r.setPrefix(null);
            assertNull(r.getPrefix());
        }
    }
}
