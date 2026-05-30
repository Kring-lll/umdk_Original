/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc route table module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc route table module
 */
package com.huawei.umdk.snc.entity;

import java.util.Map;

/**
 * 路由表 - 按 instance 索引，内含 prefix→RouteEntry 的 HashMap
 */
public class RouterTable {
    /** 所属实例ID */
    private Integer instance;
    /** 路由条目Map -- Map的key为prefix */
    private Map<String, RouteEntry> routes;

    public RouterTable() {
    }

    public Integer getInstance() {
        return instance;
    }

    public void setInstance(Integer instance) {
        this.instance = instance;
    }

    public Map<String, RouteEntry> getRoutes() {
        return routes;
    }

    public void setRoutes(Map<String, RouteEntry> routes) {
        this.routes = routes;
    }

    /**
     * 根据 prefix 查找路由条目，O(1)
     */
    public RouteEntry findRoute(String prefix) {
        if (routes == null) {
            return null;
        }
        return routes.get(prefix);
    }

    @Override
    public String toString() {
        return "RouterTable{" +
               "instance=" + instance +
               ", routesSize=" + (routes != null ? routes.size() : 0) +
               '}';
    }
}