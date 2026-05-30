/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc path info module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc path info module
 */
package com.huawei.umdk.snc.entity;

import java.util.List;

/**
 * 路径信息
 */
public class PathInfo {
    /** 中间设备列表 */
    private List<HopInfo> hops;

    public PathInfo() {
    }

    public List<HopInfo> getHops() {
        return hops;
    }

    public void setHops(List<HopInfo> hops) {
        this.hops = hops;
    }

    @Override
    public String toString() {
        return "PathInfo{" +
               "hopsCount=" + (hops != null ? hops.size() : 0) +
               '}';
    }
}
