/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: SNC (Supernode Network Controller) service
 * Create: 2026-07-07
 * Note:
 * History: 2026-07-07  Create File
 */
package com.huawei.umdk.snc.store;

import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

import com.huawei.umdk.snc.config.Logger;
import com.huawei.umdk.snc.entity.AclData;
import com.huawei.umdk.snc.entity.AclKey;
import com.huawei.umdk.snc.entity.TpAclEntity;

public class AclStore {

    private static final Logger LOG = new Logger(AclStore.class);

    private Map<String, Map<AclKey, TpAclEntity>> store;

    public void init() {
        this.store = new ConcurrentHashMap<>();
        LOG.info("init: AclStore initialized");
    }

    public void replace(AclData aclData) {
        LOG.info("replace: replacing ACL data for SuperNode %s, tpAcl count=%d",
            aclData.getSuperNodeName(), aclData.getTpAcls() != null ? aclData.getTpAcls().size() : 0);
        Map<AclKey, TpAclEntity> internalMap = new ConcurrentHashMap<>();
        if (aclData.getTpAcls() != null) {
            internalMap.putAll(aclData.getTpAcls());
        }
        store.put(aclData.getSuperNodeName(), internalMap);
    }

    public void clear() {
        if (store != null) {
            store.clear();
        }
        LOG.info("clear: AclStore cleared");
    }

    public void removeAclData(String superNodeName) {
        LOG.info("removeAclData: %s", superNodeName);
        if (store != null) {
            store.remove(superNodeName);
        }
    }

    public AclData getAclData(String superNodeName) {
        if (store == null) {
            LOG.warn("getAclData: store is null, returning null for %s", superNodeName);
            return null;
        }
        Map<AclKey, TpAclEntity> internalMap = store.get(superNodeName);
        if (internalMap == null) {
            LOG.debug("getAclData: no ACL data found for %s", superNodeName);
            return null;
        }
        LOG.debug("getAclData: found ACL data for %s, rule count=%d", superNodeName, internalMap.size());
        return new AclData(superNodeName, internalMap);
    }

    public void addAclRule(String superNodeName, AclKey key, TpAclEntity entity) {
        Map<AclKey, TpAclEntity> internalMap = store.get(superNodeName);
        if (internalMap == null) {
            LOG.error("addAclRule: ACL data not found for superNode: %s. Call setAclData() first.", superNodeName);
            throw new IllegalStateException(
                "ACL data not found for superNode: " + superNodeName + ". Call setAclData() first.");
        }
        internalMap.put(key, entity);
        LOG.info("addAclRule: %s, key=%s", superNodeName, key);
    }

    public void removeAclRule(String superNodeName, AclKey key) {
        Map<AclKey, TpAclEntity> internalMap = store.get(superNodeName);
        if (internalMap == null) {
            LOG.error("removeAclRule: ACL data not found for superNode: %s. Call setAclData() first.", superNodeName);
            throw new IllegalStateException(
                "ACL data not found for superNode: " + superNodeName + ". Call setAclData() first.");
        }
        internalMap.remove(key);
        LOG.info("removeAclRule: %s, key=%s", superNodeName, key);
    }
}
