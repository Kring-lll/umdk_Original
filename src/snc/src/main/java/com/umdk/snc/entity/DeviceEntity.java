/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc device entity module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc device entity module
 */
package com.huawei.umdk.snc.entity;

import java.util.List;

/**
 * 设备实体
 */
public class DeviceEntity {
    /** 设备唯一标识，格式：rack#os#npu 或 rack#la0 或 lc#0 -- 必填字段 */
    private String deviceId;
    /** 设备类型 -- 必填字段 */
    private DeviceType deviceType;
    /** 管理IP -- 必填字段 */
    private String ip;
    /** 所属Rack */
    private String rack;
    /** OS域(仅NPU设备有) */
    private Integer osZone;
    /** 模组信息(仅NPU设备有) */
    private Integer moduleidx;
    /** 相对编号，NPU为模组内，LA是框内，LC是超节点 */
    private Integer index;
    /** 端口列表 -- 必填字段 */
    private List<PortEntity> ports;
    /** 逻辑端口列表（聚合端口） */
    private List<LogicPortEntity> logicPorts;
    /** 路由器列表 -- 必填字段 */
    private List<RouterTable> routerTables;

    public DeviceEntity() {
    }

    public String getDeviceId() {
        return deviceId;
    }

    public void setDeviceId(String deviceId) {
        this.deviceId = deviceId;
    }

    public DeviceType getDeviceType() {
        return deviceType;
    }

    public void setDeviceType(DeviceType deviceType) {
        this.deviceType = deviceType;
    }

    public String getIp() {
        return ip;
    }

    public void setIp(String ip) {
        this.ip = ip;
    }

    public String getRack() {
        return rack;
    }

    public void setRack(String rack) {
        this.rack = rack;
    }

    public Integer getOsZone() {
        return osZone;
    }

    public void setOsZone(Integer osZone) {
        this.osZone = osZone;
    }

    public Integer getModuleidx() {
        return moduleidx;
    }

    public void setModuleidx(Integer moduleidx) {
        this.moduleidx = moduleidx;
    }

    public Integer getIndex() {
        return index;
    }

    public void setIndex(Integer index) {
        this.index = index;
    }

    public List<PortEntity> getPorts() {
        return ports;
    }

    public void setPorts(List<PortEntity> ports) {
        this.ports = ports;
    }

    public List<LogicPortEntity> getLogicPorts() {
        return logicPorts;
    }

    public void setLogicPorts(List<LogicPortEntity> logicPorts) {
        this.logicPorts = logicPorts;
    }

    public List<RouterTable> getRouterTables() {
        return routerTables;
    }

    public void setRouterTables(List<RouterTable> routerTables) {
        this.routerTables = routerTables;
    }

    /**
     * 根据端口名称查找物理端口，遍历 O(n)
     */
    public PortEntity findPort(String portName) {
        if (ports == null || portName == null) {
            return null;
        }
        for (PortEntity port : ports) {
            if (portName.equals(port.getName())) {
                return port;
            }
        }
        return null;
    }

    /**
     * 根据逻辑端口名称查找逻辑端口，遍历 O(n)
     */
    public LogicPortEntity findLogicPort(String logicPortName) {
        if (logicPorts == null || logicPortName == null) {
            return null;
        }
        for (LogicPortEntity lp : logicPorts) {
            if (logicPortName.equals(lp.getName())) {
                return lp;
            }
        }
        return null;
    }

    /**
     * 根据 instance 查找路由表，遍历 O(n)
     */
    public RouterTable findRouterTable(Integer instance) {
        if (routerTables == null || instance == null) {
            return null;
        }
        for (RouterTable rt : routerTables) {
            if (instance.equals(rt.getInstance())) {
                return rt;
            }
        }
        return null;
    }

    @Override
    public String toString() {
        return "DeviceEntity{" +
               "deviceId='" + deviceId + '\'' +
               ", deviceType=" + deviceType +
               ", ip='" + ip + '\'' +
               ", rack='" + rack + '\'' +
               ", portsCount=" + (ports != null ? ports.size() : 0) +
               '}';
    }
}