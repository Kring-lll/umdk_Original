/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc topo service module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc topo service module
 */
package com.huawei.umdk.snc.service;

import com.huawei.umdk.snc.entity.DeviceEntity;
import com.huawei.umdk.snc.entity.LogicPortEntity;
import com.huawei.umdk.snc.entity.PortEntity;
import com.huawei.umdk.snc.entity.PlanStatus;
import com.huawei.umdk.snc.entity.RouteEntry;
import com.huawei.umdk.snc.entity.RouterTable;
import com.huawei.umdk.snc.entity.TopoData;
import com.huawei.umdk.snc.exception.SNCException;
import com.huawei.umdk.snc.store.TopoStore;
import com.huawei.umdk.snc.util.CNAUtils;
import com.huawei.umdk.snc.util.EIDUtils;
import com.huawei.umdk.snc.util.PrefixUtils;

import java.util.*;

/**
 * 拓扑数据管理服务
 *
 * 职责：
 * - 拓扑数据的增加、查询、删除
 * - 参数校验
 */
public class TopoService {

    private final TopoStore topoStore;

    public TopoService(TopoStore topoStore) {
        this.topoStore = topoStore;
    }

    /**
     * 新增或更新拓扑
     */
    public void addOrUpdateTopo(TopoData topoData) {
        validateTopoData(topoData);
        topoStore.addOrUpdate(topoData);
    }

    /**
     * 查询拓扑
     */
    public TopoData findTopo(String topoName) {
        if (topoName == null || topoName.isEmpty()) {
            throw new SNCException(PlanStatus.INVALID_PARAM, "topoName must not be null or empty");
        }
        TopoData topoData = topoStore.find(topoName);
        if (topoData == null) {
            throw new SNCException(PlanStatus.TOPO_NOT_FOUND, "Topology not found: " + topoName);
        }
        return topoData;
    }

    /**
     * 删除拓扑
     */
    public void removeTopo(String topoName) {
        if (topoName == null || topoName.isEmpty()) {
            throw new SNCException(PlanStatus.INVALID_PARAM, "topoName must not be null or empty");
        }
        TopoData removed = topoStore.remove(topoName);
        if (removed == null) {
            throw new SNCException(PlanStatus.TOPO_NOT_FOUND, "Topology not found: " + topoName);
        }
    }

    /**
     * 判断拓扑是否存在
     */
    public boolean containsTopo(String topoName) {
        return topoName != null && topoStore.contains(topoName);
    }

    /**
     * 获取已加载的拓扑数量
     */
    public int getTopoCount() {
        return topoStore.size();
    }

    /**
     * 清空所有拓扑
     */
    public void clearAll() {
        topoStore.clear();
    }

    /**
     * 列出所有拓扑数据
     */
    public List<TopoData> listAllTopos() {
        return new ArrayList<>(topoStore.getAll());
    }

    /**
     * 列出所有拓扑名称
     */
    public Set<String> listAllTopoNames() {
        return topoStore.getNames();
    }

    private void validateTopoData(TopoData topoData) {
        if (topoData == null) {
            throw new SNCException(PlanStatus.INVALID_PARAM, "TopoData must not be null");
        }
        if (topoData.getName() == null || topoData.getName().isEmpty()) {
            throw new SNCException(PlanStatus.INVALID_PARAM, "TopoData name must not be null or empty");
        }
        if (topoData.getDevices() == null || topoData.getDevices().isEmpty()) {
            throw new SNCException(PlanStatus.INVALID_PARAM, "TopoData devices must not be null or empty");
        }

        // 校验每个设备的 ports 和 logicPorts 的 EID/CNA 格式
        for (DeviceEntity device : topoData.getDevices().values()) {
            validateDeviceFields(device);
        }
    }

    private void validateDeviceFields(DeviceEntity device) {
        // 校验 PortEntity
        if (device.getPorts() != null) {
            for (PortEntity port : device.getPorts()) {
                validatePortField("PortEntity", port.getName(),
                        port.getEid(), port.getCna());
            }
        }
        // 校验 LogicPortEntity
        if (device.getLogicPorts() != null) {
            for (LogicPortEntity lp : device.getLogicPorts()) {
                validatePortField("LogicPortEntity", lp.getName(),
                        lp.getEid(), lp.getCna());
            }
        }
        // 校验 RouterTable 中 RouteEntry 的 prefix 字段
        if (device.getRouterTables() != null) {
            for (RouterTable rt : device.getRouterTables()) {
                validateRouterTable(rt, device.getDeviceId());
            }
        }
    }

    private void validateRouterTable(RouterTable rt, String deviceId) {
        if (rt.getRoutes() == null) {
            return;
        }
        for (Map.Entry<String, RouteEntry> entry : rt.getRoutes().entrySet()) {
            String routePrefix = entry.getKey();
            if (routePrefix != null && !PrefixUtils.isValid(routePrefix)) {
                throw new SNCException(PlanStatus.INVALID_PARAM,
                        "Device[" + deviceId + "] routerTable instance=" + rt.getInstance()
                                + " invalid route prefix (need 32-bit 0x hex): " + routePrefix);
            }
            RouteEntry route = entry.getValue();
            if (route != null && route.getPrefix() != null
                    && !PrefixUtils.isValid(route.getPrefix())) {
                throw new SNCException(PlanStatus.INVALID_PARAM,
                        "Device[" + deviceId + "] routerTable instance=" + rt.getInstance()
                                + " invalid RouteEntry.prefix (need 32-bit 0x hex): " + route.getPrefix());
            }
        }
    }

    private void validatePortField(String entityType, String portName,
                                    String eid, String cna) {
        String prefix = entityType + (portName != null ? "[" + portName + "]" : "");
        // EID 校验（非 null 时必须是 32 位 hex）
        if (eid != null && !EIDUtils.isValid(eid)) {
            throw new SNCException(PlanStatus.INVALID_PARAM,
                    prefix + " invalid EID (need 32 hex chars): " + eid);
        }
        // CNA 校验（非 null 时必须是 24 位 hex，格式 0x...）
        if (cna != null && !CNAUtils.isValid(cna)) {
            throw new SNCException(PlanStatus.INVALID_PARAM,
                    prefix + " invalid CNA (need 24-bit 0x hex): " + cna);
        }
    }
}