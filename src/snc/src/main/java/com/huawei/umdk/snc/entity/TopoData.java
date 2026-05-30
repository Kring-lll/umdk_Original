package com.huawei.umdk.snc.entity;

import java.util.Map;

/**
 * 拓扑数据 - Map的key为name，devices的key为deviceId
 */
public class TopoData {
    /** 拓扑名称 -- 必填字段 */
    private String name;
    /** 版本号 -- 必填字段 */
    private String version;
    /** 设备Map -- 必填字段，Map的key为deviceId（设备名） */
    private Map<String, DeviceEntity> devices;

    public TopoData() {
    }

    public String getName() {
        return name;
    }

    public void setName(String name) {
        this.name = name;
    }

    public String getVersion() {
        return version;
    }

    public void setVersion(String version) {
        this.version = version;
    }

    public Map<String, DeviceEntity> getDevices() {
        return devices;
    }

    public void setDevices(Map<String, DeviceEntity> devices) {
        this.devices = devices;
    }

    /**
     * 根据 deviceId 查找设备，O(1)
     */
    public DeviceEntity findDevice(String deviceId) {
        if (devices == null || deviceId == null) {
            return null;
        }
        return devices.get(deviceId);
    }

    @Override
    public String toString() {
        return "TopoData{" +
               "name='" + name + '\'' +
               ", version='" + version + '\'' +
               ", devicesCount=" + (devices != null ? devices.size() : 0) +
               '}';
    }
}