/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc service impl test module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc service impl test module
 */
package com.huawei.umdk.snc;

import com.huawei.umdk.snc.config.SNCConfig;
import com.huawei.umdk.snc.entity.*;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.DisplayName;
import org.junit.jupiter.api.Nested;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.*;

@DisplayName("SNCServiceImpl 单元测试")
class SNCServiceImplTest {

    @Test
    @DisplayName("无参构造后 init 使用默认 config")
    void testDefaultConfigInit() {
        SNCServiceImpl impl = new SNCServiceImpl();
        impl.init();

        PathPlanRequest req = new PathPlanRequest();
        req.setSrcDevice("d1");
        req.setSrcPort("p1");
        req.setDestDevice("d2");
        req.setDestPort("p2");
        PathPlanResult result = impl.planPath(req);
        assertEquals(PlanStatus.TOPO_NOT_FOUND, result.getStatus());
    }

    @Test
    @DisplayName("带 config 构造")
    void testConstructorWithConfig() {
        SNCConfig config = new SNCConfig();
        SNCServiceImpl impl = new SNCServiceImpl(config);
        impl.init();

        PathPlanRequest req = new PathPlanRequest();
        req.setSrcDevice("d1");
        req.setSrcPort("p1");
        req.setDestDevice("d2");
        req.setDestPort("p2");
        PathPlanResult result = impl.planPath(req);
        assertEquals(PlanStatus.TOPO_NOT_FOUND, result.getStatus());
    }

    @Test
    @DisplayName("setConfig 在 init 前设置")
    void testSetConfigBeforeInit() {
        SNCServiceImpl impl = new SNCServiceImpl();
        SNCConfig config = new SNCConfig();
        config.setLogLevel("DEBUG");
        impl.setConfig(config);
        impl.init();

        PathPlanRequest req = new PathPlanRequest();
        req.setSrcDevice("d1");
        req.setSrcPort("p1");
        req.setDestDevice("d2");
        req.setDestPort("p2");
        PathPlanResult result = impl.planPath(req);
        assertEquals(PlanStatus.TOPO_NOT_FOUND, result.getStatus());
    }

    @Test
    @DisplayName("未 init 时 planPath 失败")
    void testPlanPathWithoutInit() {
        SNCServiceImpl impl = new SNCServiceImpl();
        PathPlanResult result = impl.planPath(new PathPlanRequest());
        assertEquals(PlanStatus.INVALID_PARAM, result.getStatus());
        assertEquals(-99, result.getErrorCode());
    }

    @Test
    @DisplayName("setTopoData 在 init 前")
    void testSetTopoBeforeInit() {
        SNCServiceImpl impl = new SNCServiceImpl();
        TopoData topo = new TopoData();
        topo.setName("t");
        impl.setTopoData(topo);
        impl.init();

        PathPlanRequest req = new PathPlanRequest();
        req.setSrcDevice("d1");
        req.setSrcPort("p1");
        req.setDestDevice("d2");
        req.setDestPort("p2");
        PathPlanResult result = impl.planPath(req);
        assertEquals(PlanStatus.SRC_EID_NOT_FOUND, result.getStatus());
    }

    @Test
    @DisplayName("setTopoData 在 init 后")
    void testSetTopoAfterInit() {
        SNCServiceImpl impl = new SNCServiceImpl();
        impl.init();

        TopoData topo = new TopoData();
        topo.setName("t");
        impl.setTopoData(topo);

        PathPlanRequest req = new PathPlanRequest();
        req.setSrcDevice("d1");
        req.setSrcPort("p1");
        req.setDestDevice("d2");
        req.setDestPort("p2");
        PathPlanResult result = impl.planPath(req);
        assertEquals(PlanStatus.SRC_EID_NOT_FOUND, result.getStatus());
    }

