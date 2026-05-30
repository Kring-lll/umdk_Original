package com.huawei.umdk.snc.engine;

import com.huawei.umdk.snc.entity.*;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.DisplayName;
import org.junit.jupiter.api.Nested;
import org.junit.jupiter.api.Test;

import java.util.*;

import static org.junit.jupiter.api.Assertions.*;

/**
 * AclCheckEngine 单元测试
 *
 * 覆盖 checkPath 所有分支：
 * 1. aclData/result/topoData 为 null → 直接返回
 * 2. result.status 非 null 且非 SUCCESS → 直接返回
 * 3. tpAcls 为 null/empty → 直接返回
 * 4. srcEid/dstEid 为 null → 直接返回
 * 5. ACL rule 为 null → skip
 * 6. EID 不匹配 → continue
 * 7. ACL 未找到 → ACL_NOT_FOUND
 * 8. CNA 不匹配 → ACL_DENIED
 * 9. CNA 匹配成功 → 不修改 status (保持 SUCCESS)
 * 10. 私有方法 findSrcPortCna/findDstPortCna/findPortCna 通过公共方法间接覆盖
 */
@DisplayName("AclCheckEngine 单元测试")
class AclCheckEngineTest {

    private AclCheckEngine engine;

    @BeforeEach
    void setUp() {
        engine = new AclCheckEngine();
    }

    /**
     * 辅助：创建带 EID 的 PathPlanResult（SUCCESS 状态，包含 HopInfo）
     */
    private PathPlanResult createResult(String srcEid, String dstEid,
                                          String srcDeviceId, String srcPort,
                                          String dstDeviceId, String dstPort) {
        PathPlanResult result = new PathPlanResult();
        result.setStatus(PlanStatus.SUCCESS);
        result.setSourceEid(srcEid);
        result.setDestEid(dstEid);

        HopInfo srcHop = new HopInfo();
        srcHop.setDeviceId(srcDeviceId);
        srcHop.setOutPort(srcPort);

        HopInfo dstHop = new HopInfo();
        dstHop.setDeviceId(dstDeviceId);
        dstHop.setOutPort(dstPort);
        dstHop.setInPort(dstPort);

        PathInfo path = new PathInfo();
        path.setHops(Arrays.asList(srcHop, dstHop));
        result.setPaths(Collections.singletonList(path));
        return result;
    }

    /**
     * 辅助：创建含指定 sourceEid/destEid/CNA 的 TpAclEntity
     */
    private TpAclEntity createRule(String srcEid, String dstEid, String srcCna, String dstCna) {
        TpAclEntity rule = new TpAclEntity();
        rule.setSourceEid(srcEid);
        rule.setDestEid(dstEid);
        rule.setSourceCna(srcCna);
        rule.setDestCna(dstCna);
        return rule;
    }

    /**
     * 辅助：创建简单的 TopoData（包含源/目的设备，每设备一个端口含 CNA）
     */
    private TopoData createTopo(String srcDeviceId, String srcPortName, String srcPortCna,
                                 String dstDeviceId, String dstPortName, String dstPortCna) {
        TopoData topo = new TopoData();
        Map<String, DeviceEntity> devices = new LinkedHashMap<>();

        // 源设备
        DeviceEntity srcDev = new DeviceEntity();
        srcDev.setDeviceId(srcDeviceId);
        PortEntity srcPort = new PortEntity();
        srcPort.setName(srcPortName);
        srcPort.setCna(srcPortCna);
        srcDev.setPorts(Collections.singletonList(srcPort));
        devices.put(srcDeviceId, srcDev);

        // 目的设备
        DeviceEntity dstDev = new DeviceEntity();
        dstDev.setDeviceId(dstDeviceId);
        PortEntity dstPort = new PortEntity();
        dstPort.setName(dstPortName);
        dstPort.setCna(dstPortCna);
        dstDev.setPorts(Collections.singletonList(dstPort));
        devices.put(dstDeviceId, dstDev);

        topo.setDevices(devices);
        return topo;
    }

    // ==================== 1. null 参数安全返回 ====================

    @Nested
    @DisplayName("null 参数安全返回")
    class NullParamTest {

