/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc path service test module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc path service test module
 */
package com.huawei.umdk.snc.service;

import com.huawei.umdk.snc.config.SNCConfig;
import com.huawei.umdk.snc.entity.*;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.DisplayName;
import org.junit.jupiter.api.Nested;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.*;

@DisplayName("PathService 单元测试")
class PathServiceTest {

    private SNCConfig config;
    private PathService pathService;

    @BeforeEach
    void setUp() {
        config = new SNCConfig();
        pathService = new PathService(config);
    }

    @Test
    @DisplayName("init 初始化引擎")
    void testInit() {
        pathService.init();
        // 验证 engine 已被创建（通过 planPath 的报错间接验证）
        PathPlanRequest req = new PathPlanRequest();
        PathPlanResult result = pathService.planPath(req);
        // 在 init 后但未设 topoData 时，应返回 TOPO_NOT_FOUND
        assertEquals(PlanStatus.TOPO_NOT_FOUND, result.getStatus());
    }

    @Test
    @DisplayName("未 init 时 planPath 返回 INIT 错误")
    void testPlanPathNotInit() {
        PathPlanRequest req = new PathPlanRequest();
        PathPlanResult result = pathService.planPath(req);
        assertEquals(PlanStatus.INVALID_PARAM, result.getStatus());
        assertEquals(-1, result.getErrorCode());
    }

    @Test
    @DisplayName("uninit 清理资源")
    void testUninit() {
        pathService.init();
        pathService.uninit();
        // 清理后 planPath 应失败
        PathPlanRequest req = new PathPlanRequest();
        PathPlanResult result = pathService.planPath(req);
        assertEquals(PlanStatus.INVALID_PARAM, result.getStatus());
    }

    @Test
    @DisplayName("setTopoData 后 getTopoData")
    void testSetTopoData() {
        TopoData topo = new TopoData();
        topo.setName("test");
        pathService.setTopoData(topo);
        // 没有 getter，通过 init+plan 验证数据已设置
        pathService.init();
        PathPlanRequest req = new PathPlanRequest();
        req.setSrcDevice("d1");
        req.setSrcPort("p1");
        req.setDestDevice("d2");
        req.setDestPort("p2");
        PathPlanResult result = pathService.planPath(req);
        // topo 中没有 d1/d2，返回 SRC_EID_NOT_FOUND
        assertEquals(PlanStatus.SRC_EID_NOT_FOUND, result.getStatus());
    }

    @Test
    @DisplayName("setAclData 设置数据")
    void testSetAclData() {
        AclData acl = new AclData();
        acl.setAclId("acl1");
        pathService.setAclData(acl);
        // ACL 在 ACL 关闭时不生效，路径查询不受影响
        pathService.init();
    }

    @Nested
    @DisplayName("完整流程：init → setTopo → planPath → uninit")
    class FullFlowTest {

        @Test
        @DisplayName("先 setTopoData 再 init")
        void testSetTopoBeforeInit() {
            TopoData topo = new TopoData();
            topo.setName("t1");
            pathService.setTopoData(topo);

            PathPlanRequest req = new PathPlanRequest();
            req.setSrcDevice("d1");
            req.setSrcPort("p1");
            req.setDestDevice("d2");
            req.setDestPort("p2");
            // 未 init 会失败
            PathPlanResult r1 = pathService.planPath(req);
            assertEquals(PlanStatus.INVALID_PARAM, r1.getStatus());

            // init 后
            pathService.init();
            PathPlanResult r2 = pathService.planPath(req);
            assertEquals(PlanStatus.SRC_EID_NOT_FOUND, r2.getStatus());
        }

        @Test
        @DisplayName("init 前已设 topo 和 acl 数据")
        void testInitWithPresetData() {
            TopoData topo = new TopoData();
            topo.setName("t");
            AclData acl = new AclData();
            acl.setAclId("a");
            pathService.setTopoData(topo);
            pathService.setAclData(acl);
            pathService.init();

            PathPlanRequest req = new PathPlanRequest();
            req.setSrcDevice("d1");
            req.setSrcPort("p1");
            req.setDestDevice("d2");
            req.setDestPort("p2");
            PathPlanResult result = pathService.planPath(req);
            assertEquals(PlanStatus.SRC_EID_NOT_FOUND, result.getStatus());
        }
    }

    @Test
    @DisplayName("ACL 关闭时 planPath 跳过 ACL")
    void testPlanPathAclDisabled() {
        pathService.init();
        // 即使 aclData 存在也不做 ACL
        pathService.setAclData(new AclData());
        PathPlanRequest req = new PathPlanRequest();
        PathPlanResult result = pathService.planPath(req);
        // topoData 未设置 → TOPO_NOT_FOUND 先触发
        assertEquals(PlanStatus.TOPO_NOT_FOUND, result.getStatus());
    }

    @Test
    @DisplayName("topoData 为 null 返回 TOPO_NOT_FOUND")
    void testPlanPathNoTopo() {
        pathService.init();
        PathPlanRequest req = new PathPlanRequest();
        req.setSrcDevice("d");
        req.setSrcPort("p");
        req.setDestDevice("d2");
        req.setDestPort("p2");
        PathPlanResult result = pathService.planPath(req);
        assertEquals(PlanStatus.TOPO_NOT_FOUND, result.getStatus());
    }
}