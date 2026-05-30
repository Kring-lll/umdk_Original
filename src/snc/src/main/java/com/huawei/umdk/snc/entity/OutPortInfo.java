/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc out port info module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc out port info module
 */
package com.huawei.umdk.snc.entity;

/**
 * 出端口信息 -- 包含出接口名称及路由元数据
 */
public class OutPortInfo {
    /** 出接口名称 -- 必填字段 */
    private String outInterface;
    /** 掩码长度(0-32) */
    private Integer maskLength;
    /** 下一跳IP */
    private String nextHop;
    /** 路由优先级(1-255，默认60) */
    private Integer preference;
    /** 路由标签 */
    private Integer tag;
    /** 路由协议类型 */
    private String protocol;

    public OutPortInfo() {
    }

    public OutPortInfo(String outInterface, Integer maskLength, String nextHop,
                       Integer preference, Integer tag, String protocol) {
        this.outInterface = outInterface;
        this.maskLength = maskLength;
        this.nextHop = nextHop;
        this.preference = preference;
        this.tag = tag;
        this.protocol = protocol;
    }

    public String getOutInterface() {
        return outInterface;
    }

    public void setOutInterface(String outInterface) {
        this.outInterface = outInterface;
    }

    public Integer getMaskLength() {
        return maskLength;
    }

    public void setMaskLength(Integer maskLength) {
        this.maskLength = maskLength;
    }

    public String getNextHop() {
        return nextHop;
    }

    public void setNextHop(String nextHop) {
        this.nextHop = nextHop;
    }

    public Integer getPreference() {
        return preference;
    }

    public void setPreference(Integer preference) {
        this.preference = preference;
    }

    public Integer getTag() {
        return tag;
    }

    public void setTag(Integer tag) {
        this.tag = tag;
    }

    public String getProtocol() {
        return protocol;
    }

    public void setProtocol(String protocol) {
        this.protocol = protocol;
    }

    @Override
    public String toString() {
        return "OutPortInfo{" +
               "outInterface='" + outInterface + '\'' +
               ", maskLength=" + maskLength +
               ", nextHop='" + nextHop + '\'' +
               ", preference=" + preference +
               ", tag=" + tag +
               ", protocol='" + protocol + '\'' +
               '}';
    }
}