        @Test
        @DisplayName("aclData 为 null → 直接返回 result 不变")
        void testAclDataNull() {
            PathPlanResult result = createResult("EID1", "EID2", "dev1", "port1", "dev2", "port2");
            result.setStatus(PlanStatus.SUCCESS);
            TopoData topo = createTopo("dev1", "port1", "0xAAAA", "dev2", "port2", "0xBBBB");

            PathPlanResult returned = engine.checkPath(null, result, topo);
            assertSame(result, returned);
            assertEquals(PlanStatus.SUCCESS, returned.getStatus());
        }

        @Test
        @DisplayName("result 为 null → 返回 null")
        void testResultNull() {
            TopoData topo = createTopo("dev1", "port1", "0xAAAA", "dev2", "port2", "0xBBBB");
            AclData aclData = new AclData();

            PathPlanResult returned = engine.checkPath(aclData, null, topo);
            assertNull(returned);
        }

        @Test
        @DisplayName("topoData 为 null → 直接返回 result 不变")
        void testTopoDataNull() {
            PathPlanResult result = createResult("EID1", "EID2", "dev1", "port1", "dev2", "port2");
            result.setStatus(PlanStatus.SUCCESS);
            AclData aclData = new AclData();

            PathPlanResult returned = engine.checkPath(aclData, result, null);
            assertSame(result, returned);
            assertEquals(PlanStatus.SUCCESS, returned.getStatus());
        }
    }

    // ==================== 2. 前置条件短路 ====================

    @Nested
    @DisplayName("前置条件短路返回")
    class ShortCircuitTest {

        @Test
        @DisplayName("result.status 为非 SUCCESS 且非 null → 直接返回")
        void testStatusNotSuccess() {
            PathPlanResult result = new PathPlanResult();
            result.setStatus(PlanStatus.INVALID_PARAM);
            TopoData topo = createTopo("dev1", "port1", "0xAAAA", "dev2", "port2", "0xBBBB");
            AclData aclData = new AclData();

            PathPlanResult returned = engine.checkPath(aclData, result, topo);
            assertSame(result, returned);
            assertEquals(PlanStatus.INVALID_PARAM, returned.getStatus());
        }

        @Test
        @DisplayName("tpAcls 为 null → 直接返回")
        void testTpAclsNull() {
            PathPlanResult result = createResult("EID1", "EID2", "dev1", "port1", "dev2", "port2");
            TopoData topo = createTopo("dev1", "port1", "0xAAAA", "dev2", "port2", "0xBBBB");
            AclData aclData = new AclData();
            aclData.setTpAcls(null);

            PathPlanResult returned = engine.checkPath(aclData, result, topo);
            assertEquals(PlanStatus.SUCCESS, returned.getStatus());
        }

        @Test
        @DisplayName("tpAcls 为空 Map → 直接返回")
        void testTpAclsEmpty() {
            PathPlanResult result = createResult("EID1", "EID2", "dev1", "port1", "dev2", "port2");
            TopoData topo = createTopo("dev1", "port1", "0xAAAA", "dev2", "port2", "0xBBBB");
            AclData aclData = new AclData();
            aclData.setTpAcls(new LinkedHashMap<>());

            PathPlanResult returned = engine.checkPath(aclData, result, topo);
            assertEquals(PlanStatus.SUCCESS, returned.getStatus());
        }

        @Test
        @DisplayName("srcEid 为 null → 直接返回")
        void testSrcEidNull() {
            PathPlanResult result = createResult("EID1", "EID2", "dev1", "port1", "dev2", "port2");
            result.setSourceEid(null);
            TopoData topo = createTopo("dev1", "port1", "0xAAAA", "dev2", "port2", "0xBBBB");
            AclData aclData = new AclData();
            aclData.setTpAcls(new LinkedHashMap<>());

            PathPlanResult returned = engine.checkPath(aclData, result, topo);
            assertEquals(PlanStatus.SUCCESS, returned.getStatus());
        }

