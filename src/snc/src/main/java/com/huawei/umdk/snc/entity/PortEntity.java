/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc port entity module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc port entity module
 */
package com.huawei.umdk.snc.entity;

/**
 * 端口实体（物理端口）
 */
public class PortEntity {
    /** 端口名称，如 "400GE 0/0/1" -- 必填字段 */
    private String name;
    /** 端口ID */
    private Integer id;
    /** 所属实例ID */
    private Integer instance;
    /** 连接的设备 -- 必填字段 */
    private String connectedDevice;
    /** 连接的端口 -- 必填字段 */
    private String connectedPort;
    /** 关联的CNA -- 24 bit -- 必填字段 */
    private String cna;
    /** 关联的EID -- 128 bit -- 必填字段 */
    private String eid;

    public PortEntity() {
    }

    public String getName() {
        return name;
    }

    public void setName(String name) {
        this.name = name;
    }

    public Integer getId() {
        return id;
    }

    public void setId(Integer id) {
        this.id = id;
    }

    public Integer getInstance() {
        return instance;
    }

    public void setInstance(Integer instance) {
        this.instance = instance;
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

    @Override
    public String toString() {
        return "PortEntity{" +
               "name='" + name + '\'' +
               ", connectedDevice='" + connectedDevice + '\'' +
               ", connectedPort='" + connectedPort + '\'' +
               ", cna='" + cna + '\'' +
               ", eid='" + eid + '\'' +
               '}';
    }
}