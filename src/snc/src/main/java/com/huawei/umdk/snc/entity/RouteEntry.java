/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc route entry module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc route entry module
 */
package com.huawei.umdk.snc.entity;

import java.util.List;
import java.util.ArrayList;

/**
 * 路由条目实体
 */
public class RouteEntry {
    /** 目标前缀 -- 32 bit -- 必填字段 */
    private String prefix;
    /** 出端口信息列表 -- 支持多出端口（ECMP），必填字段；各元素包含 outInterface / maskLength / nextHop / preference / tag / protocol */
    private List<OutPortInfo> outPortInfos;

    public RouteEntry() {
        this.outPortInfos = new ArrayList<>();
    }

    public RouteEntry(String prefix, List<OutPortInfo> outPortInfos) {
        this.prefix = prefix;
        this.outPortInfos = outPortInfos;
    }

    public String getPrefix() {
        return prefix;
    }

    public void setPrefix(String prefix) {
        this.prefix = prefix;
    }

    public List<OutPortInfo> getOutPortInfos() {
        return outPortInfos;
    }

    public void setOutPortInfos(List<OutPortInfo> outPortInfos) {
        this.outPortInfos = outPortInfos;
    }

    @Override
    public String toString() {
        return "RouteEntry{" +
               "prefix='" + prefix + '\'' +
               ", outPortInfos=" + outPortInfos +
               '}';
    }
}