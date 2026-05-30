/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc acl service test module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc acl service test module
 */
package com.huawei.umdk.snc.service;

import com.huawei.umdk.snc.entity.AclData;
import com.huawei.umdk.snc.entity.PlanStatus;
import com.huawei.umdk.snc.entity.TpAclEntity;
import com.huawei.umdk.snc.exception.SNCException;
import com.huawei.umdk.snc.store.AclStore;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.DisplayName;
import org.junit.jupiter.api.Test;

import java.util.HashMap;

import static org.junit.jupiter.api.Assertions.*;

@DisplayName("AclService 单元测试")
class AclServiceTest {

    private AclStore store;
    private AclService service;
    private AclData acl;

    @BeforeEach
    void setUp() {
        store = new AclStore();
        service = new AclService(store);
        acl = new AclData();
        acl.setAclId("myacl");
        acl.setTpAcls(new HashMap<>());
    }

    @Test
    @DisplayName("addOrUpdateAcl — 成功添加")
    void testAddAclSuccess() {
        service.addOrUpdateAcl(acl);
        assertTrue(service.containsAcl("myacl"));
        assertEquals(1, service.getAclCount());
    }

    @Test
    @DisplayName("addOrUpdateAcl — null 参数抛 SNCException")
    void testAddAclNull() {
        SNCException ex = assertThrows(SNCException.class, () -> service.addOrUpdateAcl(null));
        assertEquals(PlanStatus.INVALID_PARAM, ex.getStatus());
    }

    @Test
    @DisplayName("addOrUpdateAcl — aclId 为空抛 SNCException")
    void testAddAclIdEmpty() {
        AclData a = new AclData();
        SNCException ex = assertThrows(SNCException.class, () -> service.addOrUpdateAcl(a));
        assertEquals(PlanStatus.INVALID_PARAM, ex.getStatus());
    }

    @Test
    @DisplayName("addOrUpdateAcl — aclId 为 null 抛 SNCException")
    void testAddAclIdNull() {
        AclData a = new AclData();
        SNCException ex = assertThrows(SNCException.class, () -> service.addOrUpdateAcl(a));
        assertEquals(PlanStatus.INVALID_PARAM, ex.getStatus());
    }

    @Test
    @DisplayName("findAcl — 找到返回")
    void testFindAclSuccess() {
        service.addOrUpdateAcl(acl);
        AclData result = service.findAcl("myacl");
        assertNotNull(result);
        assertEquals("myacl", result.getAclId());
    }

    @Test
    @DisplayName("findAcl — null id 抛异常")
    void testFindAclNull() {
        SNCException ex = assertThrows(SNCException.class, () -> service.findAcl(null));
        assertEquals(PlanStatus.INVALID_PARAM, ex.getStatus());
    }

    @Test
    @DisplayName("findAcl — empty id 抛异常")
    void testFindAclEmpty() {
        SNCException ex = assertThrows(SNCException.class, () -> service.findAcl(""));
        assertEquals(PlanStatus.INVALID_PARAM, ex.getStatus());
    }

    @Test
    @DisplayName("findAcl — 不存在抛 ACL_NOT_FOUND")
    void testFindAclNotFound() {
        SNCException ex = assertThrows(SNCException.class, () -> service.findAcl("nonexistent"));
        assertEquals(PlanStatus.ACL_NOT_FOUND, ex.getStatus());
    }

    @Test
    @DisplayName("removeAcl — 成功删除")
    void testRemoveAclSuccess() {
        service.addOrUpdateAcl(acl);
        service.removeAcl("myacl");
        assertFalse(service.containsAcl("myacl"));
        assertEquals(0, service.getAclCount());
    }

    @Test
    @DisplayName("removeAcl — null id 抛异常")
    void testRemoveAclNull() {
        SNCException ex = assertThrows(SNCException.class, () -> service.removeAcl(null));
        assertEquals(PlanStatus.INVALID_PARAM, ex.getStatus());
    }

    @Test
    @DisplayName("removeAcl — empty id 抛异常")
    void testRemoveAclEmpty() {
        SNCException ex = assertThrows(SNCException.class, () -> service.removeAcl(""));
        assertEquals(PlanStatus.INVALID_PARAM, ex.getStatus());
    }

    @Test
    @DisplayName("removeAcl — 不存在抛 ACL_NOT_FOUND")
    void testRemoveAclNotFound() {
        SNCException ex = assertThrows(SNCException.class, () -> service.removeAcl("nonexistent"));
        assertEquals(PlanStatus.ACL_NOT_FOUND, ex.getStatus());
    }

    @Test
    @DisplayName("containsAcl — null 返回 false")
    void testContainsAclNull() {
        assertFalse(service.containsAcl(null));
    }

    @Test
    @DisplayName("getAclCount — 返回数量")
    void testGetAclCount() {
        assertEquals(0, service.getAclCount());
        service.addOrUpdateAcl(acl);
        assertEquals(1, service.getAclCount());
    }

    @Test
    @DisplayName("clearAll — 清空")
    void testClearAll() {
        service.addOrUpdateAcl(acl);
        service.clearAll();
        assertEquals(0, service.getAclCount());
    }
}