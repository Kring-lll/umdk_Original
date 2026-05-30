/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc route selection record module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc route selection record module
 */
package com.huawei.umdk.snc.entity;

import java.util.List;

/**
 * 内部选路记录
 *
 * 设计来源：出端口判断与选路记录。
 * 用于记录路由查找循环中每一步的选路信息，后续消费以计算 UDP 端口。
 */
public class RouteSelectionRecord {
    /** 当前查找设备 */
    private String deviceId;

    /** 查找的目的前缀 */
    private String prefix;

    /** 候选出端口列表 */
    private List<CandidateOutPort> candidateOutPorts;

    /** 源 CNA */
    private String scna;

    /** 目的 CNA */
    private String dcna;

    /** 哈希信息（用于 ECMP 分流） */
    private String hashInfo;

    /** 查找方向 */
    private Direction direction;

    public RouteSelectionRecord() {
    }

    public String getDeviceId() {
        return deviceId;
    }

    public void setDeviceId(String deviceId) {
        this.deviceId = deviceId;
    }

    public String getPrefix() {
        return prefix;
    }

    public void setPrefix(String prefix) {
        this.prefix = prefix;
    }

    public List<CandidateOutPort> getCandidateOutPorts() {
        return candidateOutPorts;
    }

    public void setCandidateOutPorts(List<CandidateOutPort> candidateOutPorts) {
        this.candidateOutPorts = candidateOutPorts;
    }

    public String getScna() {
        return scna;
    }

    public void setScna(String scna) {
        this.scna = scna;
    }

    public String getDcna() {
        return dcna;
    }

    public void setDcna(String dcna) {
        this.dcna = dcna;
    }

    public String getHashInfo() {
        return hashInfo;
    }

    public void setHashInfo(String hashInfo) {
        this.hashInfo = hashInfo;
    }

    public Direction getDirection() {
        return direction;
    }

    public void setDirection(Direction direction) {
        this.direction = direction;
    }

    @Override
    public String toString() {
        return "RouteSelectionRecord{" +
               "deviceId='" + deviceId + '\'' +
               ", prefix='" + prefix + '\'' +
               ", candidateCount=" + (candidateOutPorts != null ? candidateOutPorts.size() : 0) +
               ", direction=" + direction +
               '}';
    }

    /**
     * 选路方向
     */
    public enum Direction {
        /** 正向查找（从源向目的） */
        FORWARD,
        /** 反向查找（从目的向源） */
        REVERSE
    }
}