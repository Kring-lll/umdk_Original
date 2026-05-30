package com.huawei.umdk.snc.entity;

import java.util.List;

/**
 * 逻辑端口实体（聚合端口，如端口组）
 */
public class LogicPortEntity {
    /** 逻辑端口名称，如 "port_group1" -- 必填字段 */
    private String name;
    /** 关联的CNA -- 24 bit */
    private String cna;
    /** 关联的EID -- 128 bit */
    private String eid;
    /** 包含的物理端口列表 -- 必填字段（物理端口name） */
    private List<String> ports;

    public LogicPortEntity() {
    }

    public String getName() {
        return name;
    }

    public void setName(String name) {
        this.name = name;
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

    public List<String> getPorts() {
        return ports;
    }

    public void setPorts(List<String> ports) {
        this.ports = ports;
    }

    @Override
    public String toString() {
        return "LogicPortEntity{" +
               "name='" + name + '\'' +
               ", cna='" + cna + '\'' +
               ", eid='" + eid + '\'' +
               ", ports=" + ports +
               '}';
    }
}