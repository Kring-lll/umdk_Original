/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc acl store module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc acl store module
 */
package com.huawei.umdk.snc.store;

import com.huawei.umdk.snc.entity.AclData;

import java.util.*;
import java.util.concurrent.ConcurrentHashMap;

/**
 * ACL数据存储
 *
 * 职责：
 * - 管理多个 ACL 规则集（keyed by aclId）
 * - 线程安全（ConcurrentHashMap）
 * - 提供增/删/查操作
 */
public class AclStore {

    /** aclId -> AclData */
    private final Map<String, AclData> store = new ConcurrentHashMap<>();

    /**
     * 新增或更新 ACL 规则集
     */
    public void addOrUpdate(AclData aclData) {
        if (aclData == null || aclData.getAclId() == null) {
            throw new IllegalArgumentException("AclData and aclId must not be null");
        }
        store.put(aclData.getAclId(), aclData);
    }

    /**
     * 根据 aclId 查找
     */
    public AclData find(String aclId) {
        if (aclId == null) return null;
        return store.get(aclId);
    }

    /**
     * 返回任意一个已加载的 AclData（用于简单场景）
     */
    public AclData findAny() {
        for (AclData d : store.values()) {
            if (d != null) return d;
        }
        return null;
    }

    /**
     * 删除 ACL 规则集
     */
    public AclData remove(String aclId) {
        if (aclId == null) return null;
        return store.remove(aclId);
    }

    /**
     * 判断 ACL 规则集是否存在
     */
    public boolean contains(String aclId) {
        if (aclId == null) return false;
        return store.containsKey(aclId);
    }

    /**
     * 获取 ACL 规则集数量
     */
    public int size() {
        return store.size();
    }

    /**
     * 清空所有 ACL 规则集
     */
    public void clear() {
        store.clear();
    }

    /**
     * 获取所有 ACL 数据
     */
    public Collection<AclData> getAll() {
        return Collections.unmodifiableCollection(store.values());
    }

    /**
     * 获取所有 ACL ID
     */
    public Set<String> getIds() {
        return Collections.unmodifiableSet(store.keySet());
    }
}
