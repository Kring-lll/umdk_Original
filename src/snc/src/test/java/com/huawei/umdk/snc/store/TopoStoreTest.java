package com.huawei.umdk.snc.store;

import com.huawei.umdk.snc.entity.DeviceEntity;
import com.huawei.umdk.snc.entity.TopoData;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.DisplayName;
import org.junit.jupiter.api.Test;

import java.util.HashMap;

import static org.junit.jupiter.api.Assertions.*;

@DisplayName("TopoStore 单元测试")
class TopoStoreTest {

    private TopoStore store;
    private TopoData topo1;
    private TopoData topo2;

    @BeforeEach
    void setUp() {
        store = new TopoStore();
        topo1 = new TopoData();
        topo1.setName("topo1");
        topo1.setVersion("1.0");
        topo1.setDevices(new HashMap<String, DeviceEntity>());

        topo2 = new TopoData();
        topo2.setName("topo2");
        topo2.setVersion("2.0");
        topo2.setDevices(new HashMap<String, DeviceEntity>());
    }

    @Test
    @DisplayName("addOrUpdate — 正常添加")
    void testAddOrUpdate() {
        store.addOrUpdate(topo1);
        assertEquals(1, store.size());
        assertTrue(store.contains("topo1"));
    }

    @Test
    @DisplayName("addOrUpdate — 更新已存在")
    void testAddOrUpdateExisting() {
        store.addOrUpdate(topo1);
        topo1.setVersion("2.0");
        store.addOrUpdate(topo1);
        assertEquals(1, store.size());
        assertEquals("2.0", store.find("topo1").getVersion());
    }

    @Test
    @DisplayName("addOrUpdate — null 参数抛 IllegalArgumentException")
    void testAddOrUpdateNull() {
        assertThrows(IllegalArgumentException.class, () -> store.addOrUpdate(null));
    }

    @Test
    @DisplayName("addOrUpdate — name 为 null 抛异常")
    void testAddOrUpdateNameNull() {
        TopoData t = new TopoData();
        t.setVersion("1.0");
        assertThrows(IllegalArgumentException.class, () -> store.addOrUpdate(t));
    }

    @Test
    @DisplayName("find — 存在返回对象")
    void testFindExists() {
        store.addOrUpdate(topo1);
        TopoData result = store.find("topo1");
        assertNotNull(result);
        assertEquals("topo1", result.getName());
    }

    @Test
    @DisplayName("find — 不存在返回 null")
    void testFindNotExists() {
        assertNull(store.find("nonexistent"));
    }

    @Test
    @DisplayName("find — null key 返回 null")
    void testFindNull() {
        assertNull(store.find(null));
    }

    @Test
    @DisplayName("remove — 存在删除并返回")
    void testRemoveExists() {
        store.addOrUpdate(topo1);
        TopoData removed = store.remove("topo1");
        assertNotNull(removed);
        assertEquals("topo1", removed.getName());
        assertEquals(0, store.size());
        assertFalse(store.contains("topo1"));
    }

    @Test
    @DisplayName("remove — 不存在返回 null")
    void testRemoveNotExists() {
        assertNull(store.remove("nonexistent"));
    }

    @Test
    @DisplayName("contains — 存在返回 true")
    void testContainsTrue() {
        store.addOrUpdate(topo1);
        assertTrue(store.contains("topo1"));
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
        store.addOrUpdate(topo1);
        assertEquals(1, store.size());
        store.addOrUpdate(topo2);
        assertEquals(2, store.size());
    }

    @Test
    @DisplayName("clear — 清空后 size=0")
    void testClear() {
        store.addOrUpdate(topo1);
        store.addOrUpdate(topo2);
        store.clear();
        assertEquals(0, store.size());
        assertNull(store.find("topo1"));
    }
}