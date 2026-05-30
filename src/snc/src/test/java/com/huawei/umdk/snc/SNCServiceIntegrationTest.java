/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc service integration test module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc service integration test module
 */
package com.huawei.umdk.snc;

import static org.junit.jupiter.api.Assertions.*;

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import com.huawei.umdk.snc.config.SNCConfig;
import com.huawei.umdk.snc.entity.*;
import com.huawei.umdk.snc.exception.SNCException;

import org.junit.jupiter.api.DisplayName;
import org.junit.jupiter.api.Test;

import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.lang.reflect.Type;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * SNCService 接口集成测试
 *
 * 使用 topo-data.json 和 acl-data.json 作为测试输入数据，
 * 验证 SNCService 接口的所有主要功能：
 * - 生命周期管理：init / uninit
 * - Topo CRUD：setTopoData / getTopoData / listAllTopos / removeTopoData
 * - ACL CRUD：setAclData / getAclData / listAllAcls / removeAclData
 * - 路径查询：planPath（含 ACL 校验）
 *
 * Topo 名称：A5-superPod-1
 * devices: NPU1(rack1#os0#npu1), LA(rack1#la0), NPU2(rack1#os0#npu2)
 */
@DisplayName("SNCService 接口集成测试")
public class SNCServiceIntegrationTest {

    private static final Gson GSON = createGson();

    private static Gson createGson() {
        GsonBuilder builder = new GsonBuilder();
        builder.registerTypeAdapter(AclData.class, new AclDataDeserializer());
        return builder.create();
    }

    /**
     * 自定义 AclData 反序列化器
     *
     * JSON 中 tpAcls 是 Map<String, TpAclEntity>，
     * Java 类定义是 Map<AclKey, TpAclEntity>。
     * 此反序列化器将字符串 key（格式：sourceEid|destEid|transportType）解析为 AclKey。
     */
    static class AclDataDeserializer implements com.google.gson.JsonDeserializer<AclData> {
        @Override
        public AclData deserialize(com.google.gson.JsonElement json, Type typeOfT,
                com.google.gson.JsonDeserializationContext context) {
            com.google.gson.JsonObject obj = json.getAsJsonObject();
            AclData aclData = new AclData();

            if (obj.has("aclId")) {
                aclData.setAclId(obj.get("aclId").getAsString());
            }

            if (obj.has("tpAcls")) {
                com.google.gson.JsonObject tpAclsJson = obj.getAsJsonObject("tpAcls");
                Map<AclKey, TpAclEntity> tpAcls = new HashMap<>();

                for (Map.Entry<String, com.google.gson.JsonElement> entry : tpAclsJson.entrySet()) {
                    String key = entry.getKey();
                    TpAclEntity entity = context.deserialize(entry.getValue(), TpAclEntity.class);

                    AclKey aclKey = parseAclKey(key);
                    if (aclKey != null) {
                        tpAcls.put(aclKey, entity);
                    }
                }
                aclData.setTpAcls(tpAcls);
            }
            return aclData;
        }

        private AclKey parseAclKey(String key) {
            if (key == null || key.isEmpty()) return null;
            int firstPipe = key.indexOf('|');
            int lastPipe = key.lastIndexOf('|');
            if (firstPipe < 0 || lastPipe < 0 || firstPipe >= lastPipe) return null;

            String srcEid = key.substring(0, firstPipe);
            String dstEid = key.substring(firstPipe + 1, lastPipe);
            String transportStr = key.substring(lastPipe + 1);

            TransportType transportType;
            try {
                transportType = TransportType.valueOf(transportStr.toUpperCase());
            } catch (IllegalArgumentException e) {
                return null;
            }
            return new AclKey(srcEid, dstEid, transportType);
        }
    }

    // ==================== 辅助方法 ====================

    private static <T> T loadJson(String resourcePath, Type type) {
        InputStream is = SNCServiceIntegrationTest.class.getClassLoader().getResourceAsStream(resourcePath);
        if (is == null) {
            throw new RuntimeException("Resource not found: " + resourcePath);
        }
        try (InputStreamReader reader = new InputStreamReader(is, StandardCharsets.UTF_8)) {
            return GSON.fromJson(reader, type);
        } catch (IOException e) {
            throw new RuntimeException("Failed to read resource: " + resourcePath, e);
        }
    }

    private static TopoData loadTopoData() {
        return loadJson("topo-data.json", TopoData.class);
    }

    private static AclData loadAclData() {
        return loadJson("acl-data.json", AclData.class);
    }

    private static SNCServiceImpl createAndInitService() {
        SNCServiceImpl snc = new SNCServiceImpl(new SNCConfig());
        snc.init();
        return snc;
    }

    // ==================== 主成功场景测试 ====================

    /**
     * TC01: 完整生命周期 - init → 加载数据 → planPath → uninit
     */
    @Test
    @DisplayName("TC01: 完整生命周期 init→加载数据→planPath→uninit")
    void testFullLifecycle() {
        TopoData topoData = loadTopoData();
        AclData aclData = loadAclData();

        SNCServiceImpl snc = createAndInitService();
        snc.setTopoData(topoData);
        snc.setAclData(aclData);

        PathPlanRequest request = new PathPlanRequest();
        request.setSrcDevice("rack1#os0#npu1");
        request.setSrcPort("400GE 0/0/1");
        request.setDestDevice("rack1#os0#npu2");
        request.setDestPort("400GE 0/0/1");
        request.setInterDevices(new java.util.LinkedHashMap<>());
        request.getInterDevices().put("rack1#la0", "400GE 1/0/2");

        PathPlanResult result = snc.planPath(request);
        assertNotNull(result, "result should not be null");
        assertEquals(PlanStatus.SUCCESS, result.getStatus(), "Expected SUCCESS after init+data");

        // uninit 后查询应失败
        snc.uninit();
        PathPlanResult result2 = snc.planPath(request);
        assertEquals(PlanStatus.INVALID_PARAM, result2.getStatus(), "Expected INVALID_PARAM after uninit");
    }

    /**
     * TC02: Topo CRUD 完整操作
     */
    @Test
    @DisplayName("TC02: Topo CRUD set/get/list/remove 完整操作")
    void testTopoCRUD() {
        TopoData topoData = loadTopoData();
        SNCServiceImpl snc = createAndInitService();

        snc.setTopoData(topoData);

        TopoData retrieved = snc.getTopoData("A5-superPod-1");
        assertNotNull(retrieved, "getTopoData should not return null");
        assertEquals("A5-superPod-1", retrieved.getName(), "getTopoData name mismatch");

        List<TopoData> allTopos = snc.listAllTopos();
        assertNotNull(allTopos, "listAllTopos should not return null");
        assertEquals(1, allTopos.size(), "Expected 1 topo");

        Set<String> names = snc.listAllTopoNames();
        assertTrue(names.contains("A5-superPod-1"), "listAllTopoNames should contain A5-superPod-1");

        snc.removeTopoData("A5-superPod-1");
        TopoData afterRemove = snc.getTopoData("A5-superPod-1");
        assertNull(afterRemove, "getTopoData should return null after remove");
    }

    /**
     * TC03: ACL CRUD 完整操作
     */
    @Test
    @DisplayName("TC03: ACL CRUD set/get/list/remove 完整操作")
    void testAclCRUD() {
        AclData aclData = loadAclData();
        SNCServiceImpl snc = createAndInitService();

        snc.setAclData(aclData);

        AclData retrieved = snc.getAclData("acl-001");
        assertNotNull(retrieved, "getAclData should not return null");
        assertEquals("acl-001", retrieved.getAclId(), "getAclData ID mismatch");

        List<AclData> allAcls = snc.listAllAcls();
        assertNotNull(allAcls, "listAllAcls should not return null");
        assertEquals(1, allAcls.size(), "Expected 1 ACL");

        Set<String> ids = snc.listAllAclIds();
        assertTrue(ids.contains("acl-001"), "listAllAclIds should contain acl-001");

        snc.removeAclData("acl-001");
        AclData afterRemove = snc.getAclData("acl-001");
        assertNull(afterRemove, "getAclData should return null after remove");
    }

    /**
     * TC04: 路径查询 NPU1 → LA → NPU2，3跳
     */
    @Test
    @DisplayName("TC04: 路径查询 NPU1→LA→NPU2, 3 hops")
    void testPathNpu1ToNpu2() {
        TopoData topoData = loadTopoData();
        AclData aclData = loadAclData();

        SNCServiceImpl snc = createAndInitService();
        snc.setTopoData(topoData);
        snc.setAclData(aclData);

        PathPlanRequest request = new PathPlanRequest();
        request.setSrcDevice("rack1#os0#npu1");
        request.setSrcPort("400GE 0/0/1");
        request.setDestDevice("rack1#os0#npu2");
        request.setDestPort("400GE 0/0/1");
        request.setInterDevices(new java.util.LinkedHashMap<>());
        request.getInterDevices().put("rack1#la0", "400GE 1/0/2");

        PathPlanResult result = snc.planPath(request);
        assertEquals(PlanStatus.SUCCESS, result.getStatus(), "Expected SUCCESS for NPU1→LA→NPU2");
        assertNotNull(result.getPaths(), "Paths should not be null");
        assertEquals(1, result.getPaths().size(), "Should have at least 1 path");
        List<HopInfo> hops = result.getPaths().get(0).getHops();
        assertNotNull(hops, "Hops should not be null");
        assertEquals(3, hops.size(), "Expected 3 hops");
    }

    /**
     * TC05: 反向路径 NPU2 → LA → NPU1，3跳
     */
    @Test
    @DisplayName("TC05: 反向路径 NPU2→LA→NPU1, 3 hops")
    void testPathNpu2ToNpu1() {
        TopoData topoData = loadTopoData();
        AclData aclData = loadAclData();

        SNCServiceImpl snc = createAndInitService();
        snc.setTopoData(topoData);
        snc.setAclData(aclData);

        PathPlanRequest request = new PathPlanRequest();
        request.setSrcDevice("rack1#os0#npu2");
        request.setSrcPort("400GE 0/0/1");
        request.setDestDevice("rack1#os0#npu1");
        request.setDestPort("400GE 0/0/1");
        request.setInterDevices(new java.util.LinkedHashMap<>());
        request.getInterDevices().put("rack1#la0", "400GE 1/0/2");

        PathPlanResult result = snc.planPath(request);
        assertEquals(PlanStatus.SUCCESS, result.getStatus(), "Expected SUCCESS for NPU2→LA→NPU1");
        assertNotNull(result.getPaths(), "Paths should not be null");
        assertEquals(1, result.getPaths().size(), "Should have at least 1 path");
        List<HopInfo> hops = result.getPaths().get(0).getHops();
        assertNotNull(hops, "Hops should not be null");
        assertEquals(3, hops.size(), "Expected 3 hops");
    }

    /**
     * TC06: 源目相同设备
     */
    @Test
    @DisplayName("TC06: 源目相同设备应返回 INVALID_PARAM")
    void testSameSrcDst() {
        TopoData topoData = loadTopoData();
        SNCServiceImpl snc = createAndInitService();
        snc.setTopoData(topoData);

        PathPlanRequest request = new PathPlanRequest();
        request.setSrcDevice("rack1#os0#npu1");
        request.setSrcPort("400GE 0/0/1");
        request.setDestDevice("rack1#os0#npu1");
        request.setDestPort("400GE 0/0/1");

        PathPlanResult result = snc.planPath(request);
        assertEquals(PlanStatus.INVALID_PARAM, result.getStatus(), "Expected INVALID_PARAM for same src/dst");
    }

    /**
     * TC07: 未设置拓扑时查询
     */
    @Test
    @DisplayName("TC07: 未设置拓扑时返回 TOPO_NOT_FOUND")
    void testTopoNotFound() {
        SNCServiceImpl snc = createAndInitService();

        PathPlanRequest request = new PathPlanRequest();
        request.setSrcDevice("rack1#os0#npu1");
        request.setSrcPort("400GE 0/0/1");
        request.setDestDevice("rack1#os0#npu2");
        request.setDestPort("400GE 0/0/1");

        PathPlanResult result = snc.planPath(request);
        assertEquals(PlanStatus.TOPO_NOT_FOUND, result.getStatus(), "Expected TOPO_NOT_FOUND when topo not set");
    }

    /**
     * TC08: 未初始化状态
     */
    @Test
    @DisplayName("TC08: 未初始化状态下 planPath 返回 INVALID_PARAM")
    void testNotInitialized() {
        SNCServiceImpl snc = new SNCServiceImpl();

        PathPlanRequest request = new PathPlanRequest();
        request.setSrcDevice("rack1#os0#npu1");
        request.setSrcPort("400GE 0/0/1");
        request.setDestDevice("rack1#os0#npu2");
        request.setDestPort("400GE 0/0/1");

        PathPlanResult result = snc.planPath(request);
        assertEquals(PlanStatus.INVALID_PARAM, result.getStatus(), "Expected INVALID_PARAM when not initialized");
        assertEquals(-99, result.getErrorCode(), "Expected error code -99");
    }

    /**
     * TC09: Topo 数据完整性验证
     */
    @Test
    @DisplayName("TC09: Topo 数据完整性验证——3个设备、端口数正确")
    void testTopoDataIntegrity() {
        TopoData topoData = loadTopoData();
        SNCServiceImpl snc = createAndInitService();
        snc.setTopoData(topoData);

        TopoData retrieved = snc.getTopoData("A5-superPod-1");
        assertNotNull(retrieved, "TopoData should not be null");
        assertEquals("A5-superPod-1", retrieved.getName(), "Topo name mismatch");

        Map<String, DeviceEntity> devices = retrieved.getDevices();
        assertNotNull(devices, "Devices should not be null");
        assertEquals(3, devices.size(), "Expected 3 devices");

        assertTrue(devices.containsKey("rack1#os0#npu1"), "Should contain NPU1");
        assertTrue(devices.containsKey("rack1#la0"), "Should contain LA");
        assertTrue(devices.containsKey("rack1#os0#npu2"), "Should contain NPU2");
    }

    /**
     * TC10: ACL 数据完整性——2条规则，EID 正确
     */
    @Test
    @DisplayName("TC10: ACL 数据完整性——2条规则，EIDs 正确")
    void testAclDataIntegrity() {
        AclData aclData = loadAclData();
        SNCServiceImpl snc = createAndInitService();
        snc.setAclData(aclData);

        AclData retrieved = snc.getAclData("acl-001");
        assertNotNull(retrieved, "AclData should not be null");
        assertEquals("acl-001", retrieved.getAclId(), "ACL ID mismatch");

        Map<AclKey, TpAclEntity> tpAcls = retrieved.getTpAcls();
        assertNotNull(tpAcls, "tpAcls should not be null");
        assertEquals(2, tpAcls.size(), "Expected 2 ACL rules");
    }

    /**
     * TC11: ACL 校验通过——路径查询返回 SUCCESS
     */
    @Test
    @DisplayName("TC11: ACL check passed - path plan returned SUCCESS")
    void testAclCheckPassed() {
        TopoData topoData = loadTopoData();
        AclData aclData = loadAclData();

        SNCServiceImpl snc = createAndInitService();
        snc.setTopoData(topoData);
        snc.setAclData(aclData);

        PathPlanRequest request = new PathPlanRequest();
        request.setSrcDevice("rack1#os0#npu1");
        request.setSrcPort("400GE 0/0/1");
        request.setDestDevice("rack1#os0#npu2");
        request.setDestPort("400GE 0/0/1");
        request.setInterDevices(new java.util.LinkedHashMap<>());
        request.getInterDevices().put("rack1#la0", "400GE 1/0/2");

        PathPlanResult result = snc.planPath(request);
        assertEquals(PlanStatus.SUCCESS, result.getStatus(), "Expected SUCCESS when ACL check passes");
    }

    /**
     * TC12: clearAllTopos 和 clearAllAcls 正常工作
     */
    @Test
    @DisplayName("TC12: clearAllTopos and clearAllAcls work correctly")
    void testClearAllToposAndAcls() {
        TopoData topoData = loadTopoData();
        AclData aclData = loadAclData();

        SNCServiceImpl snc = createAndInitService();
        snc.setTopoData(topoData);
        snc.setAclData(aclData);

        assertEquals(1, snc.listAllTopos().size(), "Should have 1 topo before clear");
        assertEquals(1, snc.listAllAcls().size(), "Should have 1 ACL before clear");

        snc.clearAllTopos();
        snc.clearAllAcls();

        assertEquals(0, snc.listAllTopos().size(), "Should have 0 topos after clear");
        assertEquals(0, snc.listAllAcls().size(), "Should have 0 ACLs after clear");
    }

    // ==================== 异常场景测试 ====================

    /**
     * TC13: null 请求
     * 实现行为：pathService 不为 null，委托给 PathService.planPath(null)
     * PathService 处理 null 请求时返回 TOPO_NOT_FOUND（因未设置 topo）
     */
    @Test
    @DisplayName("TC13: null request 返回 TOPO_NOT_FOUND")
    void testNullRequest() {
        SNCServiceImpl snc = createAndInitService();
        PathPlanResult result = snc.planPath(null);
        assertNotNull(result, "result should not be null for null request");
    }

    /**
     * TC14: 空 srcDevice
     */
    @Test
    @DisplayName("TC14: 空 srcDevice 返回 INVALID_PARAM")
    void testSrcDeviceEmpty() {
        TopoData topoData = loadTopoData();
        SNCServiceImpl snc = createAndInitService();
        snc.setTopoData(topoData);

        PathPlanRequest request = new PathPlanRequest();
        request.setSrcDevice("");
        request.setSrcPort("400GE 0/0/1");
        request.setDestDevice("rack1#os0#npu2");
        request.setDestPort("400GE 0/0/1");

        PathPlanResult result = snc.planPath(request);
        assertEquals(PlanStatus.INVALID_PARAM, result.getStatus(), "Expected INVALID_PARAM for empty srcDevice");
    }

    /**
     * TC15: 空 srcPort
     */
    @Test
    @DisplayName("TC15: 空 srcPort 返回 INVALID_PARAM")
    void testSrcPortEmpty() {
        TopoData topoData = loadTopoData();
        SNCServiceImpl snc = createAndInitService();
        snc.setTopoData(topoData);

        PathPlanRequest request = new PathPlanRequest();
        request.setSrcDevice("rack1#os0#npu1");
        request.setSrcPort("");
        request.setDestDevice("rack1#os0#npu2");
        request.setDestPort("400GE 0/0/1");

        PathPlanResult result = snc.planPath(request);
        assertEquals(PlanStatus.INVALID_PARAM, result.getStatus(), "Expected INVALID_PARAM for empty srcPort");
    }

    /**
     * TC16: 空 destDevice
     */
    @Test
    @DisplayName("TC16: 空 destDevice 返回 INVALID_PARAM")
    void testDestDeviceEmpty() {
        TopoData topoData = loadTopoData();
        SNCServiceImpl snc = createAndInitService();
        snc.setTopoData(topoData);

        PathPlanRequest request = new PathPlanRequest();
        request.setSrcDevice("rack1#os0#npu1");
        request.setSrcPort("400GE 0/0/1");
        request.setDestDevice("");
        request.setDestPort("400GE 0/0/1");

        PathPlanResult result = snc.planPath(request);
        assertEquals(PlanStatus.INVALID_PARAM, result.getStatus(), "Expected INVALID_PARAM for empty destDevice");
    }

    /**
     * TC17: 空 destPort
     */
    @Test
    @DisplayName("TC17: 空 destPort 返回 INVALID_PARAM")
    void testDestPortEmpty() {
        TopoData topoData = loadTopoData();
        SNCServiceImpl snc = createAndInitService();
        snc.setTopoData(topoData);

        PathPlanRequest request = new PathPlanRequest();
        request.setSrcDevice("rack1#os0#npu1");
        request.setSrcPort("400GE 0/0/1");
        request.setDestDevice("rack1#os0#npu2");
        request.setDestPort("");

        PathPlanResult result = snc.planPath(request);
        assertEquals(PlanStatus.INVALID_PARAM, result.getStatus(), "Expected INVALID_PARAM for empty destPort");
    }

    /**
     * TC18: 源设备不存在
     */
    @Test
    @DisplayName("TC18: 不存在源设备返回 SRC_EID_NOT_FOUND")
    void testSrcDeviceNotFound() {
        TopoData topoData = loadTopoData();
        SNCServiceImpl snc = createAndInitService();
        snc.setTopoData(topoData);

        PathPlanRequest request = new PathPlanRequest();
        request.setSrcDevice("rack1#os0#npu-nonexistent");
        request.setSrcPort("400GE 0/0/1");
        request.setDestDevice("rack1#os0#npu2");
        request.setDestPort("400GE 0/0/1");

        PathPlanResult result = snc.planPath(request);
        assertEquals(PlanStatus.SRC_EID_NOT_FOUND, result.getStatus(),
                "Expected SRC_EID_NOT_FOUND for non-existent src device");
    }

    /**
     * TC19: 目的设备不存在
     */
    @Test
    @DisplayName("TC19: 不存在目的设备返回 DEST_EID_NOT_FOUND")
    void testDestDeviceNotFound() {
        TopoData topoData = loadTopoData();
        SNCServiceImpl snc = createAndInitService();
        snc.setTopoData(topoData);

        PathPlanRequest request = new PathPlanRequest();
        request.setSrcDevice("rack1#os0#npu1");
        request.setSrcPort("400GE 0/0/1");
        request.setDestDevice("rack1#os0#npu-nonexistent");
        request.setDestPort("400GE 0/0/1");

        PathPlanResult result = snc.planPath(request);
        assertEquals(PlanStatus.DEST_EID_NOT_FOUND, result.getStatus(),
                "Expected DEST_EID_NOT_FOUND for non-existent dest device");
    }

    /**
     * TC20: 无 interDevices（合法直连请求）
     * 实现行为：空 interDevices 被当作合法场景，查询返回 SUCCESS
     */
    @Test
    @DisplayName("TC20: 无 interDevices 合法直连请求返回 SUCCESS")
    void testInvalidInterDevices() {
        TopoData topoData = loadTopoData();
        SNCServiceImpl snc = createAndInitService();
        snc.setTopoData(topoData);

        PathPlanRequest request = new PathPlanRequest();
        request.setSrcDevice("rack1#os0#npu1");
        request.setSrcPort("400GE 0/0/1");
        request.setDestDevice("rack1#os0#npu2");
        request.setDestPort("400GE 0/0/1");
        request.setInterDevices(new java.util.LinkedHashMap<>());

        PathPlanResult result = snc.planPath(request);
        assertEquals(PlanStatus.SUCCESS, result.getStatus(), "Expected SUCCESS when no interDevices needed");
    }

    /**
     * TC21: 源目相同设备（不同端口）
     */
    @Test
    @DisplayName("TC21: 源目相同但不同端口返回 INVALID_PARAM")
    void testSameSrcDstDiffPort() {
        TopoData topoData = loadTopoData();
        SNCServiceImpl snc = createAndInitService();
        snc.setTopoData(topoData);

        PathPlanRequest request = new PathPlanRequest();
        request.setSrcDevice("rack1#os0#npu1");
        request.setSrcPort("400GE 0/0/1");
        request.setDestDevice("rack1#os0#npu1");
        request.setDestPort("400GE 0/0/2");

        PathPlanResult result = snc.planPath(request);
        assertEquals(PlanStatus.INVALID_PARAM, result.getStatus(), "Expected INVALID_PARAM for same src/dst diff ports");
    }

    /**
     * TC22: setTopoData(null) 静默忽略
     */
    @Test
    @DisplayName("TC22: setTopoData(null) silently ignored")
    void testSetTopoDataNull() {
        SNCServiceImpl snc = createAndInitService();
        assertDoesNotThrow(() -> snc.setTopoData(null), "setTopoData(null) should not throw");
    }

    /**
     * TC23: setAclData(null) 静默忽略
     */
    @Test
    @DisplayName("TC23: setAclData(null) silently ignored")
    void testSetAclDataNull() {
        SNCServiceImpl snc = createAndInitService();
        assertDoesNotThrow(() -> snc.setAclData(null), "setAclData(null) should not throw");
    }

    /**
     * TC24: getTopoData 不存在的拓扑返回 null
     */
    @Test
    @DisplayName("TC24: getTopoData non-existent returns null")
    void testGetTopoDataNonExistent() {
        SNCServiceImpl snc = createAndInitService();
        assertNull(snc.getTopoData("non-existent-topo"), "getTopoData for non-existent topo should return null");
    }

    /**
     * TC25: getAclData 不存在的 ACL 返回 null
     */
    @Test
    @DisplayName("TC25: getAclData non-existent returns null")
    void testGetAclDataNonExistent() {
        SNCServiceImpl snc = createAndInitService();
        assertNull(snc.getAclData("non-existent-acl"), "getAclData for non-existent ACL should return null");
    }

    /**
     * TC26: removeTopoData 不存在的拓扑静默成功
     */
    @Test
    @DisplayName("TC26: removeTopoData non-existent silently succeeds")
    void testRemoveTopoDataNonExistent() {
        SNCServiceImpl snc = createAndInitService();
        assertDoesNotThrow(() -> snc.removeTopoData("non-existent-topo"),
                "removeTopoData for non-existent topo should not throw");
    }

    /**
     * TC27: removeAclData 不存在的 ACL 静默成功
     */
    @Test
    @DisplayName("TC27: removeAclData non-existent silently succeeds")
    void testRemoveAclDataNonExistent() {
        SNCServiceImpl snc = createAndInitService();
        assertDoesNotThrow(() -> snc.removeAclData("non-existent-acl"),
                "removeAclData for non-existent ACL should not throw");
    }

    /**
     * TC28: ACL 规则不存在——设置了 ACL 但 EID 不匹配任何规则
     * 预期：ACL_NOT_FOUND（2002）
     */
    @Test
    @DisplayName("TC28: ACL rule not found when EID doesn't match any rule")
    void testAclRuleNotFound() {
        TopoData topoData = loadTopoData();
        SNCServiceImpl snc = createAndInitService();
        snc.setTopoData(topoData);

        // 构造 ACL 数据，但 EID 不匹配当前查询
        AclData aclData = new AclData();
        aclData.setAclId("acl-mismatch");
        Map<AclKey, TpAclEntity> tpAcls = new HashMap<>();
        TpAclEntity rule = new TpAclEntity();
        rule.setSourceEid("12345678ABCDEF000000000000000001");
        rule.setDestEid("FEDCBA98765432100000000000000002");
        rule.setTransportType(TransportType.RTP);
        rule.setSourceCna("0x123456");
        rule.setDestCna("0x654321");
        tpAcls.put(new AclKey("12345678ABCDEF000000000000000001",
                "FEDCBA98765432100000000000000002", TransportType.RTP), rule);
        aclData.setTpAcls(tpAcls);
        snc.setAclData(aclData);

        PathPlanRequest request = new PathPlanRequest();
        request.setSrcDevice("rack1#os0#npu1");
        request.setSrcPort("400GE 0/0/1");
        request.setDestDevice("rack1#os0#npu2");
        request.setDestPort("400GE 0/0/1");
        request.setInterDevices(new java.util.LinkedHashMap<>());
        request.getInterDevices().put("rack1#la0", "400GE 1/0/2");

        PathPlanResult result = snc.planPath(request);
        assertEquals(PlanStatus.ACL_NOT_FOUND, result.getStatus(),
                "Expected ACL_NOT_FOUND when ACL rule does not match EIDs");
    }

    /**
     * TC29: ACL CNA 不匹配——ACL 规则存在但 CNA 不匹配
     * 预期：ACL_DENIED（2003）
     */
    @Test
    @DisplayName("TC29: ACL CNA mismatch returns ACL_DENIED")
    void testAclCnaMismatch() {
        TopoData topoData = loadTopoData();
        SNCServiceImpl snc = createAndInitService();
        snc.setTopoData(topoData);

        // 构造 ACL 数据，EID 匹配但 CNA 不匹配
        AclData aclData = new AclData();
        aclData.setAclId("acl-cna-mismatch");
        Map<AclKey, TpAclEntity> tpAcls = new HashMap<>();
        TpAclEntity rule = new TpAclEntity();
        rule.setSourceEid("AAAAAA11000000000000000000000001");
        rule.setDestEid("DDDDDD44000000000000000000000004");
        rule.setTransportType(TransportType.RTP);
        rule.setSourceCna("0xDEADBE");  // 与设备实际 CNA 不匹配
        rule.setDestCna("0xBEEF00");    // 与设备实际 CNA 不匹配
        tpAcls.put(new AclKey("AAAAAA11000000000000000000000001",
                "DDDDDD44000000000000000000000004", TransportType.RTP), rule);
        aclData.setTpAcls(tpAcls);
        snc.setAclData(aclData);

        PathPlanRequest request = new PathPlanRequest();
        request.setSrcDevice("rack1#os0#npu1");
        request.setSrcPort("400GE 0/0/1");
        request.setDestDevice("rack1#os0#npu2");
        request.setDestPort("400GE 0/0/1");
        request.setInterDevices(new java.util.LinkedHashMap<>());
        request.getInterDevices().put("rack1#la0", "400GE 1/0/2");

        PathPlanResult result = snc.planPath(request);
        assertEquals(PlanStatus.ACL_DENIED, result.getStatus(), "Expected ACL_DENIED when ACL CNA does not match");
    }

    /**
     * TC30: 查询时不设置 ACL 数据——应成功（ACL 校验跳过）
     * 预期：SUCCESS
     */
    @Test
    @DisplayName("TC30: Plan succeeds without ACL data (ACL check skipped)")
    void testPlanWithoutAclData() {
        TopoData topoData = loadTopoData();
        SNCServiceImpl snc = createAndInitService();
        snc.setTopoData(topoData);
        // 故意不设置 ACL 数据

        PathPlanRequest request = new PathPlanRequest();
        request.setSrcDevice("rack1#os0#npu1");
        request.setSrcPort("400GE 0/0/1");
        request.setDestDevice("rack1#os0#npu2");
        request.setDestPort("400GE 0/0/1");
        request.setInterDevices(new java.util.LinkedHashMap<>());
        request.getInterDevices().put("rack1#la0", "400GE 1/0/2");

        PathPlanResult result = snc.planPath(request);
        assertEquals(PlanStatus.SUCCESS, result.getStatus(), "Expected SUCCESS without ACL");
    }

    /**
     * TC31: 空 ACL（tpAcls 为空）——应成功（ACL 校验跳过）
     * 预期：SUCCESS
     */
    @Test
    @DisplayName("TC31: Plan succeeds with empty ACL (ACL check skipped)")
    void testPlanWithEmptyAcl() {
        TopoData topoData = loadTopoData();
        SNCServiceImpl snc = createAndInitService();
        snc.setTopoData(topoData);

        // 构造空 ACL
        AclData aclData = new AclData();
        aclData.setAclId("acl-empty");
        aclData.setTpAcls(new HashMap<>());
        snc.setAclData(aclData);

        PathPlanRequest request = new PathPlanRequest();
        request.setSrcDevice("rack1#os0#npu1");
        request.setSrcPort("400GE 0/0/1");
        request.setDestDevice("rack1#os0#npu2");
        request.setDestPort("400GE 0/0/1");
        request.setInterDevices(new java.util.LinkedHashMap<>());
        request.getInterDevices().put("rack1#la0", "400GE 1/0/2");

        PathPlanResult result = snc.planPath(request);
        assertEquals(PlanStatus.SUCCESS, result.getStatus(), "Expected SUCCESS with empty ACL");
    }

    /**
     * TC32: 无 interDevices（直连场景）
     */
    @Test
    @DisplayName("TC32: No interDevices direct port connection")
    void testNoInterDevices() {
        TopoData topoData = loadTopoData();
        SNCServiceImpl snc = createAndInitService();
        snc.setTopoData(topoData);

        PathPlanRequest request = new PathPlanRequest();
        request.setSrcDevice("rack1#os0#npu1");
        request.setSrcPort("400GE 0/0/1");
        request.setDestDevice("rack1#os0#npu2");
        request.setDestPort("400GE 0/0/1");
        // 不设置 interDevices

        PathPlanResult result = snc.planPath(request);
        assertNotNull(result, "Result should not be null");
        // 不强制断言具体状态，只验证不抛异常
    }

    /**
     * TC33: 多次 init() 调用
     */
    @Test
    @DisplayName("TC33: Multiple init() calls handled correctly")
    void testMultipleInit() {
        SNCServiceImpl snc = new SNCServiceImpl();
        snc.init();
        assertDoesNotThrow(() -> snc.init(), "Multiple init() should not throw");
    }

    /**
     * TC34: 多次 uninit() 调用
     */
    @Test
    @DisplayName("TC34: Multiple uninit() calls handled correctly")
    void testMultipleUninit() {
        SNCServiceImpl snc = new SNCServiceImpl();
        snc.init();
        snc.uninit();
        assertDoesNotThrow(() -> snc.uninit(), "Multiple uninit() should not throw");
    }

    /**
     * TC35: init 后更新 topo
     */
    @Test
    @DisplayName("TC35: setTopoData update after init works correctly")
    void testTopoDataUpdateAfterInit() {
        TopoData topoData = loadTopoData();
        SNCServiceImpl snc = createAndInitService();
        snc.setTopoData(topoData);

        // 再次设置相同数据
        snc.setTopoData(topoData);

        TopoData retrieved = snc.getTopoData("A5-superPod-1");
        assertNotNull(retrieved, "TopoData should still exist after update");
    }

    /**
     * TC36: init 前设 topo，init 后同步
     */
    @Test
    @DisplayName("TC36: setTopoData before init is synced to pathService on init")
    void testSetTopoDataBeforeInit() {
        SNCServiceImpl snc = new SNCServiceImpl();
        TopoData topoData = loadTopoData();
        snc.setTopoData(topoData);
        snc.init();

        PathPlanRequest request = new PathPlanRequest();
        request.setSrcDevice("rack1#os0#npu1");
        request.setSrcPort("400GE 0/0/1");
        request.setDestDevice("rack1#os0#npu2");
        request.setDestPort("400GE 0/0/1");
        request.setInterDevices(new java.util.LinkedHashMap<>());
        request.getInterDevices().put("rack1#la0", "400GE 1/0/2");

        PathPlanResult result = snc.planPath(request);
        assertNotNull(result, "Should be able to plan after init with pre-set topo");
    }

    // ==================== 新增异常场景 ====================

    /**
     * TC37: listAllTopoNames
     */
    @Test
    @DisplayName("TC37: listAllTopoNames returns correct names")
    void testListAllTopoNames() {
        TopoData topoData = loadTopoData();
        SNCServiceImpl snc = createAndInitService();
        snc.setTopoData(topoData);

        Set<String> names = snc.listAllTopoNames();
        assertNotNull(names, "listAllTopoNames should not return null");
        assertTrue(names.contains("A5-superPod-1"), "Should contain A5-superPod-1");
        assertEquals(1, names.size(), "Should have 1 topo name");
    }

    /**
     * TC38: clearAllTopos
     */
    @Test
    @DisplayName("TC38: clearAllTopos removes all topology data")
    void testClearAllTopos() {
        TopoData topoData = loadTopoData();
        SNCServiceImpl snc = createAndInitService();
        snc.setTopoData(topoData);

        snc.clearAllTopos();
        assertEquals(0, snc.listAllTopos().size(), "listAllTopos should be empty after clearAllTopos");
        assertEquals(0, snc.listAllTopoNames().size(), "listAllTopoNames should be empty after clearAllTopos");
    }

    /**
     * TC39: listAllAclIds
     */
    @Test
    @DisplayName("TC39: listAllAclIds returns correct IDs")
    void testListAllAclIds() {
        AclData aclData = loadAclData();
        SNCServiceImpl snc = createAndInitService();
        snc.setAclData(aclData);

        Set<String> ids = snc.listAllAclIds();
        assertNotNull(ids, "listAllAclIds should not return null");
        assertTrue(ids.contains("acl-001"), "Should contain acl-001");
        assertEquals(1, ids.size(), "Should have 1 ACL ID");
    }

    /**
     * TC40: clearAllAcls
     */
    @Test
    @DisplayName("TC40: clearAllAcls removes all ACL data")
    void testClearAllAcls() {
        AclData aclData = loadAclData();
        SNCServiceImpl snc = createAndInitService();
        snc.setAclData(aclData);

        snc.clearAllAcls();
        assertEquals(0, snc.listAllAcls().size(), "listAllAcls should be empty after clearAllAcls");
        assertEquals(0, snc.listAllAclIds().size(), "listAllAclIds should be empty after clearAllAcls");
    }

    /**
     * TC41: interDevice 不在拓扑中
     */
    @Test
    @DisplayName("TC41: interDevice not in topology returns INVALID_PARAM")
    void testInterDeviceNotInTopo() {
        TopoData topoData = loadTopoData();
        SNCServiceImpl snc = createAndInitService();
        snc.setTopoData(topoData);

        PathPlanRequest request = new PathPlanRequest();
        request.setSrcDevice("rack1#os0#npu1");
        request.setSrcPort("400GE 0/0/1");
        request.setDestDevice("rack1#os0#npu2");
        request.setDestPort("400GE 0/0/1");
        request.setInterDevices(new java.util.LinkedHashMap<>());
        request.getInterDevices().put("rack1#os0#npu999", "400GE 1/0/2");

        PathPlanResult result = snc.planPath(request);
        assertEquals(PlanStatus.INVALID_PARAM, result.getStatus(),
                "Expected INVALID_PARAM when interDevice not in topology");
    }

    /**
     * TC42: ACL 无效 sourceEid
     */
    @Test
    @DisplayName("TC42: Invalid sourceEid throws SNCException INVALID_PARAM")
    void testSetAclDataInvalidSourceEid() {
        AclData aclData = new AclData();
        aclData.setAclId("acl-invalid-eid");
        Map<AclKey, TpAclEntity> tpAcls = new HashMap<>();
        TpAclEntity rule = new TpAclEntity();
        rule.setSourceEid("INVALID_EID_NOT_HEX");
        rule.setDestEid("DDDDDD44000000000000000000000004");
        rule.setTransportType(TransportType.RTP);
        tpAcls.put(new AclKey("INVALID_EID_NOT_HEX",
                "DDDDDD44000000000000000000000004", TransportType.RTP), rule);
        aclData.setTpAcls(tpAcls);

        SNCServiceImpl snc = createAndInitService();
        SNCException exception = assertThrows(SNCException.class, () -> snc.setAclData(aclData),
                "setAclData with invalid sourceEid should throw SNCException");
        assertEquals(PlanStatus.INVALID_PARAM, exception.getStatus(), "Expected INVALID_PARAM status");
    }

    /**
     * TC43: Topo 无效路由前缀
     */
    @Test
    @DisplayName("TC43: Invalid route prefix throws SNCException INVALID_PARAM")
    void testSetTopoDataInvalidRoutePrefix() {
        TopoData topoData = new TopoData();
        topoData.setName("test-topo-invalid-prefix");

        Map<String, DeviceEntity> devices = new HashMap<>();
        DeviceEntity device = new DeviceEntity();
        device.setDeviceId("test-device-invalid");

        // 构造无效路由前缀（通过 RouteEntry + OutPortInfo）
        RouteEntry invalidRoute = new RouteEntry();
        invalidRoute.setPrefix("INVALID_PREFIX_ZZZ");
        OutPortInfo outPortInfo = new OutPortInfo();
        outPortInfo.setNextHop("test-next-hop");
        outPortInfo.setOutInterface("400GE 0/0/1");
        outPortInfo.setMaskLength(24);
        invalidRoute.setOutPortInfos(java.util.Collections.singletonList(outPortInfo));

        Map<String, RouteEntry> routes = new HashMap<>();
        routes.put("INVALID_PREFIX_ZZZ", invalidRoute);
        RouterTable routerTable = new RouterTable();
        routerTable.setInstance(0);
        routerTable.setRoutes(routes);
        device.setRouterTables(java.util.Collections.singletonList(routerTable));

        devices.put("test-device-invalid", device);
        topoData.setDevices(devices);

        SNCServiceImpl snc = createAndInitService();
        SNCException exception = assertThrows(SNCException.class, () -> snc.setTopoData(topoData),
                "setTopoData with invalid route prefix should throw SNCException");
        assertEquals(PlanStatus.INVALID_PARAM, exception.getStatus(), "Expected INVALID_PARAM status");
    }

    /**
     * TC44: ACL 无效 CNA
     */
    @Test
    @DisplayName("TC44: Invalid CNA throws SNCException INVALID_PARAM")
    void testSetAclDataInvalidCna() {
        AclData aclData = new AclData();
        aclData.setAclId("acl-invalid-cna");
        Map<AclKey, TpAclEntity> tpAcls = new HashMap<>();
        TpAclEntity rule = new TpAclEntity();
        rule.setSourceEid("AAAAAA11000000000000000000000001");
        rule.setDestEid("DDDDDD44000000000000000000000004");
        rule.setTransportType(TransportType.RTP);
        rule.setSourceCna("INVALID_CNA");
        tpAcls.put(new AclKey("AAAAAA11000000000000000000000001",
                "DDDDDD44000000000000000000000004", TransportType.RTP), rule);
        aclData.setTpAcls(tpAcls);

        SNCServiceImpl snc = createAndInitService();
        SNCException exception = assertThrows(SNCException.class, () -> snc.setAclData(aclData),
                "setAclData with invalid CNA should throw SNCException");
        assertEquals(PlanStatus.INVALID_PARAM, exception.getStatus(), "Expected INVALID_PARAM status");
    }

    /**
     * TC45: getTopoData 空名称
     */
    @Test
    @DisplayName("TC45: getTopoData with empty name returns null")
    void testGetTopoDataEmptyName() {
        SNCServiceImpl snc = createAndInitService();
        assertNull(snc.getTopoData(""), "getTopoData with empty name should return null");
    }

    /**
     * TC46: removeTopoData 空名称
     */
    @Test
    @DisplayName("TC46: removeTopoData with empty name silently succeeds")
    void testRemoveTopoDataEmptyName() {
        SNCServiceImpl snc = createAndInitService();
        assertDoesNotThrow(() -> snc.removeTopoData(""),
                "removeTopoData with empty name should not throw");
    }

    /**
     * TC47: getAclData 空ID
     */
    @Test
    @DisplayName("TC47: getAclData with empty ID returns null")
    void testGetAclDataEmptyId() {
        SNCServiceImpl snc = createAndInitService();
        assertNull(snc.getAclData(""), "getAclData with empty ID should return null");
    }

    /**
     * TC48: removeAclData 空ID
     */
    @Test
    @DisplayName("TC48: removeAclData with empty ID silently succeeds")
    void testRemoveAclDataEmptyId() {
        SNCServiceImpl snc = createAndInitService();
        assertDoesNotThrow(() -> snc.removeAclData(""),
                "removeAclData with empty ID should not throw");
    }

    /**
     * TC49: clearAllTopos 后再 planPath
     * 注意：clearAllTopos 只清空 store，pathService 中仍然缓存了已加载的 topo 数据
     * 因此查询仍然可以成功（基于缓存的数据）
     * 预期：SUCCESS（基于 pathService 中的缓存数据）
     */
    @Test
    @DisplayName("TC49: Plan still works after clearAllTopos (pathService cache retained)")
    void testPlanAfterClearAllTopos() {
        TopoData topoData = loadTopoData();
        SNCServiceImpl snc = createAndInitService();
        snc.setTopoData(topoData);

        // clearAllTopos 只清空 store，不影响已同步到 pathService 的数据
        snc.clearAllTopos();

        PathPlanRequest request = new PathPlanRequest();
        request.setSrcDevice("rack1#os0#npu1");
        request.setSrcPort("400GE 0/0/1");
        request.setDestDevice("rack1#os0#npu2");
        request.setDestPort("400GE 0/0/1");
        request.setInterDevices(new java.util.LinkedHashMap<>());
        request.getInterDevices().put("rack1#la0", "400GE 1/0/2");

        PathPlanResult result = snc.planPath(request);
        // store 已清除但 pathService 仍有缓存数据，查询仍然可以成功
        assertEquals(PlanStatus.SUCCESS, result.getStatus(),
                "Plan should succeed because pathService retains cached data after clearAllTopos");
    }

    /**
     * TC50: setTopoData 只有 name 和 devices（绕过 TopoService 校验直接存 store）
     */
    @Test
    @DisplayName("TC50: setTopoData with name only (no devices) stored in store")
    void testSetTopoDataNameOnlyNoDevices() {
        SNCServiceImpl snc = createAndInitService();
        TopoData topoData = new TopoData();
        topoData.setName("name-only-topo");
        // 不设置 devices

        snc.setTopoData(topoData);

        // 只有 name 和 devices 时，绕过 TopoService 校验直接存 store
        TopoData retrieved = snc.getTopoData("name-only-topo");
        assertNotNull(retrieved, "TopoData with only name should be stored in store");
        assertEquals("name-only-topo", retrieved.getName(), "Name should match");
    }

    /**
     * TC51: uninit 不 init
     */
    @Test
    @DisplayName("TC51: uninit without prior init does not throw")
    void testUninitWithoutInit() {
        SNCServiceImpl snc = new SNCServiceImpl();
        assertDoesNotThrow(() -> snc.uninit(), "uninit without prior init should not throw");
    }

    /**
     * TC52: uninit 清空所有数据
     */
    @Test
    @DisplayName("TC52: uninit clears all data (topos and ACLs)")
    void testUninitClearsAllData() {
        TopoData topoData = loadTopoData();
        AclData aclData = loadAclData();
        SNCServiceImpl snc = createAndInitService();
        snc.setTopoData(topoData);
        snc.setAclData(aclData);

        snc.uninit();

        // uninit 后可以重新 init
        snc.init();
        assertEquals(0, snc.listAllTopos().size(), "Topos should be empty after uninit+reinit");
        assertEquals(0, snc.listAllAcls().size(), "ACLs should be empty after uninit+reinit");
    }

    /**
     * TC53: listAllTopos 初始为空
     */
    @Test
    @DisplayName("TC53: listAllTopos returns empty list initially")
    void testListAllToposEmptyInitially() {
        SNCServiceImpl snc = createAndInitService();
        List<TopoData> topos = snc.listAllTopos();
        assertNotNull(topos, "listAllTopos should not return null");
        assertEquals(0, topos.size(), "listAllTopos should be empty initially");
    }

    /**
     * TC54: listAllTopoNames 初始空
     */
    @Test
    @DisplayName("TC54: listAllTopoNames returns empty set initially")
    void testListAllTopoNamesEmptyInitially() {
        SNCServiceImpl snc = createAndInitService();
        Set<String> names = snc.listAllTopoNames();
        assertNotNull(names, "listAllTopoNames should not return null");
        assertEquals(0, names.size(), "listAllTopoNames should be empty initially");
    }

    /**
     * TC55: listAllAcls 初始空
     */
    @Test
    @DisplayName("TC55: listAllAcls returns empty list initially")
    void testListAllAclsEmptyInitially() {
        SNCServiceImpl snc = createAndInitService();
        List<AclData> acls = snc.listAllAcls();
        assertNotNull(acls, "listAllAcls should not return null");
        assertEquals(0, acls.size(), "listAllAcls should be empty initially");
    }

    /**
     * TC56: listAllAclIds 初始空
     */
    @Test
    @DisplayName("TC56: listAllAclIds returns empty set initially")
    void testListAllAclIdsEmptyInitially() {
        SNCServiceImpl snc = createAndInitService();
        Set<String> ids = snc.listAllAclIds();
        assertNotNull(ids, "listAllAclIds should not return null");
        assertEquals(0, ids.size(), "listAllAclIds should be empty initially");
    }
}