        @Test
        @DisplayName("dstEid 为 null → 直接返回")
        void testDstEidNull() {
            PathPlanResult result = createResult("EID1", "EID2", "dev1", "port1", "dev2", "port2");
            result.setDestEid(null);
            TopoData topo = createTopo("dev1", "port1", "0xAAAA", "dev2", "port2", "0xBBBB");
            AclData aclData = new AclData();
            aclData.setTpAcls(new LinkedHashMap<>());

            PathPlanResult returned = engine.checkPath(aclData, result, topo);
            assertEquals(PlanStatus.SUCCESS, returned.getStatus());
        }
    }

    // ==================== 3. ACL 匹配主逻辑 ====================

    @Nested
    @DisplayName("ACL 匹配主逻辑")
    class AclMatchTest {

        @Test
        @DisplayName("ACL 规则 CNA 完全匹配 → 不改变 status（保持 SUCCESS）")
        void testCnaMatchSuccess() {
            PathPlanResult result = createResult("EID1", "EID2", "dev1", "port1", "dev2", "port2");
            TopoData topo = createTopo("dev1", "port1", "0xAAAA1111", "dev2", "port2", "0xDDDD4444");

            AclData aclData = new AclData();
            Map<AclKey, TpAclEntity> tpAcls = new LinkedHashMap<>();
            TpAclEntity rule = createRule("EID1", "EID2", "0xAAAA1111", "0xDDDD4444");
            tpAcls.put(new AclKey("EID1", "EID2", TransportType.RTP), rule);
            aclData.setTpAcls(tpAcls);

            PathPlanResult returned = engine.checkPath(aclData, result, topo);
            assertEquals(PlanStatus.SUCCESS, returned.getStatus());
        }

        @Test
        @DisplayName("ACL 规则 EID 不匹配 → 跳过（最终 ACL_NOT_FOUND）")
        void testEidNotMatch() {
            PathPlanResult result = createResult("EID1", "EID2", "dev1", "port1", "dev2", "port2");
            TopoData topo = createTopo("dev1", "port1", "0xAAAA1111", "dev2", "port2", "0xDDDD4444");

            AclData aclData = new AclData();
            Map<AclKey, TpAclEntity> tpAcls = new LinkedHashMap<>();
            // 规则中 EID 与 result 中 EID 不匹配
            TpAclEntity rule = createRule("WRONG_EID", "EID2", "0xAAAA1111", "0xDDDD4444");
            tpAcls.put(new AclKey("WRONG_EID", "EID2", TransportType.RTP), rule);
            aclData.setTpAcls(tpAcls);

            PathPlanResult returned = engine.checkPath(aclData, result, topo);
            assertEquals(PlanStatus.ACL_NOT_FOUND, returned.getStatus());
        }

        @Test
        @DisplayName("ACL 找到但源 CNA 不匹配 → ACL_DENIED")
        void testSrcCnaMismatch() {
            PathPlanResult result = createResult("EID1", "EID2", "dev1", "port1", "dev2", "port2");
            TopoData topo = createTopo("dev1", "port1", "0xAAAA1111", "dev2", "port2", "0xDDDD4444");

            AclData aclData = new AclData();
            Map<AclKey, TpAclEntity> tpAcls = new LinkedHashMap<>();
            // 源 CNA 用错误值
            TpAclEntity rule = createRule("EID1", "EID2", "0x9999999", "0xDDDD4444");
            tpAcls.put(new AclKey("EID1", "EID2", TransportType.RTP), rule);
            aclData.setTpAcls(tpAcls);

            PathPlanResult returned = engine.checkPath(aclData, result, topo);
            assertEquals(PlanStatus.ACL_DENIED, returned.getStatus());
            assertNotNull(returned.getErrorMessage());
            assertTrue(returned.getErrorMessage().contains("CNA mismatch"));
        }

