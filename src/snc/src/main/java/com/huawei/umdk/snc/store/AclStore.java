package com.huawei.umdk.snc.store;

import java.util.HashMap;
import java.util.Map;

import com.huawei.umdk.snc.entity.AclData;
import com.huawei.umdk.snc.entity.AclKey;
import com.huawei.umdk.snc.entity.TpAclEntity;

public class AclStore {

    private Map<String, AclData> store;

    public void init() {
        this.store = new HashMap<>();
    }

    public void replace(AclData aclData) {
        store.put(aclData.getSuperNodeName(), aclData);
    }

    public void clear() {
        if (store != null) {
            store.clear();
        }
    }

    public void removeAclData(String superNodeName) {
        store.remove(superNodeName);
    }

    public AclData getAclData(String superNodeName) {
        return store.get(superNodeName);
    }

    public void addAclRule(String superNodeName, AclKey key, TpAclEntity entity) {
        AclData aclData = store.get(superNodeName);
        if (aclData != null && aclData.getTpAcls() != null) {
            aclData.getTpAcls().put(key, entity);
        }
    }

    public void removeAclRule(String superNodeName, AclKey key) {
        AclData aclData = store.get(superNodeName);
        if (aclData != null && aclData.getTpAcls() != null) {
            aclData.getTpAcls().remove(key);
        }
    }
}
