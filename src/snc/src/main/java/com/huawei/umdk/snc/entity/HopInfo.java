package com.huawei.umdk.snc.entity;

/**
 * 跳步信息
 *
 * 字段约束：
 * - 源节点：只有 outPort，无 inPort
 * - 目的节点：只有 inPort，无 outPort
 * - 中间节点：两者皆有
 */
public class HopInfo {
    /** 设备ID */
    private String deviceId;

    /** 入端口 — 目的节点和中间节点一定有 */
    private String inPort;

    /** 出端口 — 源节点和中间节点一定有 */
    private String outPort;

    /** 多路径使能 */
    private Boolean multiPath;

    /** 设备类型 */
    private DeviceType deviceType;

    public HopInfo() {
    }

    public HopInfo(String deviceId, String inPort, String outPort, DeviceType deviceType) {
        this.deviceId = deviceId;
        this.inPort = inPort;
        this.outPort = outPort;
        this.deviceType = deviceType;
    }

    public String getDeviceId() {
        return deviceId;
    }

    public void setDeviceId(String deviceId) {
        this.deviceId = deviceId;
    }

    public String getInPort() {
        return inPort;
    }

    public void setInPort(String inPort) {
        this.inPort = inPort;
    }

    public String getOutPort() {
        return outPort;
    }

    public void setOutPort(String outPort) {
        this.outPort = outPort;
    }

    public Boolean getMultiPath() {
        return multiPath;
    }

    public void setMultiPath(Boolean multiPath) {
        this.multiPath = multiPath;
    }

    public DeviceType getDeviceType() {
        return deviceType;
    }

    public void setDeviceType(DeviceType deviceType) {
        this.deviceType = deviceType;
    }

    @Override
    public String toString() {
        return "HopInfo{" +
               "deviceId='" + deviceId + '\'' +
               ", inPort='" + inPort + '\'' +
               ", outPort='" + outPort + '\'' +
               ", multiPath=" + multiPath +
               ", deviceType=" + deviceType +
               '}';
    }
}