        @Test
        @DisplayName("ACL 找到但目的 CNA 不匹配 → ACL_DENIED")
        void testDstCnaMismatch() {
            PathPlanResult result = createResult("EID1", "EID2", "dev1", "port1", "dev2", "port2");
            TopoData topo = createTopo("dev1", "port1", "0xAAAA1111", "dev2", "port2", "0xDDDD4444");

            AclData aclData = new AclData();
            Map<AclKey, TpAclEntity> tpAcls = new LinkedHashMap<>();
            // 目的 CNA 用错误值
            TpAclEntity rule = createRule("EID1", "EID2", "0xAAAA1111", "0x9999999");
            tpAcls.put(new AclKey("EID1", "EID2", TransportType.RTP), rule);
            aclData.setTpAcls(tpAcls);

            PathPlanResult returned = engine.checkPath(aclData, result, topo);
            assertEquals(PlanStatus.ACL_DENIED, returned.getStatus());
        }

        @Test
        @DisplayName("tpAcls 中 value 为 null → 跳过")
        void testRuleValueNull() {
            PathPlanResult result = createResult("EID1", "EID2", "dev1", "port1", "dev2", "port2");
            TopoData topo = createTopo("dev1", "port1", "0xAAAA1111", "dev2", "port2", "0xDDDD4444");

            AclData aclData = new AclData();
            Map<AclKey, TpAclEntity> tpAcls = new LinkedHashMap<>();
            // 存一条 null value
            tpAcls.put(new AclKey("EID1", "EID2", TransportType.RTP), null);
            // 再加一条不匹配的
            TpAclEntity rule2 = createRule("WRONG", "WRONG", "0xAAAA1111", "0xDDDD4444");
            tpAcls.put(new AclKey("WRONG", "WRONG", TransportType.RTP), rule2);
            aclData.setTpAcls(tpAcls);

            PathPlanResult returned = engine.checkPath(aclData, result, topo);
            // null 被跳过，rule2 EID 不匹配，最终 ACL_NOT_FOUND
            assertEquals(PlanStatus.ACL_NOT_FOUND, returned.getStatus());
        }

        @Test
        @DisplayName("ACL 完全找不到 → ACL_NOT_FOUND 带 errorMessage")
        void testAclNotFound() {
            PathPlanResult result = createResult("EID1", "EID2", "dev1", "port1", "dev2", "port2");
            TopoData topo = createTopo("dev1", "port1", "0xAAAA1111", "dev2", "port2", "0xDDDD4444");

            AclData aclData = new AclData();
            Map<AclKey, TpAclEntity> tpAcls = new LinkedHashMap<>();
            TpAclEntity rule = createRule("OTHER1", "OTHER2", "0xAAAA1111", "0xDDDD4444");
            tpAcls.put(new AclKey("OTHER1", "OTHER2", TransportType.RTP), rule);
            aclData.setTpAcls(tpAcls);

            PathPlanResult returned = engine.checkPath(aclData, result, topo);
            assertEquals(PlanStatus.ACL_NOT_FOUND, returned.getStatus());
            assertEquals(PlanStatus.ACL_NOT_FOUND.getCode(), returned.getErrorCode());
            assertTrue(returned.getErrorMessage().contains("ACL rule not found"));
            assertTrue(returned.getErrorMessage().contains("EID1"));
            assertTrue(returned.getErrorMessage().contains("EID2"));
        }

        @Test
        @DisplayName("CNA 忽略大小写匹配 → SUCCESS")
        void testCnaCaseInsensitiveMatch() {
            PathPlanResult result = createResult("EID1", "EID2", "dev1", "port1", "dev2", "port2");
            // topo 中用大写 CNA
            TopoData topo = createTopo("dev1", "port1", "0XAAAA1111", "dev2", "port2", "0XDDDD4444");

            AclData aclData = new AclData();
            Map<AclKey, TpAclEntity> tpAcls = new LinkedHashMap<>();
            // 规则用小写 cna
            TpAclEntity rule = createRule("EID1", "EID2", "0xaaaa1111", "0xdddd4444");
            tpAcls.put(new AclKey("EID1", "EID2", TransportType.RTP), rule);
            aclData.setTpAcls(tpAcls);

            PathPlanResult returned = engine.checkPath(aclData, result, topo);
            assertEquals(PlanStatus.SUCCESS, returned.getStatus());
        }
    }

    // ==================== 4. 私有方法间接覆盖（通过 checkPath 触发）====================

    @Nested
    @DisplayName("findSrcPortCna / findDstPortCna 边界（通过 checkPath 间接覆盖）")
    class PrivateMethodTest {

