/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc acl store test module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc acl store test module
 */
package com.huawei.umdk.snc.store;

import com.huawei.umdk.snc.entity.AclData;
import com.huawei.umdk.snc.entity.TpAclEntity;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.DisplayName;
import org.junit.jupiter.api.Test;

import com.huawei.umdk.snc.entity.AclKey;
import java.util.HashMap;

import static org.junit.jupiter.api.Assertions.*;

@DisplayName("AclStore 单元测试")
class AclStoreTest {

    private AclStore store;
    private AclData acl1;
    private AclData acl2;

    @BeforeEach
    void setUp() {
        store = new AclStore();
        acl1 = new AclData();
        acl1.setAclId("acl1");
        acl1.setTpAcls(new HashMap<>());

        acl2 = new AclData();
        acl2.setAclId("acl2");
        acl2.setTpAcls(new HashMap<>());
    }

    @Test
    @DisplayName("addOrUpdate — 正常添加")
    void testAddOrUpdate() {
        store.addOrUpdate(acl1);
        assertEquals(1, store.size());
        assertTrue(store.contains("acl1"));
    }

    @Test
    @DisplayName("addOrUpdate — 更新已存在")
    void testAddOrUpdateExisting() {
        store.addOrUpdate(acl1);
        acl1.setAclId("acl1");  // same id
        store.addOrUpdate(acl1);
        assertEquals(1, store.size());
    }

    @Test
    @DisplayName("addOrUpdate — null 参数抛 IllegalArgumentException")
    void testAddOrUpdateNull() {
        assertThrows(IllegalArgumentException.class, () -> store.addOrUpdate(null));
    }

    @Test
    @DisplayName("addOrUpdate — aclId 为 null 抛异常")
    void testAddOrUpdateIdNull() {
        AclData a = new AclData();
        assertThrows(IllegalArgumentException.class, () -> store.addOrUpdate(a));
    }

    @Test
    @DisplayName("find — 存在返回对象")
    void testFindExists() {
        store.addOrUpdate(acl1);
        AclData result = store.find("acl1");
        assertNotNull(result);
        assertEquals("acl1", result.getAclId());
    }

    @Test
    @DisplayName("find — 不存在返回 null")
    void testFindNotExists() {
        assertNull(store.find("nonexistent"));
    }

    @Test
    @DisplayName("findAny �?空集合返�?null")
    void testFindAnyEmpty() {
        assertNull(store.findAny());
    }

    @Test
    @DisplayName("findAny — 有数据返回任意一个")
    void testFindAnyWithData() {
        store.addOrUpdate(acl1);
        AclData result = store.findAny();
        assertNotNull(result);
        assertEquals("acl1", result.getAclId());
    }

    @Test
    @DisplayName("findAny — 多个数据返回其中一个")
    void testFindAnyMultiple() {
        store.addOrUpdate(acl1);
        store.addOrUpdate(acl2);
        AclData result = store.findAny();
        assertNotNull(result);
    }

    @Test
    @DisplayName("remove — 存在删除并返回")
    void testRemoveExists() {
        store.addOrUpdate(acl1);
        AclData removed = store.remove("acl1");
        assertNotNull(removed);
        assertEquals("acl1", removed.getAclId());
        assertEquals(0, store.size());
        assertFalse(store.contains("acl1"));
    }

    @Test
    @DisplayName("remove — 不存在返回 null")
    void testRemoveNotExists() {
        assertNull(store.remove("nonexistent"));
    }

    @Test
    @DisplayName("contains �?存在返回 true")
    void testContainsTrue() {
        store.addOrUpdate(acl1);
        assertTrue(store.contains("acl1"));
    }

    @Test
    @DisplayName("contains — 不存在返回 false")
    void testContainsFalse() {
        assertFalse(store.contains("nonexistent"));
    }

    @Test
    @DisplayName("contains — null 返回 false")
    void testContainsNull() {
        assertFalse(store.contains(null));
    }

    @Test
    @DisplayName("size — 反映实际数量")
    void testSize() {
        assertEquals(0, store.size());
        store.addOrUpdate(acl1);
        assertEquals(1, store.size());
        store.addOrUpdate(acl2);
        assertEquals(2, store.size());
    }

    @Test
    @DisplayName("clear — 清空后 size=0")
    void testClear() {
        store.addOrUpdate(acl1);
        store.addOrUpdate(acl2);
        store.clear();
        assertEquals(0, store.size());
        assertNull(store.find("acl1"));
        assertNull(store.findAny());
    }
}