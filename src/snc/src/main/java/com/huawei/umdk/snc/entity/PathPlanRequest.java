package com.huawei.umdk.snc.entity;

import java.util.Map;

/**
 * 路径查询请求
 */
public class PathPlanRequest {
    /** 源物理端口名称 */
    private String srcPort;

    /** 目的物理端口名称 */
    private String destPort;

    /** 源设备 deviceId */
    private String srcDevice;

    /** 目的设备 deviceId */
    private String destDevice;

    /** 中间设备及出端口列表 -- Map<设备ID, 出端口名称> */
    private Map<String, String> interDevices;

    public PathPlanRequest() {
    }

    public String getSrcPort() {
        return srcPort;
    }

    public void setSrcPort(String srcPort) {
        this.srcPort = srcPort;
    }

    public String getDestPort() {
        return destPort;
    }

    public void setDestPort(String destPort) {
        this.destPort = destPort;
    }

    public String getSrcDevice() {
        return srcDevice;
    }

    public void setSrcDevice(String srcDevice) {
        this.srcDevice = srcDevice;
    }

    public String getDestDevice() {
        return destDevice;
    }

    public void setDestDevice(String destDevice) {
        this.destDevice = destDevice;
    }

    public Map<String, String> getInterDevices() {
        return interDevices;
    }

    public void setInterDevices(Map<String, String> interDevices) {
        this.interDevices = interDevices;
    }

    @Override
    public String toString() {
        return "PathPlanRequest{" +
               "srcDevice='" + srcDevice + '\'' +
               ", srcPort='" + srcPort + '\'' +
               ", destDevice='" + destDevice + '\'' +
               ", destPort='" + destPort + '\'' +
               ", interDevices=" + interDevices +
               '}';
    }
}