/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc acl data module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc acl data module
 */
package com.huawei.umdk.snc.entity;

import java.util.Map;

/**
 * ACL数据容器 - 通过 aclId 索引
 * tpAcls Map的key为 AclKey（复合对象）
 */
public class AclData {
    /** ACL标识，用于删除操作 -- 必填字段 */
    private String aclId;
    /** ACL Map -- 必填字段，Map的key为AclKey（复合对象） */
    private Map<AclKey, TpAclEntity> tpAcls;

    public AclData() {
    }

    public String getAclId() {
        return aclId;
    }

    public void setAclId(String aclId) {
        this.aclId = aclId;
    }

    public Map<AclKey, TpAclEntity> getTpAcls() {
        return tpAcls;
    }

    public void setTpAcls(Map<AclKey, TpAclEntity> tpAcls) {
        this.tpAcls = tpAcls;
    }

    /**
     * 根据 (sourceEid, destEid, transportType) 查找 ACL 规则，O(1)
     */
    public TpAclEntity findAcl(String sourceEid, String destEid, TransportType transportType) {
        if (tpAcls == null) {
            return null;
        }
        return tpAcls.get(new AclKey(sourceEid, destEid, transportType));
    }

    @Override
    public String toString() {
        return "AclData{" +
               "aclId='" + aclId + '\'' +
               ", tpAclsCount=" + (tpAcls != null ? tpAcls.size() : 0) +
               '}';
    }
}