        @Test
        @DisplayName("paths 为 null 时 CNA 查找返回 null → ACL_NOT_FOUND")
        void testPathsNull() {
            PathPlanResult result = new PathPlanResult();
            result.setStatus(PlanStatus.SUCCESS);
            result.setSourceEid("EID1");
            result.setDestEid("EID2");
            result.setPaths(null);

            TopoData topo = createTopo("dev1", "port1", "0xAAAA1111", "dev2", "port2", "0xDDDD4444");

            AclData aclData = new AclData();
            Map<AclKey, TpAclEntity> tpAcls = new LinkedHashMap<>();
            TpAclEntity rule = createRule("EID1", "EID2", "0xAAAA1111", "0xDDDD4444");
            tpAcls.put(new AclKey("EID1", "EID2", TransportType.RTP), rule);
            aclData.setTpAcls(tpAcls);

            PathPlanResult returned = engine.checkPath(aclData, result, topo);
            // CNA 查找返回 null（paths=null），不等，走 ACL_DENIED
            assertEquals(PlanStatus.ACL_DENIED, returned.getStatus());
        }

        @Test
        @DisplayName("paths 为空列表 → ACL_DENIED（CNA 为 null 不匹配）")
        void testPathsEmpty() {
            PathPlanResult result = new PathPlanResult();
            result.setStatus(PlanStatus.SUCCESS);
            result.setSourceEid("EID1");
            result.setDestEid("EID2");
            result.setPaths(Collections.emptyList());

            TopoData topo = createTopo("dev1", "port1", "0xAAAA1111", "dev2", "port2", "0xDDDD4444");

            AclData aclData = new AclData();
            Map<AclKey, TpAclEntity> tpAcls = new LinkedHashMap<>();
            TpAclEntity rule = createRule("EID1", "EID2", "0xAAAA1111", "0xDDDD4444");
            tpAcls.put(new AclKey("EID1", "EID2", TransportType.RTP), rule);
            aclData.setTpAcls(tpAcls);

            PathPlanResult returned = engine.checkPath(aclData, result, topo);
            assertEquals(PlanStatus.ACL_DENIED, returned.getStatus());
        }

        @Test
        @DisplayName("hops 为空列表 → ACL_DENIED（CNA 为 null 不匹配）")
        void testHopsEmpty() {
            PathPlanResult result = new PathPlanResult();
            result.setStatus(PlanStatus.SUCCESS);
            result.setSourceEid("EID1");
            result.setDestEid("EID2");

            PathInfo path = new PathInfo();
            path.setHops(Collections.emptyList());
            result.setPaths(Collections.singletonList(path));

            TopoData topo = createTopo("dev1", "port1", "0xAAAA1111", "dev2", "port2", "0xDDDD4444");

            AclData aclData = new AclData();
            Map<AclKey, TpAclEntity> tpAcls = new LinkedHashMap<>();
            TpAclEntity rule = createRule("EID1", "EID2", "0xAAAA1111", "0xDDDD4444");
            tpAcls.put(new AclKey("EID1", "EID2", TransportType.RTP), rule);
            aclData.setTpAcls(tpAcls);

            PathPlanResult returned = engine.checkPath(aclData, result, topo);
            assertEquals(PlanStatus.ACL_DENIED, returned.getStatus());
        }

        @Test
        @DisplayName("设备 ID 在拓扑中不存在 → findPortCna 返回 null → ACL_DENIED")
        void testDeviceNotFoundInTopo() {
            PathPlanResult result = createResult("EID1", "EID2", "dev_null", "port1", "dev2", "port2");
            TopoData topo = createTopo("dev1", "port1", "0xAAAA1111", "dev2", "port2", "0xDDDD4444");

            AclData aclData = new AclData();
            Map<AclKey, TpAclEntity> tpAcls = new LinkedHashMap<>();
            TpAclEntity rule = createRule("EID1", "EID2", "0xAAAA1111", "0xDDDD4444");
            tpAcls.put(new AclKey("EID1", "EID2", TransportType.RTP), rule);
            aclData.setTpAcls(tpAcls);

            PathPlanResult returned = engine.checkPath(aclData, result, topo);
            assertEquals(PlanStatus.ACL_DENIED, returned.getStatus());
        }

