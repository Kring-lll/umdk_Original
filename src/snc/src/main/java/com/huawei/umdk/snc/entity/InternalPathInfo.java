package com.huawei.umdk.snc.entity;

import java.util.List;

/**
 * 内部路径信息
 *
 * 设计来源：内部数据结构，含完整的拓扑连接信息，
 * 比对外 PathPlanResult 多 cna/eid/connectedDevice/connectedPort 等字段。
 */
public class InternalPathInfo {
    /** 路径逐跳信息 */
    private List<InternalPathHop> hops;

    /** 源 EID */
    private String sourceEid;

    /** 目的 EID */
    private String destEid;

    /** 源 CNA */
    private String sourceCna;

    /** 目的 CNA */
    private String destCna;

    /** 总跳数 */
    private int hopCount;

    public InternalPathInfo() {
    }

    public List<InternalPathHop> getHops() {
        return hops;
    }

    public void setHops(List<InternalPathHop> hops) {
        this.hops = hops;
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

    public int getHopCount() {
        return hopCount;
    }

    public void setHopCount(int hopCount) {
        this.hopCount = hopCount;
    }

    @Override
    public String toString() {
        return "InternalPathInfo{" +
               "sourceEid='" + sourceEid + '\'' +
               ", destEid='" + destEid + '\'' +
               ", sourceCna='" + sourceCna + '\'' +
               ", destCna='" + destCna + '\'' +
               ", hopCount=" + hopCount +
               ", hopsCount=" + (hops != null ? hops.size() : 0) +
               '}';
    }
}