    @Test
    @DisplayName("setAclData 在 init 前")
    void testSetAclBeforeInit() {
        SNCServiceImpl impl = new SNCServiceImpl();
        AclData acl = new AclData();
        acl.setAclId("a");
        impl.setAclData(acl);
        impl.init();

        PathPlanRequest req = new PathPlanRequest();
        req.setSrcDevice("d1");
        req.setSrcPort("p1");
        req.setDestDevice("d2");
        req.setDestPort("p2");
        PathPlanResult result = impl.planPath(req);
        assertEquals(PlanStatus.TOPO_NOT_FOUND, result.getStatus());
    }

    @Test
    @DisplayName("setAclData 在 init 后")
    void testSetAclAfterInit() {
        SNCServiceImpl impl = new SNCServiceImpl();
        impl.init();

        AclData acl = new AclData();
        acl.setAclId("a");
        impl.setAclData(acl);

        PathPlanRequest req = new PathPlanRequest();
        req.setSrcDevice("d1");
        req.setSrcPort("p1");
        req.setDestDevice("d2");
        req.setDestPort("p2");
        PathPlanResult result = impl.planPath(req);
        assertEquals(PlanStatus.TOPO_NOT_FOUND, result.getStatus());
    }

    @Test
    @DisplayName("uninit 清理后 planPath 失败")
    void testUninit() {
        SNCServiceImpl impl = new SNCServiceImpl();
        impl.init();
        impl.uninit();

        PathPlanRequest req = new PathPlanRequest();
        PathPlanResult result = impl.planPath(req);
        assertEquals(PlanStatus.INVALID_PARAM, result.getStatus());
    }

    @Test
    @DisplayName("重复 init 不会异常")
    void testDoubleInit() {
        SNCServiceImpl impl = new SNCServiceImpl();
        impl.init();
        assertDoesNotThrow(() -> impl.init());
    }

    @Test
    @DisplayName("未 init 时 uninit 不异常")
    void testUninitWithoutInit() {
        SNCServiceImpl impl = new SNCServiceImpl();
        assertDoesNotThrow(() -> impl.uninit());
    }

    @Test
    @DisplayName("未 init 时 setTopoData 不异常")
    void testSetTopoWithoutInit() {
        SNCServiceImpl impl = new SNCServiceImpl();
        assertDoesNotThrow(() -> impl.setTopoData(new TopoData()));
    }

    @Test
    @DisplayName("未 init 时 setAclData 不异常")
    void testSetAclWithoutInit() {
        SNCServiceImpl impl = new SNCServiceImpl();
        assertDoesNotThrow(() -> impl.setAclData(new AclData()));
    }

    @Test
    @DisplayName("完整生命周期：setTopo → init → planPath → uninit")
    void testFullLifecycle() {
        SNCServiceImpl impl = new SNCServiceImpl();
        TopoData topo = new TopoData();
        topo.setName("test");
        impl.setTopoData(topo);
        impl.init();

        PathPlanRequest req = new PathPlanRequest();
        req.setSrcDevice("d1");
        req.setSrcPort("p1");
        req.setDestDevice("d2");
        req.setDestPort("p2");
        PathPlanResult r1 = impl.planPath(req);
        assertNotNull(r1);

        impl.uninit();
        PathPlanResult r2 = impl.planPath(req);
        assertEquals(PlanStatus.INVALID_PARAM, r2.getStatus());
    }

    @Nested
    @DisplayName("setTopoData / setAclData null 处理")
    class NullDataTest {

        private SNCServiceImpl impl;

        @BeforeEach
        void setUp() {
            impl = new SNCServiceImpl();
            impl.init();
        }

        @Test
        @DisplayName("setTopoData null 不抛异常")
        void testSetTopoNull() {
            assertDoesNotThrow(() -> impl.setTopoData(null));
        }

        @Test
        @DisplayName("setAclData null 不抛异常")
        void testSetAclNull() {
            assertDoesNotThrow(() -> impl.setAclData(null));
        }

        @Test
        @DisplayName("planPath null 参数")
        void testPlanPathNull() {
            PathPlanResult result = impl.planPath(null);
            assertNotNull(result);
            // 实际流程：pathService 已 init，但 topoData 未设置 → TOPO_NOT_FOUND
            assertNotNull(result.getStatus());
        }
    }
}