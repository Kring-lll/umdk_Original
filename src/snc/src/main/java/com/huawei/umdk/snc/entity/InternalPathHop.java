/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc internal path hop module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc internal path hop module
 */
package com.huawei.umdk.snc.entity;

/**
 * 内部路径跳步信息
 *
 * 与对外 HopInfo 的差异：多了 cna、eid、connectedDevice、connectedPort 等拓扑内部字段。
 */
public class InternalPathHop {
    /** 设备ID */
    private String deviceId;

    /** 设备类型 */
    private DeviceType deviceType;

    /** 入端口 */
    private String inPort;

    /** 出端口 */
    private String outPort;

    /** 该设备在该跳上的 CNA */
    private String cna;

    /** 该设备在该跳上的 EID */
    private String eid;

    /** 该端口直连的对端设备 */
    private String connectedDevice;

    /** 该端口直连的对端端口 */
    private String connectedPort;

    /** 该设备所在机架 */
    private String rack;

    /** 跳步索引（从 0 开始） */
    private int hopIndex;

    public InternalPathHop() {
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

    public String getCna() {
        return cna;
    }

    public void setCna(String cna) {
        this.cna = cna;
    }

    public String getEid() {
        return eid;
    }

    public void setEid(String eid) {
        this.eid = eid;
    }

    public String getConnectedDevice() {
        return connectedDevice;
    }

    public void setConnectedDevice(String connectedDevice) {
        this.connectedDevice = connectedDevice;
    }

    public String getConnectedPort() {
        return connectedPort;
    }

    public void setConnectedPort(String connectedPort) {
        this.connectedPort = connectedPort;
    }

    public String getRack() {
        return rack;
    }

    public void setRack(String rack) {
        this.rack = rack;
    }

    public int getHopIndex() {
        return hopIndex;
    }

    public void setHopIndex(int hopIndex) {
        this.hopIndex = hopIndex;
    }

    @Override
    public String toString() {
        return "InternalPathHop{" +
               "deviceId='" + deviceId + '\'' +
               ", deviceType=" + deviceType +
               ", inPort='" + inPort + '\'' +
               ", outPort='" + outPort + '\'' +
               ", cna='" + cna + '\'' +
               ", eid='" + eid + '\'' +
               ", hopIndex=" + hopIndex +
               '}';
    }
}