        @Test
        @DisplayName("端口名在设备上不存在 → findPortCna 返回 null → ACL_DENIED")
        void testPortNotFoundOnDevice() {
            PathPlanResult result = createResult("EID1", "EID2", "dev1", "non_exist_port", "dev2", "port2");
            TopoData topo = createTopo("dev1", "port1", "0xAAAA1111", "dev2", "port2", "0xDDDD4444");

            AclData aclData = new AclData();
            Map<AclKey, TpAclEntity> tpAcls = new LinkedHashMap<>();
            TpAclEntity rule = createRule("EID1", "EID2", "0xAAAA1111", "0xDDDD4444");
            tpAcls.put(new AclKey("EID1", "EID2", TransportType.RTP), rule);
            aclData.setTpAcls(tpAcls);

            PathPlanResult returned = engine.checkPath(aclData, result, topo);
            assertEquals(PlanStatus.ACL_DENIED, returned.getStatus());
        }

        @Test
        @DisplayName("目的设备 inPort 为 null 时回退到 outPort → 匹配 CNA")
        void testDstInPortNullFallback() {
            PathPlanResult result = new PathPlanResult();
            result.setStatus(PlanStatus.SUCCESS);
            result.setSourceEid("EID1");
            result.setDestEid("EID2");

            HopInfo srcHop = new HopInfo();
            srcHop.setDeviceId("dev1");
            srcHop.setOutPort("port1");

            HopInfo dstHop = new HopInfo();
            dstHop.setDeviceId("dev2");
            dstHop.setOutPort("port2");
            dstHop.setInPort(null); // inPort 为 null，应回退用 outPort

            PathInfo path = new PathInfo();
            path.setHops(Arrays.asList(srcHop, dstHop));
            result.setPaths(Collections.singletonList(path));

            TopoData topo = createTopo("dev1", "port1", "0xAAAA1111", "dev2", "port2", "0xDDDD4444");

            AclData aclData = new AclData();
            Map<AclKey, TpAclEntity> tpAcls = new LinkedHashMap<>();
            TpAclEntity rule = createRule("EID1", "EID2", "0xAAAA1111", "0xDDDD4444");
            tpAcls.put(new AclKey("EID1", "EID2", TransportType.RTP), rule);
            aclData.setTpAcls(tpAcls);

            PathPlanResult returned = engine.checkPath(aclData, result, topo);
            assertEquals(PlanStatus.SUCCESS, returned.getStatus());
        }

        @Test
        @DisplayName("目的设备 inPort 为空字符串时回退到 outPort → 匹配 CNA")
        void testDstInPortEmptyFallback() {
            PathPlanResult result = new PathPlanResult();
            result.setStatus(PlanStatus.SUCCESS);
            result.setSourceEid("EID1");
            result.setDestEid("EID2");

            HopInfo srcHop = new HopInfo();
            srcHop.setDeviceId("dev1");
            srcHop.setOutPort("port1");

            HopInfo dstHop = new HopInfo();
            dstHop.setDeviceId("dev2");
            dstHop.setOutPort("port2");
            dstHop.setInPort(""); // 空 inPort，应回退用 outPort

            PathInfo path = new PathInfo();
            path.setHops(Arrays.asList(srcHop, dstHop));
            result.setPaths(Collections.singletonList(path));

            TopoData topo = createTopo("dev1", "port1", "0xAAAA1111", "dev2", "port2", "0xDDDD4444");

            AclData aclData = new AclData();
            Map<AclKey, TpAclEntity> tpAcls = new LinkedHashMap<>();
            TpAclEntity rule = createRule("EID1", "EID2", "0xAAAA1111", "0xDDDD4444");
            tpAcls.put(new AclKey("EID1", "EID2", TransportType.RTP), rule);
            aclData.setTpAcls(tpAcls);

            PathPlanResult returned = engine.checkPath(aclData, result, topo);
            assertEquals(PlanStatus.SUCCESS, returned.getStatus());
        }
    }
}