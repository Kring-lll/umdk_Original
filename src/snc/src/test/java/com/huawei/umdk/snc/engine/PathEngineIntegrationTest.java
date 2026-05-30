/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc path engine integration test test module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc path engine integration test test module
 */
package com.huawei.umdk.snc.engine;

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import com.google.gson.reflect.TypeToken;
import com.huawei.umdk.snc.SNCService;
import com.huawei.umdk.snc.SNCServiceImpl;
import com.huawei.umdk.snc.config.SNCConfig;
import com.huawei.umdk.snc.entity.*;
import com.huawei.umdk.snc.entity.HopInfo;
import com.huawei.umdk.snc.entity.PathInfo;

import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.lang.reflect.Type;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * PathEngine 主成功场景集成测试
 *
 * 使用 topo-data.json 和 acl-data.json 作为测试输入数据，
 * 验证完整的拓扑加载→ACL加载→路径查询流程。
 *
 * Topo 名称：A5-superPod-1
 * devices: NPU1(rack1#os0#npu1), LA(rack1#la0), NPU2(rack1#os0#npu2)
 */
public class PathEngineIntegrationTest {

    private static final Gson GSON = createGson();

    private static Gson createGson() {
        GsonBuilder builder = new GsonBuilder();
        // 注册 AclData 的自定义反序列化器，处理 tpAcls 中字符串 key 转 AclKey
        builder.registerTypeAdapter(AclData.class, new AclDataDeserializer());
        return builder.create();
    }

    /**
     * 自定义 AclData 反序列化器
     *
     * 问题：JSON 中 tpAcls 是一个 Map<String, TpAclEntity>，
     * 但 Java 类定义是 Map<AclKey, TpAclEntity>。
     * 此反序列化器将字符串 key（格式：sourceEid|destEid|transportType）
     * 解析为 AclKey 对象。
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

                    // 解析 key: sourceEid|destEid|transportType
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
            // 格式: sourceEid|destEid|transportType
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

    /**
     * 从 classpath 加载 JSON 并反序列化为指定类型
     */
    private static <T> T loadJson(String resourcePath, Type type) {
        InputStream is = PathEngineIntegrationTest.class.getClassLoader().getResourceAsStream(resourcePath);
        if (is == null) {
            throw new RuntimeException("Resource not found: " + resourcePath);
        }
        try (InputStreamReader reader = new InputStreamReader(is, StandardCharsets.UTF_8)) {
            return GSON.fromJson(reader, type);
        } catch (IOException e) {
            throw new RuntimeException("Failed to read resource: " + resourcePath, e);
        }
    }

    /**
     * 加载 TopoData
     */
    private static TopoData loadTopoData() {
        return loadJson("topo-data.json", TopoData.class);
    }

    /**
     * 加载 AclData
     */
    private static AclData loadAclData() {
        return loadJson("acl-data.json", AclData.class);
    }

    /**
     * 创建并初始化 SNCService 实例
     */
    private static SNCService createSNCService(TopoData topoData, AclData aclData) {
        SNCConfig config = new SNCConfig();
        SNCServiceImpl snc = new SNCServiceImpl(config);
        snc.init();
        snc.setTopoData(topoData);
        snc.setAclData(aclData);
        return snc;
    }

    // ==================== 测试方法 ====================

    /**
     * TC01: 主成功场景 - NPU1→NPU2（路径经过LA）
     *
     * 预期路径：NPU1(rack1#os0#npu1) → LA(rack1#la0) → NPU2(rack1#os0#npu2)
     * 共3跳（每个设备一个 HopInfo）
     */
    private static TestResult testMainSuccessPath() {
        try {
            // 1. 加载数据
            TopoData topoData = loadTopoData();
            AclData aclData = loadAclData();

            // 2. 创建服务并初始化
            SNCService snc = createSNCService(topoData, aclData);

            // 3. 构造请求 - NPU1 → NPU2（device+port 模式）
            PathPlanRequest request = new PathPlanRequest();
            request.setSrcDevice("rack1#os0#npu1");
            request.setSrcPort("400GE 0/0/1");
            request.setDestDevice("rack1#os0#npu2");
            request.setDestPort("400GE 0/0/1");
            request.setInterDevices(new java.util.LinkedHashMap<>());
            request.getInterDevices().put("rack1#la0", "400GE 1/0/2");

            // 4. 执行查询
            PathPlanResult result = snc.planPath(request);

            // 5. 验证
            if (result == null) {
                return TestResult.fail("TC01", "result is null");
            }
            if (result.getStatus() != PlanStatus.SUCCESS) {
                return TestResult.fail("TC01", "Expected SUCCESS, got " + result.getStatus()
                        + ": " + result.getErrorMessage());
            }
            List<PathInfo> paths = result.getPaths();
            if (paths == null || paths.isEmpty()) {
                return TestResult.fail("TC01", "paths is empty");
            }
            PathInfo pathInfo = paths.get(0);
            List<HopInfo> hops = pathInfo.getHops();
            if (hops == null || hops.isEmpty()) {
                return TestResult.fail("TC01", "path hops is empty");
            }

            // 验证跳数（3 个设备 = 3 跳）
            int hopCount = hops.size();
            if (hopCount != 3) {
                return TestResult.fail("TC01", "Expected 3 hops, got " + hopCount);
            }

            // 验证第一跳（NPU1）：deviceId=rack1#os0#npu1, outPort=400GE 0/0/1
            HopInfo hop1 = hops.get(0);
            if (!"rack1#os0#npu1".equals(hop1.getDeviceId())) {
                return TestResult.fail("TC01", "Hop1 deviceId expected rack1#os0#npu1, got " + hop1.getDeviceId());
            }
            if (!"400GE 0/0/1".equals(hop1.getOutPort())) {
                return TestResult.fail("TC01", "Hop1 outPort expected 400GE 0/0/1, got " + hop1.getOutPort());
            }

            // 验证第二跳（LA）：deviceId=rack1#la0, outPort=400GE 1/0/2
            HopInfo hop2 = hops.get(1);
            if (!"rack1#la0".equals(hop2.getDeviceId())) {
                return TestResult.fail("TC01", "Hop2 deviceId expected rack1#la0, got " + hop2.getDeviceId());
            }
            if (!"400GE 1/0/2".equals(hop2.getOutPort())) {
                return TestResult.fail("TC01", "Hop2 outPort expected 400GE 1/0/2, got " + hop2.getOutPort());
            }

            // 验证第三跳（NPU2）：deviceId=rack1#os0#npu2, outPort=400GE 0/0/1
            HopInfo hop3 = hops.get(2);
            if (!"rack1#os0#npu2".equals(hop3.getDeviceId())) {
                return TestResult.fail("TC01", "Hop3 deviceId expected rack1#os0#npu2, got " + hop3.getDeviceId());
            }
            if (!"400GE 0/0/1".equals(hop3.getOutPort())) {
                return TestResult.fail("TC01", "Hop3 outPort expected 400GE 0/0/1, got " + hop3.getOutPort());
            }

            return TestResult.pass("TC01", "Path: NPU1→LA→NPU2, " + hopCount + " hops");
        } catch (Exception e) {
            return TestResult.fail("TC01", "Exception: " + e.getMessage());
        }
    }

    /**
     * TC02: 反向路径 - NPU2→NPU1（路径经过LA）
     *
     * 预期路径：NPU2(rack1#os0#npu2) → LA(rack1#la0) → NPU1(rack1#os0#npu1)
     * 共3跳
     */
    private static TestResult testReversePath() {
        try {
            // 1. 加载数据
            TopoData topoData = loadTopoData();
            AclData aclData = loadAclData();

            // 2. 创建服务并初始化
            SNCService snc = createSNCService(topoData, aclData);

            // 3. 构造请求 - NPU2 → NPU1（device+port 模式）
            PathPlanRequest request = new PathPlanRequest();
            request.setSrcDevice("rack1#os0#npu2");
            request.setSrcPort("400GE 0/0/1");
            request.setDestDevice("rack1#os0#npu1");
            request.setDestPort("400GE 0/0/1");
            request.setInterDevices(new java.util.LinkedHashMap<>());
            request.getInterDevices().put("rack1#la0", "400GE 1/0/1");

            // 4. 执行查询
            PathPlanResult result = snc.planPath(request);

            // 5. 验证
            if (result == null) {
                return TestResult.fail("TC02", "result is null");
            }
            if (result.getStatus() != PlanStatus.SUCCESS) {
                return TestResult.fail("TC02", "Expected SUCCESS, got " + result.getStatus()
                        + ": " + result.getErrorMessage());
            }
            List<PathInfo> paths = result.getPaths();
            if (paths == null || paths.isEmpty()) {
                return TestResult.fail("TC02", "paths is empty");
            }
            PathInfo pathInfo = paths.get(0);
        List<HopInfo> hops = pathInfo.getHops();
            if (hops == null || hops.isEmpty()) {
                return TestResult.fail("TC02", "path hops is empty");
            }

            int hopCount = hops.size();
            if (hopCount != 3) {
                return TestResult.fail("TC02", "Expected 3 hops, got " + hopCount);
            }

            // 验证第一跳（NPU2）
            HopInfo hop1 = hops.get(0);
            if (!"rack1#os0#npu2".equals(hop1.getDeviceId())) {
                return TestResult.fail("TC02", "Hop1 deviceId expected rack1#os0#npu2, got " + hop1.getDeviceId());
            }

            // 验证第二跳（LA）
            HopInfo hop2 = hops.get(1);
            if (!"rack1#la0".equals(hop2.getDeviceId())) {
                return TestResult.fail("TC02", "Hop2 deviceId expected rack1#la0, got " + hop2.getDeviceId());
            }

            // 验证第三跳（NPU1）
            HopInfo hop3 = hops.get(2);
            if (!"rack1#os0#npu1".equals(hop3.getDeviceId())) {
                return TestResult.fail("TC02", "Hop3 deviceId expected rack1#os0#npu1, got " + hop3.getDeviceId());
            }

            return TestResult.pass("TC02", "Path: NPU2→LA→NPU1, " + hopCount + " hops");
        } catch (Exception e) {
            return TestResult.fail("TC02", "Exception: " + e.getMessage());
        }
    }

    /**
     * TC03: 参数校验 - 无效的 srcDevice（null）
     */
    private static TestResult testInvalidSrcDevice() {
        try {
            TopoData topoData = loadTopoData();
            SNCService snc = createSNCService(topoData, null);

            PathPlanRequest request = new PathPlanRequest();
            request.setSrcDevice(null);  // 无效
            request.setSrcPort("400GE 0/0/1");
            request.setDestDevice("rack1#os0#npu2");
            request.setDestPort("400GE 0/0/1");

            PathPlanResult result = snc.planPath(request);
            if (result.getStatus() == PlanStatus.INVALID_PARAM) {
                return TestResult.pass("TC03", "Got expected INVALID_PARAM for null srcDevice");
            }
            return TestResult.fail("TC03", "Expected INVALID_PARAM, got " + result.getStatus());
        } catch (Exception e) {
            return TestResult.fail("TC03", "Exception: " + e.getMessage());
        }
    }

    /**
     * TC04: 拓扑未设置
     */
    private static TestResult testTopoNotSet() {
        try {
            SNCServiceImpl snc = new SNCServiceImpl(new SNCConfig());
            snc.init();

            PathPlanRequest request = new PathPlanRequest();
            request.setSrcDevice("rack1#os0#npu1");
            request.setSrcPort("400GE 0/0/1");
            request.setDestDevice("rack1#os0#npu2");
            request.setDestPort("400GE 0/0/1");

            PathPlanResult result = snc.planPath(request);
            if (result.getStatus() == PlanStatus.TOPO_NOT_FOUND) {
                return TestResult.pass("TC04", "Got expected TOPO_NOT_FOUND when topo not set");
            }
            return TestResult.fail("TC04", "Expected TOPO_NOT_FOUND, got " + result.getStatus()
                    + ": " + result.getErrorMessage());
        } catch (Exception e) {
            return TestResult.fail("TC04", "Exception: " + e.getMessage());
        }
    }

    /**
     * TC05: 源设备在拓扑中不存在
     */
    private static TestResult testSrcDeviceNotFound() {
        try {
            TopoData topoData = loadTopoData();
            SNCService snc = createSNCService(topoData, null);

            PathPlanRequest request = new PathPlanRequest();
            request.setSrcDevice("NonExistentDevice");
            request.setSrcPort("400GE 0/0/1");
            request.setDestDevice("rack1#os0#npu2");
            request.setDestPort("400GE 0/0/1");

            PathPlanResult result = snc.planPath(request);
            if (result.getStatus() == PlanStatus.SRC_EID_NOT_FOUND) {
                return TestResult.pass("TC05", "Got expected SRC_EID_NOT_FOUND");
            }
            return TestResult.fail("TC05", "Expected SRC_EID_NOT_FOUND, got " + result.getStatus()
                    + ": " + result.getErrorMessage());
        } catch (Exception e) {
            return TestResult.fail("TC05", "Exception: " + e.getMessage());
        }
    }

    /**
     * TC06: 源目相同
     */
    private static TestResult testSameSrcDst() {
        try {
            TopoData topoData = loadTopoData();
            SNCService snc = createSNCService(topoData, null);

            PathPlanRequest request = new PathPlanRequest();
            request.setSrcDevice("rack1#os0#npu1");
            request.setSrcPort("400GE 0/0/1");
            request.setDestDevice("rack1#os0#npu1");
            request.setDestPort("400GE 0/0/1");

            PathPlanResult result = snc.planPath(request);
            if (result.getStatus() == PlanStatus.INVALID_PARAM) {
                return TestResult.pass("TC06", "Got expected INVALID_PARAM for same src/dst");
            }
            return TestResult.fail("TC06", "Expected INVALID_PARAM, got " + result.getStatus()
                    + ": " + result.getErrorMessage());
        } catch (Exception e) {
            return TestResult.fail("TC06", "Exception: " + e.getMessage());
        }
    }

    /**
     * TC07: 服务未初始化
     */
    private static TestResult testServiceNotInit() {
        try {
            SNCServiceImpl snc = new SNCServiceImpl(new SNCConfig());
            // 不调用 init()

            PathPlanRequest request = new PathPlanRequest();
            request.setSrcDevice("rack1#os0#npu1");
            request.setSrcPort("400GE 0/0/1");
            request.setDestDevice("rack1#os0#npu2");
            request.setDestPort("400GE 0/0/1");

            PathPlanResult result = snc.planPath(request);
            if (result.getStatus() == PlanStatus.INVALID_PARAM) {
                return TestResult.pass("TC07", "Got expected INVALID_PARAM for not initialized");
            }
            return TestResult.fail("TC07", "Expected INVALID_PARAM, got " + result.getStatus()
                    + ": " + result.getErrorMessage());
        } catch (Exception e) {
            return TestResult.fail("TC07", "Exception: " + e.getMessage());
        }
    }

    /**
     * TC08: JSON 实体完整性校验
     */
    private static TestResult testJsonEntityIntegrity() {
        try {
            TopoData topoData = loadTopoData();

            // 验证拓扑名
            if (!"A5-superPod-1".equals(topoData.getName())) {
                return TestResult.fail("TC08", "Topo name mismatch");
            }

            // 验证设备数量（3台）
            if (topoData.getDevices() == null || topoData.getDevices().size() != 3) {
                return TestResult.fail("TC08", "Expected 3 devices, got "
                        + (topoData.getDevices() != null ? topoData.getDevices().size() : 0));
            }

            // 验证设备是否存在
            DeviceEntity npu1 = topoData.findDevice("rack1#os0#npu1");
            DeviceEntity la = topoData.findDevice("rack1#la0");
            DeviceEntity npu2 = topoData.findDevice("rack1#os0#npu2");

            if (npu1 == null) return TestResult.fail("TC08", "NPU1 device not found");
            if (la == null) return TestResult.fail("TC08", "LA device not found");
            if (npu2 == null) return TestResult.fail("TC08", "NPU2 device not found");

            // 验证设备类型
            if (npu1.getDeviceType() != DeviceType.NPU)
                return TestResult.fail("TC08", "NPU1 device type mismatch");
            if (la.getDeviceType() != DeviceType.LA)
                return TestResult.fail("TC08", "LA device type mismatch");
            if (npu2.getDeviceType() != DeviceType.NPU)
                return TestResult.fail("TC08", "NPU2 device type mismatch");

            // 验证端口连通性
            PortEntity npu1Port = npu1.findPort("400GE 0/0/1");
            if (npu1Port == null) return TestResult.fail("TC08", "NPU1 port 400GE 0/0/1 not found");
            if (!"rack1#la0".equals(npu1Port.getConnectedDevice()))
                return TestResult.fail("TC08", "NPU1 port connectedDevice mismatch");

            PortEntity laPort1 = la.findPort("400GE 1/0/1");
            if (laPort1 == null) return TestResult.fail("TC08", "LA port 400GE 1/0/1 not found");
            if (!"rack1#os0#npu1".equals(laPort1.getConnectedDevice()))
                return TestResult.fail("TC08", "LA port1 connectedDevice mismatch");

            PortEntity laPort2 = la.findPort("400GE 1/0/2");
            if (laPort2 == null) return TestResult.fail("TC08", "LA port 400GE 1/0/2 not found");
            if (!"rack1#os0#npu2".equals(laPort2.getConnectedDevice()))
                return TestResult.fail("TC08", "LA port2 connectedDevice mismatch");

            // 验证路由表存在
            if (npu1.getRouterTables() == null || npu1.getRouterTables().isEmpty())
                return TestResult.fail("TC08", "NPU1 routerTable not found");
            if (la.getRouterTables() == null || la.getRouterTables().isEmpty())
                return TestResult.fail("TC08", "LA routerTable not found");
            if (npu2.getRouterTables() == null || npu2.getRouterTables().isEmpty())
                return TestResult.fail("TC08", "NPU2 routerTable not found");

            // 验证路由条目（NPU1 → DDDDDD44 prefix）
            RouterTable npu1Rt = npu1.getRouterTables().get(0);
            if (npu1Rt.getRoutes() == null || !npu1Rt.getRoutes().containsKey("DDDDDD44"))
                return TestResult.fail("TC08", "NPU1 missing route for prefix DDDDDD44");

            return TestResult.pass("TC08", "All entities loaded correctly with proper connectivity");
        } catch (Exception e) {
            return TestResult.fail("TC08", "Exception: " + e.getMessage());
        }
    }

    /**
     * TC09: ACL 加载和数据完整性校验
     */
    private static TestResult testAclDataIntegrity() {
        try {
            AclData aclData = loadAclData();

            if (!"acl-001".equals(aclData.getAclId())) {
                return TestResult.fail("TC09", "ACL ID mismatch");
            }

            Map<AclKey, TpAclEntity> acls = aclData.getTpAcls();
            if (acls == null || acls.size() != 2) {
                return TestResult.fail("TC09", "Expected 2 ACL entries, got "
                        + (acls != null ? acls.size() : 0));
            }

            // 验证 NPU1→NPU2 的 ACL 规则
            AclKey key1 = new AclKey("AAAAAA11000000000000000000000001",
                    "DDDDDD44000000000000000000000004", TransportType.RTP);
            TpAclEntity rule1 = aclData.findAcl(key1.getSourceEid(), key1.getDestEid(), key1.getTransportType());
            if (rule1 == null) {
                return TestResult.fail("TC09", "ACL rule for NPU1→NPU2 not found");
            }
            if (!"0xAAAA1111".equals(rule1.getSourceCna())) {
                return TestResult.fail("TC09", "Source CNA mismatch for rule1");
            }
            if (!"0xDDDD4444".equals(rule1.getDestCna())) {
                return TestResult.fail("TC09", "Dest CNA mismatch for rule1");
            }

            // 验证 NPU2→NPU1 的 ACL 规则
            AclKey key2 = new AclKey("DDDDDD44000000000000000000000004",
                    "AAAAAA11000000000000000000000001", TransportType.RTP);
            TpAclEntity rule2 = aclData.findAcl(key2.getSourceEid(), key2.getDestEid(), key2.getTransportType());
            if (rule2 == null) {
                return TestResult.fail("TC09", "ACL rule for NPU2→NPU1 not found");
            }
            if (!"0xDDDD4444".equals(rule2.getSourceCna())) {
                return TestResult.fail("TC09", "Source CNA mismatch for rule2");
            }

            return TestResult.pass("TC09", "ACL data loaded correctly with 2 rules");
        } catch (Exception e) {
            return TestResult.fail("TC09", "Exception: " + e.getMessage());
        }
    }

    /**
     * TC10: ACL 校验集成 - NPU1→NPU2 路径应通过 ACL
     */
    private static TestResult testAclCheckIntegration() {
        try {
            TopoData topoData = loadTopoData();
            AclData aclData = loadAclData();
            SNCService snc = createSNCService(topoData, aclData);

            PathPlanRequest request = new PathPlanRequest();
            request.setSrcDevice("rack1#os0#npu1");
            request.setSrcPort("400GE 0/0/1");
            request.setDestDevice("rack1#os0#npu2");
            request.setDestPort("400GE 0/0/1");
            request.setInterDevices(new java.util.LinkedHashMap<>());
            request.getInterDevices().put("rack1#la0", "400GE 1/0/2");

            PathPlanResult result = snc.planPath(request);

            // 设计文档 0525.md §8.6：ACL 错误通过 PathPlanResult.status 返回
            if (result.getStatus() == PlanStatus.ACL_NOT_FOUND || result.getStatus() == PlanStatus.ACL_DENIED) {
                return TestResult.fail("TC10", "ACL check failed: " + result.getStatus()
                        + " - " + result.getErrorMessage());
            }
            if (result.getStatus() != PlanStatus.SUCCESS) {
                return TestResult.fail("TC10", "Expected SUCCESS, got " + result.getStatus());
            }
            // ACL 通过（status 仍为 SUCCESS）
            return TestResult.pass("TC10", "ACL check passed as expected");
        } catch (Exception e) {
            return TestResult.fail("TC10", "Exception: " + e.getMessage());
        }
    }

    // ==================== 入口 ====================

    public static void main(String[] args) {
        System.out.println("========================================");
        System.out.println("  PathEngine 主成功场景集成测试");
        System.out.println("========================================");
        System.out.println();

        int passed = 0;
        int failed = 0;
        int total = 0;

        // TC01: 主成功场景
        total++;
        TestResult r = testMainSuccessPath();
        printResult(r);
        if (r.passed) passed++; else failed++;

        // TC02: 反向路径
        total++;
        r = testReversePath();
        printResult(r);
        if (r.passed) passed++; else failed++;

        // TC03: 无效 srcDevice
        total++;
        r = testInvalidSrcDevice();
        printResult(r);
        if (r.passed) passed++; else failed++;

        // TC04: 拓扑未设置
        total++;
        r = testTopoNotSet();
        printResult(r);
        if (r.passed) passed++; else failed++;

        // TC05: 源设备不存在
        total++;
        r = testSrcDeviceNotFound();
        printResult(r);
        if (r.passed) passed++; else failed++;

        // TC06: 源目相同
        total++;
        r = testSameSrcDst();
        printResult(r);
        if (r.passed) passed++; else failed++;

        // TC07: 服务未初始化
        total++;
        r = testServiceNotInit();
        printResult(r);
        if (r.passed) passed++; else failed++;

        // TC08: JSON 实体完整性
        total++;
        r = testJsonEntityIntegrity();
        printResult(r);
        if (r.passed) passed++; else failed++;

        // TC09: ACL 数据完整性
        total++;
        r = testAclDataIntegrity();
        printResult(r);
        if (r.passed) passed++; else failed++;

        // TC10: ACL 校验集成
        total++;
        r = testAclCheckIntegration();
        printResult(r);
        if (r.passed) passed++; else failed++;

        System.out.println();
        System.out.println("========================================");
        System.out.println("  测试结果汇总: " + passed + "/" + total + " 通过, "
                + failed + "/" + total + " 失败");
        System.out.println("========================================");

        if (failed > 0) {
            System.exit(1);
        }
    }

    private static void printResult(TestResult r) {
        String status = r.passed ? "[PASS]" : "[FAIL]";
        System.out.println(status + " " + r.testName + ": " + r.message);
    }

    static class TestResult {
        String testName;
        boolean passed;
        String message;

        static TestResult pass(String name, String msg) {
            TestResult r = new TestResult();
            r.testName = name;
            r.passed = true;
            r.message = msg;
            return r;
        }

        static TestResult fail(String name, String msg) {
            TestResult r = new TestResult();
            r.testName = name;
            r.passed = false;
            r.message = msg;
            return r;
        }
    }
}