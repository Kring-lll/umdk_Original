/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc topo store module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc topo store module
 */
package com.huawei.umdk.snc.store;

import com.huawei.umdk.snc.entity.TopoData;

import java.util.*;
import java.util.concurrent.ConcurrentHashMap;

/**
 * 拓扑数据存储
 *
 * 职责：
 * - 管理多个拓扑（keyed by topoName）
 * - 线程安全（ConcurrentHashMap）
 * - 提供增/删/查操作
 */
public class TopoStore {

    /** topoName -> TopoData */
    private final Map<String, TopoData> store = new ConcurrentHashMap<>();

    /**
     * 新增或更新拓扑
     */
    public void addOrUpdate(TopoData topoData) {
        if (topoData == null || topoData.getName() == null) {
            throw new IllegalArgumentException("TopoData and topoName must not be null");
        }
        store.put(topoData.getName(), topoData);
    }

    /**
     * 根据拓扑名称查找
     */
    public TopoData find(String topoName) {
        if (topoName == null) return null;
        return store.get(topoName);
    }

    /**
     * 删除拓扑
     */
    public TopoData remove(String topoName) {
        if (topoName == null) return null;
        return store.remove(topoName);
    }

    /**
     * 判断拓扑是否存在
     */
    public boolean contains(String topoName) {
        if (topoName == null) return false;
        return store.containsKey(topoName);
    }

    /**
     * 获取所有拓扑名称
     */
    public int size() {
        return store.size();
    }

    /**
     * 清空所有拓扑
     */
    public void clear() {
        store.clear();
    }

    /**
     * 获取所有拓扑数据
     */
    public Collection<TopoData> getAll() {
        return Collections.unmodifiableCollection(store.values());
    }

    /**
     * 获取所有拓扑名称
     */
    public Set<String> getNames() {
        return Collections.unmodifiableSet(store.keySet());
    }
}
