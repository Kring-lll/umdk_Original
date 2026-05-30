package com.huawei.umdk.snc.service;

import com.huawei.umdk.snc.entity.DeviceEntity;
import com.huawei.umdk.snc.entity.PlanStatus;
import com.huawei.umdk.snc.entity.TopoData;
import com.huawei.umdk.snc.exception.SNCException;
import com.huawei.umdk.snc.store.TopoStore;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.DisplayName;
import org.junit.jupiter.api.Test;

import java.util.HashMap;
import java.util.Collections;

import static org.junit.jupiter.api.Assertions.*;

@DisplayName("TopoService 单元测试")
class TopoServiceTest {

    private TopoStore store;
    private TopoService service;
    private TopoData topo;

    @BeforeEach
    void setUp() {
        store = new TopoStore();
        service = new TopoService(store);
        topo = new TopoData();
        topo.setName("mytopo");
        topo.setVersion("1.0");
        DeviceEntity dev = new DeviceEntity();
        dev.setDeviceId("rack1#os0#npu1");
        topo.setDevices(new HashMap<>(Collections.singletonMap("rack1#os0#npu1", dev)));
    }

    @Test
    @DisplayName("addOrUpdateTopo — 成功添加")
    void testAddTopoSuccess() {
        service.addOrUpdateTopo(topo);
        assertTrue(service.containsTopo("mytopo"));
        assertEquals(1, service.getTopoCount());
    }

    @Test
    @DisplayName("addOrUpdateTopo — null 参数抛 SNCException")
    void testAddTopoNull() {
        SNCException ex = assertThrows(SNCException.class, () -> service.addOrUpdateTopo(null));
        assertEquals(PlanStatus.INVALID_PARAM, ex.getStatus());
    }

    @Test
    @DisplayName("addOrUpdateTopo — name 为空抛 SNCException")
    void testAddTopoNameEmpty() {
        TopoData t = new TopoData();
        t.setDevices(new HashMap<String, DeviceEntity>());
        SNCException ex = assertThrows(SNCException.class, () -> service.addOrUpdateTopo(t));
        assertEquals(PlanStatus.INVALID_PARAM, ex.getStatus());
    }

    @Test
    @DisplayName("addOrUpdateTopo — name 为 null 抛 SNCException")
    void testAddTopoNameNull() {
        TopoData t = new TopoData();
        t.setDevices(new HashMap<String, DeviceEntity>());
        SNCException ex = assertThrows(SNCException.class, () -> service.addOrUpdateTopo(t));
        assertEquals(PlanStatus.INVALID_PARAM, ex.getStatus());
    }

    @Test
    @DisplayName("addOrUpdateTopo — devices 为空抛 SNCException")
    void testAddTopoDevicesEmpty() {
        TopoData t = new TopoData();
        t.setName("t");
        SNCException ex = assertThrows(SNCException.class, () -> service.addOrUpdateTopo(t));
        assertEquals(PlanStatus.INVALID_PARAM, ex.getStatus());
    }

    @Test
    @DisplayName("addOrUpdateTopo — devices 为 null 抛 SNCException")
    void testAddTopoDevicesNull() {
        TopoData t = new TopoData();
        t.setName("t");
        SNCException ex = assertThrows(SNCException.class, () -> service.addOrUpdateTopo(t));
        assertEquals(PlanStatus.INVALID_PARAM, ex.getStatus());
    }

    @Test
    @DisplayName("findTopo — 找到返回")
    void testFindTopoSuccess() {
        service.addOrUpdateTopo(topo);
        TopoData result = service.findTopo("mytopo");
        assertNotNull(result);
        assertEquals("mytopo", result.getName());
    }

    @Test
    @DisplayName("findTopo — null name 抛异常")
    void testFindTopoNull() {
        SNCException ex = assertThrows(SNCException.class, () -> service.findTopo(null));
        assertEquals(PlanStatus.INVALID_PARAM, ex.getStatus());
    }

    @Test
    @DisplayName("findTopo — empty name 抛异常")
    void testFindTopoEmpty() {
        SNCException ex = assertThrows(SNCException.class, () -> service.findTopo(""));
        assertEquals(PlanStatus.INVALID_PARAM, ex.getStatus());
    }

    @Test
    @DisplayName("findTopo — 不存在抛 TOPO_NOT_FOUND")
    void testFindTopoNotFound() {
        SNCException ex = assertThrows(SNCException.class, () -> service.findTopo("nonexistent"));
        assertEquals(PlanStatus.TOPO_NOT_FOUND, ex.getStatus());
    }

    @Test
    @DisplayName("removeTopo — 成功删除")
    void testRemoveTopoSuccess() {
        service.addOrUpdateTopo(topo);
        service.removeTopo("mytopo");
        assertFalse(service.containsTopo("mytopo"));
        assertEquals(0, service.getTopoCount());
    }

    @Test
    @DisplayName("removeTopo — null name 抛异常")
    void testRemoveTopoNull() {
        SNCException ex = assertThrows(SNCException.class, () -> service.removeTopo(null));
        assertEquals(PlanStatus.INVALID_PARAM, ex.getStatus());
    }

    @Test
    @DisplayName("removeTopo — empty name 抛异常")
    void testRemoveTopoEmpty() {
        SNCException ex = assertThrows(SNCException.class, () -> service.removeTopo(""));
        assertEquals(PlanStatus.INVALID_PARAM, ex.getStatus());
    }

    @Test
    @DisplayName("removeTopo — 不存在抛 TOPO_NOT_FOUND")
    void testRemoveTopoNotFound() {
        SNCException ex = assertThrows(SNCException.class, () -> service.removeTopo("nonexistent"));
        assertEquals(PlanStatus.TOPO_NOT_FOUND, ex.getStatus());
    }

    @Test
    @DisplayName("containsTopo — null 返回 false")
    void testContainsTopoNull() {
        assertFalse(service.containsTopo(null));
    }

    @Test
    @DisplayName("getTopoCount — 返回数量")
    void testGetTopoCount() {
        assertEquals(0, service.getTopoCount());
        service.addOrUpdateTopo(topo);
        assertEquals(1, service.getTopoCount());
    }

    @Test
    @DisplayName("clearAll — 清空")
    void testClearAll() {
        service.addOrUpdateTopo(topo);
        service.clearAll();
        assertEquals(0, service.getTopoCount());
    }
}