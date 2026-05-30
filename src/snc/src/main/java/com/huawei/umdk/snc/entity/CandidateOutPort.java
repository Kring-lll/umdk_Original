/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc candidate out port module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc candidate out port module
 */
package com.huawei.umdk.snc.entity;

/**
 * 候选出接口
 *
 * 记录单个候选出接口及其是否被选中（ECMP 选路结果）。
 */
public class CandidateOutPort {
    /** 出接口名称，取自 OutPortInfo.outInterface */
    private String outInterface;

    /** 下一跳 IP，取自 OutPortInfo.nextHop */
    private String nextHop;

    /** 是否为选中的出接口（ECMP 选路结果） */
    private boolean selected;

    public CandidateOutPort() {
    }

    public CandidateOutPort(String outInterface, String nextHop, boolean selected) {
        this.outInterface = outInterface;
        this.nextHop = nextHop;
        this.selected = selected;
    }

    public String getOutInterface() {
        return outInterface;
    }

    public void setOutInterface(String outInterface) {
        this.outInterface = outInterface;
    }

    public String getNextHop() {
        return nextHop;
    }

    public void setNextHop(String nextHop) {
        this.nextHop = nextHop;
    }

    public boolean isSelected() {
        return selected;
    }

    public void setSelected(boolean selected) {
        this.selected = selected;
    }

    @Override
    public String toString() {
        return "CandidateOutPort{" +
               "outInterface='" + outInterface + '\'' +
               ", nextHop='" + nextHop + '\'' +
               ", selected=" + selected +
               '}';
    }
}