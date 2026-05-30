/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc exception test module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc exception test module
 */
package com.huawei.umdk.snc.exception;

import com.huawei.umdk.snc.entity.PlanStatus;
import org.junit.jupiter.api.DisplayName;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.*;

@DisplayName("SNCException 单元测试")
class SNCExceptionTest {

    @Test
    @DisplayName("单参构造 — status + errorCode + message from status")
    void testConstructorStatusOnly() {
        SNCException ex = new SNCException(PlanStatus.SUCCESS);
        assertEquals(PlanStatus.SUCCESS, ex.getStatus());
        assertEquals(0, ex.getErrorCode());
        assertEquals("成功", ex.getMessage());
    }

    @Test
    @DisplayName("单参构造 — INVALID_PARAM")
    void testConstructorStatusInvalidParam() {
        SNCException ex = new SNCException(PlanStatus.INVALID_PARAM);
        assertEquals(PlanStatus.INVALID_PARAM, ex.getStatus());
        assertEquals(3001, ex.getErrorCode());
        assertEquals("无效参数", ex.getMessage());
    }

    @Test
    @DisplayName("双参构造 — 自定义 detail")
    void testConstructorStatusAndDetail() {
        SNCException ex = new SNCException(PlanStatus.TOPO_NOT_FOUND, "Topology 'test' not found");
        assertEquals(PlanStatus.TOPO_NOT_FOUND, ex.getStatus());
        assertEquals(2001, ex.getErrorCode());
        assertEquals("Topology 'test' not found", ex.getMessage());
    }

    @Test
    @DisplayName("双参构造 — null detail 回退到 status message")
    void testConstructorStatusAndNullDetail() {
        SNCException ex = new SNCException(PlanStatus.ACL_NOT_FOUND, null);
        assertEquals(PlanStatus.ACL_NOT_FOUND, ex.getStatus());
        assertEquals(2002, ex.getErrorCode());
        assertEquals("ACL未找到", ex.getMessage());
    }

    @Test
    @DisplayName("三参构造 — 包含 cause")
    void testConstructorWithCause() {
        RuntimeException cause = new RuntimeException("Root cause");
        SNCException ex = new SNCException(PlanStatus.PATH_NOT_FOUND, "Path not reachable", cause);
        assertEquals(PlanStatus.PATH_NOT_FOUND, ex.getStatus());
        assertEquals(2004, ex.getErrorCode());
        assertEquals("Path not reachable", ex.getMessage());
        assertSame(cause, ex.getCause());
    }

    @Test
    @DisplayName("三参构造 — null detail 且 null cause")
    void testConstructorNullDetailNullCause() {
        SNCException ex = new SNCException(PlanStatus.ACL_DENIED, null, null);
        assertEquals(PlanStatus.ACL_DENIED, ex.getStatus());
        assertEquals(2003, ex.getErrorCode());
        assertEquals("ACL拒绝", ex.getMessage());
        assertNull(ex.getCause());
    }

    @Test
    @DisplayName("toString 包含 status, errorCode, message")
    void testToString() {
        SNCException ex = new SNCException(PlanStatus.TOPO_NOT_FOUND, "my topo");
        String s = ex.toString();
        assertTrue(s.contains("SNCException"));
        assertTrue(s.contains("TOPO_NOT_FOUND"));
        assertTrue(s.contains("2001"));
        assertTrue(s.contains("my topo"));
    }

    @Test
    @DisplayName("继承自 RuntimeException")
    void testIsRuntimeException() {
        SNCException ex = new SNCException(PlanStatus.SUCCESS);
        assertTrue(ex instanceof RuntimeException);
    }
}