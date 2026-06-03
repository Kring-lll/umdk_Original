/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc tp acl entity module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc tp acl entity module
 */
package com.huawei.umdk.snc.entity;

/**
 * TP-ACL 实体
 *
 * ACL校验规则：
 * - 路径规划时，使用 (sourceEid, destEid, transportType) 在 HashMap 中查找
 * - 查找到表项后，验证 sourceAddress.cna 与源设备端口 CNA 一致，
 *   destAddress.cna 与目的设备端口 CNA 一致
 */
public class TpAclEntity {
    /** 源EID -- 128 bit -- 必填字段 */
    private String sourceEid;
    /** 目的EID -- 128 bit -- 必填字段 */
    private String destEid;
    /** 传输类型 -- 必填字段 */
    private TransportType transportType;
    /** 源地址（只支持CNA）-- 24 bit -- 必填字段 */
    private String sourceCna;
    /** 目的地址（只支持CNA）-- 24 bit -- 必填字段 */
    private String destCna;
    /** 模板ID (1-19) */
    private Integer templateId;

    public TpAclEntity() {
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

    public String getSourceCna() {
        return sourceCna;
    }

    public void setSourceCna(String sourceCna) {
        this.sourceCna = sourceCna;
    }

    public String getDestCna() {
        return destCna;
    }

    public void setDestCna(String destCna) {
        this.destCna = destCna;
    }

    public Integer getTemplateId() {
        return templateId;
    }

    public void setTemplateId(Integer templateId) {
        this.templateId = templateId;
    }

    /**
     * 生成 AclKey
     */
    public AclKey toKey() {
        return new AclKey(sourceEid, destEid, transportType);
    }

    @Override
    public String toString() {
        return "TpAclEntity{" +
               "sourceEid='" + sourceEid + '\'' +
               ", destEid='" + destEid + '\'' +
               ", transportType=" + transportType +
                ", sourceCna='" + sourceCna + '\'' +
                ", destCna='" + destCna + '\'' +
               '}';
    }
}