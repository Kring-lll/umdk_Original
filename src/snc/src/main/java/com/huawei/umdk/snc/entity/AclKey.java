/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc acl key module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc acl key module
 */
package com.huawei.umdk.snc.entity;

import java.util.Objects;

/**
 * ACL复合键 - (sourceEid + destEid + transportType) 联合唯一标识一条ACL规则
 */
public class AclKey {
    /** 源EID -- 128 bit */
    private String sourceEid;
    /** 目的EID -- 128 bit */
    private String destEid;
    /** 传输类型 */
    private TransportType transportType;

    public AclKey() {
    }

    public AclKey(String sourceEid, String destEid, TransportType transportType) {
        this.sourceEid = sourceEid;
        this.destEid = destEid;
        this.transportType = transportType;
    }

    public String getSourceEid() {
        return sourceEid;
    }

    public void setSourceEid(String sourceEid) {
        this.sourceEid = sourceEid;
    }

    public String getDestEid() {
        return destEid;
    }

    public void setDestEid(String destEid) {
        this.destEid = destEid;
    }

    public TransportType getTransportType() {
        return transportType;
    }

    public void setTransportType(TransportType transportType) {
        this.transportType = transportType;
    }

    @Override
    public boolean equals(Object o) {
        if (this == o) return true;
        if (o == null || getClass() != o.getClass()) return false;
        AclKey aclKey = (AclKey) o;
        return Objects.equals(sourceEid, aclKey.sourceEid) &&
               Objects.equals(destEid, aclKey.destEid) &&
               transportType == aclKey.transportType;
    }

    @Override
    public int hashCode() {
        return Objects.hash(sourceEid, destEid, transportType);
    }

    @Override
    public String toString() {
        return "AclKey{" +
               "sourceEid='" + sourceEid + '\'' +
               ", destEid='" + destEid + '\'' +
               ", transportType=" + transportType +
               '}';
    }
}