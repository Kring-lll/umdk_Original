/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc path plan result module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc path plan result module
 */
package com.huawei.umdk.snc.entity;

import java.util.List;

/**
 * 路径规划结果
 */
public class PathPlanResult {
    /** 源EID */
    private String sourceEid;

    /** 目的EID */
    private String destEid;

    /** 路径详情列表 */
    private List<PathInfo> paths;

    /** 查询状态 */
    private PlanStatus status;

    /** 失败原因(如果查询失败) */
    private String errorMessage;

    /** 多路径使能 */
    private Boolean multiPath;

    /** Ack UDP 源端口 */
    private Integer ackUdpSrcPort;

    /** Data UDP 源端口 */
    private Integer dataUdpSrcPort;

    /** Spray 使能 */
    private Boolean spray;

    /** 错误码 — 便于上层精确区分失败原因，成功时为 0 */
    private int errorCode;

    public PathPlanResult() {
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

    public List<PathInfo> getPaths() {
        return paths;
    }

    public void setPaths(List<PathInfo> paths) {
        this.paths = paths;
    }

    public PlanStatus getStatus() {
        return status;
    }

    public void setStatus(PlanStatus status) {
        this.status = status;
    }

    public String getErrorMessage() {
        return errorMessage;
    }

    public void setErrorMessage(String errorMessage) {
        this.errorMessage = errorMessage;
    }

    public Boolean getMultiPath() {
        return multiPath;
    }

    public void setMultiPath(Boolean multiPath) {
        this.multiPath = multiPath;
    }

    public Integer getAckUdpSrcPort() {
        return ackUdpSrcPort;
    }

    public void setAckUdpSrcPort(Integer ackUdpSrcPort) {
        this.ackUdpSrcPort = ackUdpSrcPort;
    }

    public Integer getDataUdpSrcPort() {
        return dataUdpSrcPort;
    }

    public void setDataUdpSrcPort(Integer dataUdpSrcPort) {
        this.dataUdpSrcPort = dataUdpSrcPort;
    }

    public Boolean getSpray() {
        return spray;
    }

    public void setSpray(Boolean spray) {
        this.spray = spray;
    }

    // ---- errorCode getter/setter ----

    public int getErrorCode() {
        return errorCode;
    }

    public void setErrorCode(int errorCode) {
        this.errorCode = errorCode;
    }

    // ==================== 工厂方法 ====================

    public static PathPlanResult success(List<PathInfo> paths) {
        PathPlanResult result = new PathPlanResult();
        result.status = PlanStatus.SUCCESS;
        result.errorCode = 0;
        result.paths = paths;
        return result;
    }

    public static PathPlanResult fail(PlanStatus status, int errorCode, String errorMessage) {
        PathPlanResult result = new PathPlanResult();
        result.status = status;
        result.errorCode = errorCode;
        result.errorMessage = errorMessage;
        return result;
    }

    @Override
    public String toString() {
        return "PathPlanResult{" +
               "status=" + status +
               ", errorCode=" + errorCode +
               ", errorMessage='" + errorMessage + '\'' +
               ", pathsCount=" + (paths != null ? paths.size() : 0) +
               ", sourceEid='" + sourceEid + '\'' +
               ", destEid='" + destEid + '\'' +
               ", multiPath=" + multiPath +
               ", spray=" + spray +
               '}';
    }
}
